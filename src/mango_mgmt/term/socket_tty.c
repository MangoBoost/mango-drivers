// SPDX-License-Identifier: GPL-2.0
/*
 * Socket-backed chardev with terminal ioctl emulation.
 *
 * Captures the kernel's own socket file_operations at init time (via a
 * throwaway socket), then overrides open/release/ioctl.  The inherited
 * read_iter/write_iter/poll run directly against userspace — no bounce
 * buffers, no kernel_recvmsg/kernel_sendmsg reimplementation.
 *
 * open:    creates a TCP socket, connects to the SoC's external IP
 *          (read from the MPIPE NAT struct) on mango_mgmt_term_port,
 *          and stores the raw struct socket * in filp->private_data.
 *          If the netdev / NAT / connect isn't ready, leaves
 *          private_data NULL — mmap-only mode.
 * release: sock_release + NULL private_data.
 * ioctl:   terminal emulation (TCGETS, TIOCMGET, etc.) for minicom
 *          compatibility; unknown ioctls fall through to the socket's
 *          own handler.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/fs.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/termios.h>
#include <linux/sockios.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <asm/ioctls.h>
#include <net/sock.h>
#include <net/tcp.h>

#include <mango_compat/socket.h>

#include "socket_tty.h"
#include "../cdev.h"
#include "../dev.h"
#include "../mpipe/netdev.h"

/* ===== Class-level state (set once at init) ===== */

static struct file_operations sty_fops;
static const struct file_operations *orig_fops;

/* ===== Helpers ===== */

static struct mango_mgmt_cdev *sty_get_cdev(struct inode *inode)
{
	return mango_mgmt_cdev_of(inode);
}

/* Used by both sty_ioctl's fall-through and the inherited-socket-op
 * shims further down to short-circuit when sty_open left private_data
 * NULL (mmap-only mode: netdev/NAT/connect not ready). */
#define STY_SOCK_GUARD(filp)                                                   \
	do {                                                                   \
		if (!(filp)->private_data)                                     \
			return -ENOTCONN;                                      \
	} while (0)

/* ===== Overrides ===== */

/*
 * Open succeeds even if the terminal session can't be established —
 * private_data is left NULL and the inherited socket-fops shims
 * return -ENOTCONN.  This lets a userspace tool mmap the cdev with the
 * netdev down (no NAT, no link).  Terminal users see the usual
 * connection error from their first read/write.
 */
static int sty_open_locked(struct mango_mgmt_dev *mgmt, struct file *filp)
{
	struct net_device *ndev;
	struct mango_mgmt_mpipe_netdev_priv *priv;
	struct mango_mgmt_mpipe_nat *nat;
	struct sockaddr_in addr;
	struct socket *sock;
	long ext;
	int ret;

	ndev = mango_mgmt_mpipe_get_netdev(mgmt);
	if (!ndev)
		return 0; /* mmap-only mode */

	priv = netdev_priv(ndev);
	nat = &priv->nat;

	if (!mango_mgmt_link_ready(nat, ndev))
		return 0; /* mmap-only mode */

	ext = atomic_long_read(&nat->external);

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &sock);
	if (ret)
		return 0; /* mmap-only mode */

	/* Bind to the MPIPE interface (multi-card disambiguation) */
	sock->sk->sk_bound_dev_if = ndev->ifindex;
	sock->sk->sk_sndtimeo = 5 * HZ;

	/* Bind source to Host's external IP */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = nat_host(ext);
	ret = kernel_bind(sock, MANGO_COMPAT_SOCKADDR(&addr), sizeof(addr));
	if (ret) {
		sock_release(sock);
		return 0; /* mmap-only mode */
	}

	/* Connect to SoC's external IP on terminal port */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(mango_mgmt_term_port);
	addr.sin_addr.s_addr = nat_soc(ext);

	ret = kernel_connect(sock, MANGO_COMPAT_SOCKADDR(&addr), sizeof(addr),
			     0);
	if (ret) {
		sock_release(sock);
		return 0; /* mmap-only mode */
	}

	sock_set_keepalive(sock->sk);
	tcp_sock_set_keepidle(sock->sk, 5);
	tcp_sock_set_keepintvl(sock->sk, 5);
	tcp_sock_set_keepcnt(sock->sk, 3);

	filp->private_data = sock;
	return 0;
}

