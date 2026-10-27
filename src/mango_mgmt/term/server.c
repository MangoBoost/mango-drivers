// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost VirtIO Custom Transport - virtio-console (SoC only)
 *
 * Registers a virtio-console device with the kernel, bridging the SoC's
 * hvc0 to a TCP listener on port 1022.  The Host connects via term.c.
 *
 * Also contains the virtio transport plumbing (config ops, virtqueue
 * creation, taking over the device side of each ring) — previously split
 * across common.c. The ring code itself is vring_host.c.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_console.h>
#include <linux/virtio_ring.h>
#include <linux/kthread.h>
#include <linux/printk.h>
#include <linux/kref.h>
#include <linux/atomic.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <net/sock.h>
#include <net/tcp.h>

#include <mango_compat/socket.h>

#include "../dev.h"
#include "../mpipe/netdev.h"

#include "server.h"
#include "pa-kiov.h"
#include "vring_host.h"

/* ===== Virtio internals (private to this file) ===== */

#define MANGO_MGMT_VIRTIO_VQ_MAX 2
#define MANGO_MGMT_VIRTIO_NUM_DESCS 256

enum mango_mgmt_virtio_vq_idx {
	MANGO_MGMT_VIRTIO_RX = 0,
	MANGO_MGMT_VIRTIO_TX = 1,
};

struct mango_mgmt_ring {
	struct mango_mgmt_vrh vrh;
	struct virtqueue *vq;
	struct mango_mgmt_kiov kiov;
};

struct mango_mgmt_virtio_dev {
	struct virtio_device vdev; /* must be first */
	struct mango_mgmt_dev *mgmt_dev;

	void *config_space;
	size_t config_space_len;
	u64 features;
	u8 status;
	atomic_t device_ready;
	wait_queue_head_t ready_wq;

	struct mango_mgmt_ring vrh[MANGO_MGMT_VIRTIO_VQ_MAX];
	spinlock_t vq_lock;

	wait_queue_head_t vq_wq[MANGO_MGMT_VIRTIO_VQ_MAX];
	atomic_t vq_pending[MANGO_MGMT_VIRTIO_VQ_MAX];

	struct socket *bridge_listen_sock;
	struct socket *bridge_sock;
	atomic_t bridge_connected;
	wait_queue_head_t bridge_wq;

	atomic_long_t rx_dropped;
	struct kref kref;

	struct task_struct *rx_dispatch_thread;
	struct task_struct *tx_produce_thread;
};

/* find_vqs() takes struct virtqueue_info[] from v6.11. */
#ifdef MANGO_COMPAT_HAS_VIRTQUEUE_INFO
#define USE_NEW_VIRTIO_API
#endif

/* Console bridge port — Host term.c connects here */

#define MANGO_CONSOLE_BUF_SIZE 4096

/* virtio-console config defaults */
#define MANGO_CONSOLE_COLS 80
#define MANGO_CONSOLE_ROWS 25
#define MANGO_CONSOLE_MAX_PORTS 1

/* ===== Virtio Config Operations ===== */

static u64 mango_virtio_get_features(struct virtio_device *vdev)
{
	return BIT_ULL(VIRTIO_F_VERSION_1) | BIT_ULL(VIRTIO_CONSOLE_F_SIZE) |
	       BIT_ULL(VIRTIO_CONSOLE_F_EMERG_WRITE);
}

static int mango_virtio_finalize_features(struct virtio_device *vdev)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vdev, struct mango_mgmt_virtio_dev, vdev);

	vring_transport_features(vdev);
	mvdev->features = vdev->features;
	return 0;
}

static void mango_virtio_get_config(struct virtio_device *vdev,
				    unsigned int offset, void *buf,
				    unsigned int len)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vdev, struct mango_mgmt_virtio_dev, vdev);

	if (!mvdev->config_space || offset + len > mvdev->config_space_len)
		return;

	memcpy(buf, (u8 *)mvdev->config_space + offset, len);
}

static void mango_virtio_set_config(struct virtio_device *vdev,
				    unsigned int offset, const void *buf,
				    unsigned int len)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vdev, struct mango_mgmt_virtio_dev, vdev);

	if (!mvdev->config_space || offset + len > mvdev->config_space_len)
		return;

	/* Console has no config write restrictions */
	memcpy((u8 *)mvdev->config_space + offset, buf, len);
}

