/* SPDX-License-Identifier: GPL-2.0 */
/*
 * libcompat — SunRPC server shims.
 *
 * Abstracts SunRPC API differences across kernel versions.  All
 * detection is grep-based (no LINUX_VERSION_CODE — vendor kernels like
 * Xilinx 6.6 backport features).
 *
 * Feature flags (libcompat/compat.mk) consumed here or directly by
 * driver code:
 *
 *   MANGO_COMPAT_SVC_DESTROY_TAKES_SVCP   svc_destroy(struct svc_serv **)
 *                                         (6.8+)
 *   MANGO_COMPAT_SVC_DESTROY_TAKES_KREF   sv_refcnt exists in svc_serv
 *                                         (Xilinx 6.6, some 5.x)
 *     (neither)                           svc_destroy(struct svc_serv *)
 *                                         (some Ubuntu 5.15)
 *   MANGO_COMPAT_SVC_RECV_IS_VOID         svc_recv returns void
 *                                         (6.7-6.17, Xilinx 6.6)
 *   MANGO_COMPAT_SVC_RECV_TAKES_TIMEOUT   svc_recv takes long timeout
 *                                         (< 6.3, 7.0+)
 *     (neither)                           svc_recv returns int, no
 *                                         timeout (6.3-6.6)
 *   MANGO_COMPAT_HAS_SVC_PROCESS          svc_process exported (call it
 *                                         after svc_recv) (< 7.0)
 *   MANGO_COMPAT_HAS_SVC_THREAD_SHOULD_STOP  svc_thread_should_stop()
 *                                         exists (6.8+); else use
 *                                         kthread_should_stop()
 *   MANGO_COMPAT_HAS_SVC_THREAD_INIT_STATUS  svc_thread_init_status()
 *                                         exists (6.8+); else startup
 *                                         needs no readiness handshake
 *   MANGO_COMPAT_SVC_CREATE_TAKES_OPS     svc_create takes svc_serv_ops *
 *                                         (some Ubuntu 5.15)
 *   MANGO_COMPAT_HAS_SVC_XPRT_CREATE      svc_xprt_create in
 *                                         Module.symvers (most kernels;
 *                                         svc_addsock fallback otherwise)
 *   MANGO_COMPAT_SVC_ADDSOCK_TAKES_NET    svc_addsock has struct net *
 *                                         param (5.15.0-173+, 6.8+)
 *   MANGO_COMPAT_SVC_SET_NUM_THREADS_TAKES_INT  2nd arg is 'unsigned int
 *                                         min_threads' (7.0+); else
 *                                         'struct svc_pool *pool'
 *   MANGO_COMPAT_SVC_XPRT_DESTROY_ALL_HAS_UNREGISTER  3rd 'bool
 *                                         unregister' arg present (7.0+)
 *   MANGO_COMPAT_SVC_PROC_HAS_ARGZERO     svc_procedure has pc_argzero
 *                                         (>= ~5.13)
 *   MANGO_COMPAT_SVC_PROC_HAS_NAME        svc_procedure has pc_name
 *                                         (>= ~5.15)
 *   MANGO_COMPAT_SVC_DISPATCH_TAKES_STATP vs_dispatch takes __be32 *statp
 *                                         (driver code)
 *   MANGO_COMPAT_PG_AUTH_RETURNS_ENUM     pg_authenticate returns enum
 *                                         svc_auth_status (driver code)
 *   MANGO_COMPAT_SVC_PROGRAM_HAS_PG_STATS svc_program has pg_stats
 *                                         (driver code)
 *   MANGO_COMPAT_SVC_VS_COUNT_IS_LONG     vs_count is unsigned long *
 *                                         (driver code)
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_COMPAT_SUNRPC_H_
#define _MANGO_COMPAT_SUNRPC_H_

#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/svc_xprt.h>

#include <mango_compat/socket.h>

/* ====================================================================
 * Section 1: Service lifecycle compat
 * ==================================================================== */

/*
 * svc_xprt_create / svc_xprt_destroy_all — checked against Module.symvers.
 * When exported: the header (or kbuild) provides the declaration.
 * When not exported (some Ubuntu 5.15): fall back to
 * svc_addsock with a manually-created TCP socket.
 *
 * Kernel 7.0 also added a third 'bool unregister' arg to
 * svc_xprt_destroy_all; the mango_compat_svc_xprt_destroy_all() wrapper
 * below dispatches to the right form.
 */
#ifndef MANGO_COMPAT_HAS_SVC_XPRT_CREATE
#include <linux/file.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/fdtable.h>