static int sty_open(struct inode *inode, struct file *filp)
{
	struct mango_mgmt_cdev *cdev = sty_get_cdev(inode);
	struct mango_mgmt_dev *mgmt;
	int idx, ret = -ENODEV;

	filp->private_data = NULL;

	mgmt = mango_mgmt_cdev_lock(cdev, &idx);
	if (mgmt)
		ret = sty_open_locked(mgmt, filp);
	mango_mgmt_cdev_unlock(cdev, idx);

	return ret;
}

static int sty_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct mango_mgmt_cdev *cdev = sty_get_cdev(file_inode(filp));
	struct mango_mgmt_dev *mgmt;
	int idx, ret = -ENODEV;

	mgmt = mango_mgmt_cdev_lock(cdev, &idx);
	if (mgmt)
		ret = mango_mgmt_cdev_mmap_common(mgmt, vma);
	mango_mgmt_cdev_unlock(cdev, idx);

	return ret;
}

static int sty_release(struct inode *inode, struct file *filp)
{
	struct socket *sock = filp->private_data;

	if (sock)
		sock_release(sock);
	filp->private_data = NULL;
	return 0;
}

/* Populate a kernel termios with our fake 115200-8N1 serial settings.
 * Shared by TCGETS (legacy struct termios, no c_ispeed/c_ospeed) and
 * TCGETS2 (struct termios2 — same layout as struct ktermios, with
 * speeds).  Newer glibc (resolute and beyond) issues TCGETS2/TCSETS2
 * from tcgetattr/tcsetattr; older glibc (noble) issues TCGETS/TCSETS. */
static void sty_fill_termios(struct ktermios *tp)
{
	memset(tp, 0, sizeof(*tp));

	tp->c_cflag = B115200 | CS8 | CREAD | CLOCAL;
	tp->c_iflag = IGNBRK;
	tp->c_ispeed = 115200;
	tp->c_ospeed = 115200;
	tp->c_cc[VMIN] = 1;
	tp->c_cc[VTIME] = 5;
	tp->c_cc[VINTR] = 0x03;
	tp->c_cc[VQUIT] = 0x1C;
	tp->c_cc[VERASE] = 0x7F;
	tp->c_cc[VKILL] = 0x15;
	tp->c_cc[VEOF] = 0x04;
	tp->c_cc[VSTART] = 0x11;
	tp->c_cc[VSTOP] = 0x13;
}

static long sty_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case TCGETS: {
		struct ktermios tp;

		sty_fill_termios(&tp);

		return copy_to_user(argp, &tp, sizeof(tp) - 2 * sizeof(speed_t))
			       ? -EFAULT
			       : 0;
	}

	/* struct termios2 has identical layout to struct ktermios (incl.
	 * c_ispeed/c_ospeed), so copy the whole thing. */
	case TCGETS2: {
		struct ktermios tp;

		sty_fill_termios(&tp);

		return copy_to_user(argp, &tp, sizeof(tp)) ? -EFAULT : 0;
	}

	case TCSETS:
	case TCSETSW:
	case TCSETSF:
	case TCSETS2:
	case TCSETSW2:
	case TCSETSF2:
	case TCFLSH:
		return 0;

	case TIOCMGET: {
		int bits = TIOCM_DSR | TIOCM_CAR | TIOCM_CTS;

		if (copy_to_user(argp, &bits, sizeof(bits)))
			return -EFAULT;
		return 0;
	}

	case TIOCMSET:
	case TIOCMBIS:
	case TIOCMBIC:
		return 0;

	case TIOCGWINSZ: {
		struct winsize ws = { .ws_row = 24, .ws_col = 80 };

		if (copy_to_user(argp, &ws, sizeof(ws)))
			return -EFAULT;
		return 0;
	}

	case TIOCSWINSZ:
	case TIOCEXCL:
	case TIOCNXCL:
		return 0;

	case TIOCGSERIAL:
	case TIOCSSERIAL:
		return -ENOTTY;

	default:
		/* FIONREAD, TIOCOUTQ, etc. — delegate to socket's own ioctl.
		 * Same NULL-private_data guard as the read/write/poll shims
		 * below: sock_ioctl assumes filp came from a socket fd and
		 * would otherwise NULL-deref when the cdev was opened in
		 * mmap-only mode (no host IP yet → sty_open left private_data
		 * NULL). */
		STY_SOCK_GUARD(filp);
		if (orig_fops && orig_fops->unlocked_ioctl)
			return orig_fops->unlocked_ioctl(filp, cmd, arg);
		return -ENOTTY;
	}
}