static void mango_virtio_reset(struct virtio_device *vdev)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vdev, struct mango_mgmt_virtio_dev, vdev);

	atomic_set(&mvdev->device_ready, 0);
	mvdev->status = 0;
}

static u8 mango_virtio_get_status(struct virtio_device *vdev)
{
	return container_of(vdev, struct mango_mgmt_virtio_dev, vdev)->status;
}

static void mango_virtio_set_status(struct virtio_device *vdev, u8 status)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vdev, struct mango_mgmt_virtio_dev, vdev);

	mvdev->status = status;

	if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		atomic_set_release(&mvdev->device_ready, 1);
		wake_up_all(&mvdev->ready_wq);
	} else {
		atomic_set(&mvdev->device_ready, 0);
	}
}

/* Wakes the per-VQ kthread when the guest adds descriptors. */
static bool mango_virtio_notify(struct virtqueue *vq)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vq->vdev, struct mango_mgmt_virtio_dev, vdev);
	int idx = vq->index;

	if (idx < MANGO_MGMT_VIRTIO_VQ_MAX) {
		if (atomic_cmpxchg(&mvdev->vq_pending[idx], 0, 1) == 0)
			wake_up_all(&mvdev->vq_wq[idx]);
	}
	return true;
}

static void mango_virtio_vrh_notify(struct mango_mgmt_vrh *vrh)
{
	struct mango_mgmt_ring *mvrh =
		container_of(vrh, struct mango_mgmt_ring, vrh);

	vring_interrupt(0, mvrh->vq);
}

/* ===== Virtqueue Creation ===== */

/* Tear down whatever create_vqs built, for the virtio core's del_vqs and for
 * create_vqs' own unwind: a ring and its kiov come and go together. */
static void mango_virtio_del_vqs(struct virtio_device *vdev)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vdev, struct mango_mgmt_virtio_dev, vdev);
	int i;

	for (i = 0; i < MANGO_MGMT_VIRTIO_VQ_MAX; i++) {
		mango_mgmt_kiov_cleanup(&mvdev->vrh[i].kiov);
		if (mvdev->vrh[i].vq) {
			vring_del_virtqueue(mvdev->vrh[i].vq);
			mvdev->vrh[i].vq = NULL;
		}
	}
}

static int mango_virtio_create_vqs(struct virtio_device *vdev,
				   unsigned int nvqs, struct virtqueue **vqs,
				   void (**callbacks)(struct virtqueue *),
				   const char *const *names)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(vdev, struct mango_mgmt_virtio_dev, vdev);
	int i, err;

	if (nvqs > MANGO_MGMT_VIRTIO_VQ_MAX)
		return -EINVAL;

	for (i = 0; i < nvqs; i++) {
		struct mango_mgmt_ring *ring = &mvdev->vrh[i];
		const struct vring *vring;

		/* Both sides live in this kernel, so weak (SMP) barriers are
		 * the right ones; no per-buffer context, no shrinking. */
		vqs[i] = vring_create_virtqueue(
			i, MANGO_MGMT_VIRTIO_NUM_DESCS, PAGE_SIZE, vdev, true,
			false, false, mango_virtio_notify,
			callbacks ? callbacks[i] : NULL,
			(names && names[i]) ? names[i] : "mango_vq");
		if (!vqs[i]) {
			err = -ENOMEM;
			goto err_free;
		}
		ring->vq = vqs[i];

		vring = virtqueue_get_vring(vqs[i]);
		err = mango_mgmt_vrh_init(&ring->vrh, vdev->features, vring,
					  mango_virtio_vrh_notify);
		if (err) {
			dev_err(&vdev->dev, "ring %d: cannot drive it: %d", i,
				err);
			goto err_free;
		}
		err = mango_mgmt_kiov_init(&ring->kiov, vring->num, GFP_KERNEL);
		if (err)
			goto err_free;
	}

	return 0;

err_free:
	mango_virtio_del_vqs(vdev);
	return err;
}