static inline int svc_xprt_create(struct svc_serv *serv, const char *xprt_name,
				  struct net *net, const int family,
				  const unsigned short port, int flags,
				  const struct cred *cred)
{
	struct socket *sock;
	struct sockaddr_in addr;
	struct file *file;
	char name[25];
	int fd, ret;

	ret = sock_create_kern(net, family, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret)
		return ret;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	ret = kernel_bind(sock, MANGO_COMPAT_SOCKADDR(&addr), sizeof(addr));
	if (ret)
		goto err_sock;

	ret = kernel_listen(sock, 64);
	if (ret)
		goto err_sock;

	/* svc_addsock takes an fd — wrap the kernel socket */
	file = sock_alloc_file(sock, O_RDWR, NULL);
	if (IS_ERR(file))
		return PTR_ERR(file); /* sock_alloc_file consumed sock */

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(file);
		return fd;
	}
	fd_install(fd, file);

#ifdef MANGO_COMPAT_SVC_ADDSOCK_TAKES_NET
	ret = svc_addsock(serv, net, fd, name, sizeof(name), cred);
#else
	ret = svc_addsock(serv, fd, name, sizeof(name), cred);
#endif

	/* svc_addsock holds its own socket ref; close our fd */
	close_fd(fd);

	return ret < 0 ? ret : 0;

err_sock:
	sock_release(sock);
	return ret;
}

#endif

/* svc_xprt_destroy_all wrapper — handles three cases:
 *   - kernel 6.18+ exports a 3-arg form (bool unregister; a no-op extra
 *     rpcbind cleanup for vs_hidden programs like ours)
 *   - v5.18-v6.17 export the 2-arg form
 *   - older kernels ship the same function under its pre-rename name,
 *     svc_shutdown_net(); it MUST be called — their svc_destroy() has
 *     BUG_ON(!list_empty(&serv->sv_permsocks)), so skipping teardown
 *     panics at server stop. */
static inline void mango_compat_svc_xprt_destroy_all(struct svc_serv *serv,
						     struct net *net)
{
#if defined(MANGO_COMPAT_HAS_SVC_XPRT_CREATE) &&                               \
	defined(MANGO_COMPAT_SVC_XPRT_DESTROY_ALL_HAS_UNREGISTER)
	svc_xprt_destroy_all(serv, net, true);
#elif defined(MANGO_COMPAT_HAS_SVC_XPRT_CREATE)
	svc_xprt_destroy_all(serv, net);
#else
	svc_shutdown_net(serv, net);
#endif
}

/* svc_set_num_threads wrapper — kernel 7.0 changed the 2nd arg from
 * 'struct svc_pool *pool' (NULL = all pools) to 'unsigned int min_threads'
 * (0 = default).  Either way the caller only cares about the target thread
 * count. */
static inline int mango_compat_svc_set_num_threads(struct svc_serv *serv,
						   int nrservs)
{
#ifdef MANGO_COMPAT_SVC_SET_NUM_THREADS_TAKES_INT
	return svc_set_num_threads(serv, 0, nrservs);
#else
	return svc_set_num_threads(serv, NULL, nrservs);
#endif
}

/* svc_destroy — three known signatures across kernels. */
static inline void mango_compat_svc_serv_free(struct svc_serv **servp)
{
	if (!*servp)
		return;
#if defined(MANGO_COMPAT_SVC_DESTROY_TAKES_SVCP)
	svc_destroy(servp);
#elif defined(MANGO_COMPAT_SVC_DESTROY_TAKES_KREF)
	svc_destroy(&(*servp)->sv_refcnt);
	*servp = NULL;
#else
	svc_destroy(*servp);
	*servp = NULL;
#endif
}

/* svc_recv — four forms detected by compat.mk greps (not VERSION_CODE,
 * because vendor kernels like Xilinx 6.6 backport later signatures):
 *   MANGO_COMPAT_SVC_RECV_IS_VOID:       void svc_recv(rqstp)  (6.6-6.19)
 *   MANGO_COMPAT_SVC_RECV_TAKES_TIMEOUT: int  svc_recv(rqstp, timeout)
 *                                        (<6.3, 7.0+)
 *   neither:                             int  svc_recv(rqstp)  (6.3-6.6)
 *
 * Whether the caller must run svc_process() after a successful recv is a
 * separate axis tracked by MANGO_COMPAT_HAS_SVC_PROCESS:
 *   - <6.7:   svc_process is exported, recv returns int → caller dispatches.
 *   - 6.7-6.17: svc_recv folds dispatch in (void return) → no svc_process.
 *   - 7.0+:   svc_recv folds dispatch in but went back to int+timeout, and
 *             svc_process was unexported. */
static inline void mango_compat_svc_recv_one(struct svc_rqst *rqstp)
{
#ifdef MANGO_COMPAT_SVC_RECV_IS_VOID
	svc_recv(rqstp);
#else
	int err;

#ifdef MANGO_COMPAT_SVC_RECV_TAKES_TIMEOUT
	err = svc_recv(rqstp, 60 * HZ);
#else
	err = svc_recv(rqstp);
#endif
#ifdef MANGO_COMPAT_HAS_SVC_PROCESS
	/* EAGAIN/EINTR: no request ready (timeout or signal) — skip process. */
	if (err != -EAGAIN && err != -EINTR)
		svc_process(rqstp);
#else
	/* 7.0+: svc_recv dispatches internally; svc_process isn't exported. */
	(void)err;
#endif
#endif
}