/* ===== NULL-guard shims for inherited socket ops =====
 *
 * sty_open may leave private_data NULL when the cdev is opened in
 * mmap-only mode (netdev down, link not ready, etc).  The inherited
 * socket fops would deref NULL, so each op-that-touches-the-socket
 * gets a tiny shim that uses STY_SOCK_GUARD (defined above) to
 * short-circuit with -ENOTCONN before calling through. */

static ssize_t sty_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	STY_SOCK_GUARD(iocb->ki_filp);
	return orig_fops->read_iter(iocb, to);
}

static ssize_t sty_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	STY_SOCK_GUARD(iocb->ki_filp);
	return orig_fops->write_iter(iocb, from);
}

static __poll_t sty_poll(struct file *filp, poll_table *wait)
{
	if (!filp->private_data)
		return EPOLLERR | EPOLLHUP;
	return orig_fops->poll(filp, wait);
}

static ssize_t sty_splice_read(struct file *in, loff_t *ppos,
			       struct pipe_inode_info *pipe, size_t len,
			       unsigned int flags)
{
	STY_SOCK_GUARD(in);
	return orig_fops->splice_read(in, ppos, pipe, len, flags);
}

static ssize_t sty_splice_write(struct pipe_inode_info *pipe, struct file *out,
				loff_t *ppos, size_t len, unsigned int flags)
{
	STY_SOCK_GUARD(out);
	return orig_fops->splice_write(pipe, out, ppos, len, flags);
}

static int sty_fasync(int fd, struct file *filp, int on)
{
	STY_SOCK_GUARD(filp);
	return orig_fops->fasync(fd, filp, on);
}

/* ===== Init / Exit ===== */

int mango_mgmt_socket_tty_init(void)
{
	struct socket *tmp;
	struct file *f;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &tmp);
	if (ret)
		return ret;

	f = sock_alloc_file(tmp, O_RDWR, NULL);
	if (IS_ERR(f)) {
		sock_release(tmp);
		return PTR_ERR(f);
	}

	/* Copy socket's fops, then patch our overrides */
	sty_fops = *f->f_op;
	orig_fops = f->f_op;

	sty_fops.open = sty_open;
	sty_fops.release = sty_release;
	sty_fops.unlocked_ioctl = sty_ioctl;
	sty_fops.mmap = sty_mmap;

	/* Wrap inherited socket ops so mmap-only opens (private_data == NULL)
	 * don't deref NULL.  Only patch slots that orig_fops actually
	 * provides — leaving NULL slots NULL keeps VFS's default fallbacks
	 * (e.g. -ESPIPE for llseek). */
	if (orig_fops->read_iter)
		sty_fops.read_iter = sty_read_iter;
	if (orig_fops->write_iter)
		sty_fops.write_iter = sty_write_iter;
	if (orig_fops->poll)
		sty_fops.poll = sty_poll;
	if (orig_fops->splice_read)
		sty_fops.splice_read = sty_splice_read;
	if (orig_fops->splice_write)
		sty_fops.splice_write = sty_splice_write;
	if (orig_fops->fasync)
		sty_fops.fasync = sty_fasync;

	/* sock_alloc_file took ownership of tmp; fput releases both */
	fput(f);

	return 0;
}

void mango_mgmt_socket_tty_exit(void)
{
	orig_fops = NULL;
	memset(&sty_fops, 0, sizeof(sty_fops));
}

const struct file_operations *mango_mgmt_socket_tty_fops(void)
{
	return &sty_fops;
}