#ifdef USE_NEW_VIRTIO_API

static int mango_virtio_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
				 struct virtqueue **vqs,
				 struct virtqueue_info *vqs_info,
				 struct irq_affinity *desc_affinity)
{
	/* Unpack vqs_info into the legacy callback/names form */
	void (**cbs)(struct virtqueue *) = NULL;
	const char **names = NULL;
	int i, ret;

	cbs = kcalloc(nvqs, sizeof(*cbs), GFP_KERNEL);
	names = kcalloc(nvqs, sizeof(*names), GFP_KERNEL);
	if (!cbs || !names) {
		kfree(cbs);
		kfree(names);
		return -ENOMEM;
	}
	for (i = 0; i < nvqs; i++) {
		cbs[i] = vqs_info[i].callback;
		names[i] = vqs_info[i].name;
	}
	ret = mango_virtio_create_vqs(vdev, nvqs, vqs, cbs, names);
	kfree(cbs);
	kfree(names);
	return ret;
}

#else

static int mango_virtio_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
				 struct virtqueue **vqs,
				 void (**callbacks)(struct virtqueue *),
				 const char *const *names, const bool *ctx,
				 struct irq_affinity *desc_affinity)
{
	return mango_virtio_create_vqs(vdev, nvqs, vqs, callbacks, names);
}

#endif /* USE_NEW_VIRTIO_API */

static const struct virtio_config_ops mango_virtio_config_ops = {
	.get_features = mango_virtio_get_features,
	.finalize_features = mango_virtio_finalize_features,
	.get = mango_virtio_get_config,
	.set = mango_virtio_set_config,
	.reset = mango_virtio_reset,
	.get_status = mango_virtio_get_status,
	.set_status = mango_virtio_set_status,
	.find_vqs = mango_virtio_find_vqs,
	.del_vqs = mango_virtio_del_vqs,
};

/* ===== Device Lifecycle ===== */

static void mango_virtio_dev_destroy(struct kref *kref)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(kref, struct mango_mgmt_virtio_dev, kref);
	int i;

	for (i = 0; i < MANGO_MGMT_VIRTIO_VQ_MAX; i++)
		mango_mgmt_kiov_cleanup(&mvdev->vrh[i].kiov);

	kfree(mvdev->config_space);
	kfree(mvdev);
}

static void mango_virtio_dev_release(struct device *dev)
{
	struct mango_mgmt_virtio_dev *mvdev =
		container_of(dev, struct mango_mgmt_virtio_dev, vdev.dev);

	kref_put(&mvdev->kref, mango_virtio_dev_destroy);
}

/* ===== TCP Bridge: listen + accept ===== */

static int mango_console_listen(struct mango_mgmt_virtio_dev *mvdev)
{
	struct sockaddr_in addr;
	struct socket *listen_sock;
	int ret;

	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP,
			       &listen_sock);
	if (ret)
		return ret;

	sock_set_reuseaddr(listen_sock->sk);

	/* Bind to MPIPE interface only — not exposed on other SoC NICs */
	{
		struct net_device *ndev =
			mango_mgmt_mpipe_get_netdev(mvdev->mgmt_dev);
		if (ndev)
			listen_sock->sk->sk_bound_dev_if = ndev->ifindex;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(mango_mgmt_term_port);

	ret = kernel_bind(listen_sock, MANGO_COMPAT_SOCKADDR(&addr),
			  sizeof(addr));
	if (ret) {
		sock_release(listen_sock);
		return ret;
	}

	ret = kernel_listen(listen_sock, 1);
	if (ret) {
		sock_release(listen_sock);
		return ret;
	}

	mvdev->bridge_listen_sock = listen_sock;

	dev_info(mvdev->mgmt_dev->dev, "Console: listening on port %d\n",
		 mango_mgmt_term_port);
	return 0;
}