/* Thread stop check for the svc thread loop.
 *
 * On 6.8+ (commit fa341560ca74) SunRPC stops threads via
 * SP_NEED_VICTIM / SP_VICTIM_REMAINS pool flags — NOT kthread_stop —
 * and the thread function must call svc_thread_should_stop(rqstp) to
 * observe the stop request.  Using kthread_should_stop() on 6.8+ means
 * the loop never exits and svc_set_num_threads(0) hangs forever in
 * wait_on_bit(SP_VICTIM_REMAINS) during module unload.
 *
 * On older kernels SunRPC uses kthread_stop() and the correct check is
 * kthread_should_stop(). */
static inline bool mango_compat_svc_thread_should_stop(struct svc_rqst *rqstp)
{
#ifdef MANGO_COMPAT_HAS_SVC_THREAD_SHOULD_STOP
	return svc_thread_should_stop(rqstp);
#else
	(void)rqstp;
	return kthread_should_stop();
#endif
}

/* Startup readiness report for the svc thread loop.
 *
 * On 6.8+ svc_start_kthreads() wakes the new thread and then waits in
 * wait_var_event(&rqstp->rq_err, rqstp->rq_err != -EAGAIN), so the
 * thread function must publish its init result via
 * svc_thread_init_status(rqstp, 0).  Skipping it leaves rq_err at
 * -EAGAIN and svc_set_num_threads(1) never returns — the thread still
 * serves requests, so the server looks healthy while everything after
 * server init is silently skipped.
 *
 * Older kernels have no such handshake, so this is a no-op there. */
static inline void mango_compat_svc_thread_init_ok(struct svc_rqst *rqstp)
{
#ifdef MANGO_COMPAT_HAS_SVC_THREAD_INIT_STATUS
	svc_thread_init_status(rqstp, 0);
#else
	(void)rqstp;
#endif
}

/* svc_create — old kernels take struct svc_serv_ops * instead of a
 * bare thread function.  The macro builds the ops struct inline. */
#ifdef MANGO_COMPAT_SVC_CREATE_TAKES_OPS
#define MANGO_COMPAT_SVC_CREATE(prog, bufsz, threadfn)                         \
	({                                                                     \
		static const struct svc_serv_ops _ops = {                      \
			.svo_function = (threadfn),                            \
			.svo_enqueue_xprt = svc_xprt_do_enqueue,               \
			.svo_module = THIS_MODULE,                             \
		};                                                             \
		svc_create((prog), (bufsz), &_ops);                            \
	})
#else
#define MANGO_COMPAT_SVC_CREATE(prog, bufsz, threadfn)                         \
	svc_create((prog), (bufsz), (threadfn))
#endif

/* ====================================================================
 * Section 2: Server-side XDR helpers
 *
 * bool fn(struct svc_rqst *, struct xdr_stream *); bodies use the
 * __be32 *p the START macros position, ntohl(*p++) / *p++ = htonl(val).
 * ==================================================================== */

#define MANGO_COMPAT_SVC_DECODE_FUNC(name)                                     \
	static bool mgmt_svc_dec_##name(struct svc_rqst *rqstp,                \
					struct xdr_stream *xdr)

#define MANGO_COMPAT_SVC_ENCODE_FUNC(name)                                     \
	static bool mgmt_svc_enc_##name(struct svc_rqst *rqstp,                \
					struct xdr_stream *xdr)

#define MANGO_COMPAT_SVC_DECODE_START(nwords)                                  \
	__be32 *p __maybe_unused =                                             \
		(nwords) ? xdr_inline_decode(xdr, (nwords)*4) : NULL;          \
	if ((nwords) && !p)                                                    \
	return false

#define MANGO_COMPAT_SVC_ENCODE_START(nwords)                                  \
	__be32 *p __maybe_unused =                                             \
		(nwords) ? xdr_reserve_space(xdr, (nwords)*4) : NULL;          \
	if ((nwords) && !p)                                                    \
	return false

#define MANGO_COMPAT_SVC_DECODE_RETURN return true
#define MANGO_COMPAT_SVC_ENCODE_RETURN return true

/* ====================================================================
 * Section 3: Procedure table field compat
 * ==================================================================== */

#ifdef MANGO_COMPAT_SVC_PROC_HAS_ARGZERO
#define MANGO_COMPAT_SVC_ARGZERO(sz) .pc_argzero = sz,
#else
#define MANGO_COMPAT_SVC_ARGZERO(sz)
#endif

#ifdef MANGO_COMPAT_SVC_PROC_HAS_NAME
#define MANGO_COMPAT_SVC_NAME(n) .pc_name = n,
#else
#define MANGO_COMPAT_SVC_NAME(n)
#endif

#endif /* _MANGO_COMPAT_SUNRPC_H_ */