static int mango_console_accept(struct mango_mgmt_virtio_dev *mvdev)
{
	struct socket *client_sock;
	int ret;

	ret = kernel_accept(mvdev->bridge_listen_sock, &client_sock, 0);
	if (ret)
		return ret;

	sock_set_keepalive(client_sock->sk);
	tcp_sock_set_keepidle(client_sock->sk, 5);
	tcp_sock_set_keepintvl(client_sock->sk, 5);
	tcp_sock_set_keepcnt(client_sock->sk, 3);

	mvdev->bridge_sock = client_sock;
	atomic_set(&mvdev->bridge_connected, 1);
	wake_up_all(&mvdev->bridge_wq);

	dev_info(mvdev->mgmt_dev->dev, "Console: Host connected on port %d\n",
		 mango_mgmt_term_port);
	return 0;
}

static void mango_console_disconnect(struct mango_mgmt_virtio_dev *mvdev)
{
	atomic_set(&mvdev->bridge_connected, 0);
	if (mvdev->bridge_sock) {
		sock_release(mvdev->bridge_sock);
		mvdev->bridge_sock = NULL;
	}
	dev_info(mvdev->mgmt_dev->dev, "Console: Host disconnected\n");
}

/* ===== Console Kthreads ===== */

static int mango_virtio_wait_ready(struct mango_mgmt_virtio_dev *mvdev)
{
	wait_event_interruptible(mvdev->ready_wq,
				 kthread_should_stop() ||
					 atomic_read(&mvdev->device_ready));
	return kthread_should_stop() ? -EINTR : 0;
}

/* RX kthread: network → guest */
static int mango_console_rx_kthread(void *data)
{
	struct mango_mgmt_virtio_dev *mvdev = data;
	struct mango_mgmt_vrh *const vrh =
		&mvdev->vrh[MANGO_MGMT_VIRTIO_RX].vrh;
	struct mango_mgmt_kiov *const kiov =
		&mvdev->vrh[MANGO_MGMT_VIRTIO_RX].kiov;
	u8 *scratch;
	int ret;

	if (mango_virtio_wait_ready(mvdev))
		return 0;

	scratch = kmalloc(MANGO_CONSOLE_BUF_SIZE, GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;

	ret = mango_console_listen(mvdev);
	if (ret) {
		kfree(scratch);
		return kthread_should_stop() ? 0 : ret;
	}

	/* Outer loop: accept a client, serve it, re-accept on disconnect */
	while (!kthread_should_stop()) {
		ret = mango_console_accept(mvdev);
		if (ret) {
			if (kthread_should_stop())
				break;
			msleep_interruptible(100);
			continue;
		}

		/* Inner loop: bridge network → guest for this client */
		while (!kthread_should_stop()) {
			struct msghdr msg = { .msg_flags = 0 };
			struct kvec iov;
			u16 head;
			size_t desc_cap;
			int recvd;
			ssize_t copied;

			ret = mango_mgmt_vrh_getdesc(vrh, NULL, kiov, &head);
			if (!ret) {
				if (wait_event_interruptible(
					    mvdev->vq_wq[MANGO_MGMT_VIRTIO_RX],
					    kthread_should_stop() ||
						    (atomic_cmpxchg(
							     &mvdev->vq_pending
								      [MANGO_MGMT_VIRTIO_RX],
							     1, 0) == 1 &&
						     (ret = mango_mgmt_vrh_getdesc(
							      vrh, NULL, kiov,
							      &head)))))
					continue;
				if (kthread_should_stop())
					break;
			}
			if (ret < 0)
				break;

			desc_cap = mango_mgmt_kiov_total_len(kiov);
			if (!desc_cap) {
				mango_mgmt_vrh_complete(vrh, head, 0);
				continue;
			}

			iov.iov_base = scratch;
			iov.iov_len =
				min_t(size_t, desc_cap, MANGO_CONSOLE_BUF_SIZE);

			recvd = kernel_recvmsg(mvdev->bridge_sock, &msg, &iov,
					       1, iov.iov_len, 0);
			if (recvd <= 0) {
				mango_mgmt_vrh_abandon(vrh);
				break; /* client disconnected */
			}

			copied = mango_mgmt_pa_kiov_from_buf(kiov, scratch,
							     recvd);

			mango_mgmt_vrh_complete(vrh, head, copied);
			if (mango_mgmt_vrh_need_notify(vrh))
				mango_mgmt_vrh_notify(vrh);
		}

		mango_console_disconnect(mvdev);
	}

	kfree(scratch);
	return 0;
}

/* TX kthread: guest → network */
static int mango_console_tx_kthread(void *data)
{
	struct mango_mgmt_virtio_dev *mvdev = data;
	struct mango_mgmt_vrh *const vrh =
		&mvdev->vrh[MANGO_MGMT_VIRTIO_TX].vrh;
	struct mango_mgmt_kiov *const kiov =
		&mvdev->vrh[MANGO_MGMT_VIRTIO_TX].kiov;
	u8 *scratch;

	if (mango_virtio_wait_ready(mvdev))
		return 0;

	scratch = kmalloc(MANGO_CONSOLE_BUF_SIZE, GFP_KERNEL);
	if (!scratch)
		return -ENOMEM;

	while (!kthread_should_stop()) {
		/* Wait for RX kthread to accept a TCP connection */
		wait_event_interruptible(
			mvdev->bridge_wq,
			kthread_should_stop() ||
				atomic_read(&mvdev->bridge_connected));
		if (kthread_should_stop())
			break;

		/* Inner loop: bridge guest → network for this client */
		while (!kthread_should_stop() &&
		       atomic_read(&mvdev->bridge_connected)) {
			int ret;
			u16 head;
			size_t n;
			ssize_t copied;
			struct msghdr msg = { .msg_flags = 0 };
			struct kvec iov;

			ret = mango_mgmt_vrh_getdesc(vrh, kiov, NULL, &head);
			if (!ret) {
				if (wait_event_interruptible(
					    mvdev->vq_wq[MANGO_MGMT_VIRTIO_TX],
					    kthread_should_stop() ||
						    !atomic_read(
							    &mvdev->bridge_connected) ||
						    (atomic_cmpxchg(
							     &mvdev->vq_pending
								      [MANGO_MGMT_VIRTIO_TX],
							     1, 0) == 1 &&
						     (ret = mango_mgmt_vrh_getdesc(
							      vrh, kiov, NULL,
							      &head)))))
					continue;
				if (kthread_should_stop() ||
				    !atomic_read(&mvdev->bridge_connected))
					break;
			}
			if (ret < 0)
				break;

			n = mango_mgmt_kiov_total_len(kiov);
			if (!n) {
				mango_mgmt_vrh_complete(vrh, head, 0);
				continue;
			}

			n = min_t(size_t, n, MANGO_CONSOLE_BUF_SIZE);
			copied = mango_mgmt_pa_kiov_to_buf(scratch, kiov, n);

			mango_mgmt_vrh_complete(vrh, head, 0);
			if (mango_mgmt_vrh_need_notify(vrh))
				mango_mgmt_vrh_notify(vrh);

			if (copied <= 0)
				continue;

			iov.iov_base = scratch;
			iov.iov_len = copied;

			if (kernel_sendmsg(mvdev->bridge_sock, &msg, &iov, 1,
					   copied) < 0)
				break;
		}
		/* Client disconnected — loop back and wait for next */
	}

	kfree(scratch);
	return 0;
}

/* ===== Init / Cleanup ===== */

int mango_mgmt_term_server_init(struct mango_mgmt_dev *mgmt_dev)
{
	struct mango_mgmt_virtio_dev *mvdev;
	struct virtio_console_config *cfg;
	int i, ret;

	mvdev = kzalloc(sizeof(*mvdev), GFP_KERNEL);
	if (!mvdev)
		return -ENOMEM;

	mvdev->mgmt_dev = mgmt_dev;
	mvdev->status = 0;
	atomic_set(&mvdev->device_ready, 0);
	init_waitqueue_head(&mvdev->ready_wq);
	kref_init(&mvdev->kref);
	spin_lock_init(&mvdev->vq_lock);

	for (i = 0; i < MANGO_MGMT_VIRTIO_VQ_MAX; i++) {
		init_waitqueue_head(&mvdev->vq_wq[i]);
		atomic_set(&mvdev->vq_pending[i], 0);
	}

	atomic_set(&mvdev->bridge_connected, 0);
	init_waitqueue_head(&mvdev->bridge_wq);
	atomic_long_set(&mvdev->rx_dropped, 0);

	/* Config space */
	mvdev->config_space =
		kzalloc(sizeof(struct virtio_console_config), GFP_KERNEL);
	if (!mvdev->config_space) {
		kfree(mvdev);
		return -ENOMEM;
	}
	mvdev->config_space_len = sizeof(struct virtio_console_config);

	cfg = mvdev->config_space;
	cfg->cols = MANGO_CONSOLE_COLS;
	cfg->rows = MANGO_CONSOLE_ROWS;
	cfg->max_nr_ports = MANGO_CONSOLE_MAX_PORTS;
	cfg->emerg_wr = 1;

	/* Register virtio device */
	mvdev->vdev.id.device = VIRTIO_ID_CONSOLE;
	mvdev->vdev.id.vendor = 0x1af4;
	mvdev->vdev.config = &mango_virtio_config_ops;
	mvdev->vdev.dev.parent = mgmt_dev->dev;
	mvdev->vdev.dev.release = mango_virtio_dev_release;

	ret = register_virtio_device(&mvdev->vdev);
	if (ret) {
		dev_err(mgmt_dev->dev, "Failed to register virtio-console: %d",
			ret);
		put_device(&mvdev->vdev.dev);
		return ret;
	}

	mvdev->status = VIRTIO_CONFIG_S_ACKNOWLEDGE;
	mgmt_dev->mgmt.term = mvdev;

	/* Start kthreads */
	mvdev->rx_dispatch_thread =
		kthread_run(mango_console_rx_kthread, mvdev,
			    "mango_mgmt_con_rx/%d", mgmt_dev->mgmt.devid);
	if (IS_ERR(mvdev->rx_dispatch_thread)) {
		ret = PTR_ERR(mvdev->rx_dispatch_thread);
		mgmt_dev->mgmt.term = NULL;
		unregister_virtio_device(&mvdev->vdev);
		return ret;
	}

	mvdev->tx_produce_thread =
		kthread_run(mango_console_tx_kthread, mvdev,
			    "mango_mgmt_con_tx/%d", mgmt_dev->mgmt.devid);
	if (IS_ERR(mvdev->tx_produce_thread)) {
		ret = PTR_ERR(mvdev->tx_produce_thread);
		kthread_stop(mvdev->rx_dispatch_thread);
		mgmt_dev->mgmt.term = NULL;
		unregister_virtio_device(&mvdev->vdev);
		return ret;
	}

	return 0;
}

void mango_mgmt_term_server_exit(struct mango_mgmt_dev *mgmt_dev)
{
	struct mango_mgmt_virtio_dev *mvdev;

	if (!mgmt_dev)
		return;

	mvdev = mgmt_dev->mgmt.term;
	if (!mvdev)
		return;

	mgmt_dev->mgmt.term = NULL;
	atomic_set(&mvdev->device_ready, 0);

	/* Shutdown sockets to unblock kthreads */
	if (mvdev->bridge_sock)
		kernel_sock_shutdown(mvdev->bridge_sock, SHUT_RDWR);
	if (mvdev->bridge_listen_sock)
		kernel_sock_shutdown(mvdev->bridge_listen_sock, SHUT_RDWR);

	wake_up_all(&mvdev->bridge_wq);
	wake_up_all(&mvdev->vq_wq[MANGO_MGMT_VIRTIO_RX]);
	wake_up_all(&mvdev->vq_wq[MANGO_MGMT_VIRTIO_TX]);

	/* Stop kthreads */
	if (mvdev->rx_dispatch_thread) {
		kthread_stop(mvdev->rx_dispatch_thread);
		mvdev->rx_dispatch_thread = NULL;
	}
	if (mvdev->tx_produce_thread) {
		kthread_stop(mvdev->tx_produce_thread);
		mvdev->tx_produce_thread = NULL;
	}

	/* Release sockets (safe — kthreads stopped) */
	if (mvdev->bridge_sock) {
		sock_release(mvdev->bridge_sock);
		mvdev->bridge_sock = NULL;
	}
	if (mvdev->bridge_listen_sock) {
		sock_release(mvdev->bridge_listen_sock);
		mvdev->bridge_listen_sock = NULL;
	}

	unregister_virtio_device(&mvdev->vdev);
}
