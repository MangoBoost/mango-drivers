/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vring_host.h - The device side of a split virtio ring, for the console.
 *
 * What the kernel's vringh.ko does for the console backend in server.c, cut
 * down to the ring the console negotiates: split ring, no indirect
 * descriptors, no event index, no packed ring. Kept in-tree because the
 * distros the SoC runs on do not all build vringh.ko (Debian sets none of
 * VHOST_RING's selectors), and because the console never needs the rest.
 *
 * The wire format -- struct vring_desc / vring_avail / vring_used -- is fixed
 * by the virtio specification, so nothing here tracks upstream code. The
 * barrier pairing with the driver side (drivers/virtio/virtio_ring.c) is:
 *   read avail->idx, rmb, read avail->ring[] and desc[]   (getdesc)
 *   write used->ring[], wmb, write used->idx              (complete)
 *   mb, read avail->flags                                 (need_notify)
 *
 * Derived from the kernel-address path of drivers/vhost/vringh.c (GPL-2.0).
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_VRING_HOST_H_
#define _MANGO_MGMT_VRING_HOST_H_

#include <linux/types.h>
#include <linux/uio.h>
#include <linux/virtio_ring.h>

/**
 * struct mango_mgmt_kiov - One descriptor chain, as an array of segments.
 * @iov: the segments; iov_base carries the descriptor's address verbatim
 *	 (a physical address for the console's rings; see pa-kiov.c)
 * @consumed: bytes already taken from iov[i], the copier's bookkeeping
 * @i: the segment the copier is on
 * @used: how many segments the last getdesc filled
 * @max_num: capacity of @iov
 */
struct mango_mgmt_kiov {
	struct kvec *iov;
	size_t consumed;
	unsigned int i, used, max_num;
};

/**
 * struct mango_mgmt_vrh - Device-side state for one split ring.
 * @vring: the ring's three tables and its size
 * @little_endian: VIRTIO_F_VERSION_1 negotiated (else native endian)
 * @last_avail_idx: the next avail slot to consume
 * @used_idx: the next used slot to publish
 * @notify: what to call when the driver wants to hear about used buffers
 */
struct mango_mgmt_vrh {
	struct vring vring;
	bool little_endian;
	u16 last_avail_idx;
	u16 used_idx;
	void (*notify)(struct mango_mgmt_vrh *vrh);
};

/**
 * mango_mgmt_vrh_init - Take over the device side of a ring.
 * @vrh: the state to set up
 * @features: the negotiated feature bits
 * @vring: the ring, from virtqueue_get_vring()
 * @notify: called from mango_mgmt_vrh_notify()
 *
 * Returns -EINVAL for a ring size that is 0, above 0xffff or not a power of
 * two, and -EOPNOTSUPP for features this implementation does not handle
 * (VIRTIO_RING_F_INDIRECT_DESC, VIRTIO_RING_F_EVENT_IDX, VIRTIO_F_RING_PACKED).
 */
int mango_mgmt_vrh_init(struct mango_mgmt_vrh *vrh, u64 features,
			const struct vring *vring,
			void (*notify)(struct mango_mgmt_vrh *vrh));

/**
 * mango_mgmt_vrh_getdesc - Take the next available chain.
 * @vrh: the ring
 * @riov: where the driver-readable (device-written-to-us, i.e. TX) segments
 *	  go, or NULL if none are expected
 * @wiov: where the driver-writable (RX) segments go, or NULL if none expected
 * @head: the chain's head index, for mango_mgmt_vrh_complete()
 *
 * Returns 0 if the ring is empty, 1 with @head and the kiov filled, or a
 * negative errno for a malformed chain: -EINVAL for an index out of range or
 * a readable segment after a writable one, -ELOOP for a chain longer than the
 * ring, -EPROTO for a segment of a kind the caller passed NULL for, -ENOBUFS
 * if the kiov is too small. On a negative return the head has still been
 * consumed and @head is valid, so the caller may hand the chain back with
 * mango_mgmt_vrh_complete(vrh, head, 0).
 */
int mango_mgmt_vrh_getdesc(struct mango_mgmt_vrh *vrh,
			   struct mango_mgmt_kiov *riov,
			   struct mango_mgmt_kiov *wiov, u16 *head);

/**
 * mango_mgmt_vrh_abandon - Give back the chain the last getdesc took.
 * @vrh: the ring
 *
 * Valid only right after a getdesc that returned 1 whose head has not been
 * completed; the next getdesc returns the same chain.
 */
void mango_mgmt_vrh_abandon(struct mango_mgmt_vrh *vrh);

/**
 * mango_mgmt_vrh_complete - Publish a chain as used.
 * @vrh: the ring
 * @head: from mango_mgmt_vrh_getdesc()
 * @len: bytes written into the chain's writable segments
 */
void mango_mgmt_vrh_complete(struct mango_mgmt_vrh *vrh, u16 head, u32 len);

/**
 * mango_mgmt_vrh_need_notify - Does the driver want an interrupt now?
 * @vrh: the ring
 *
 * Without event indices this is the driver's VRING_AVAIL_F_NO_INTERRUPT
 * flag, read after a full barrier so a used index we just published is
 * visible before the driver's flag is.
 */
bool mango_mgmt_vrh_need_notify(struct mango_mgmt_vrh *vrh);

static inline void mango_mgmt_vrh_notify(struct mango_mgmt_vrh *vrh)
{
	if (vrh->notify)
		vrh->notify(vrh);
}

/**
 * mango_mgmt_kiov_init - Allocate room for a chain of up to @num segments.
 *
 * A chain cannot legally be longer than the ring, so sizing this to the ring
 * means getdesc never allocates.
 */
int mango_mgmt_kiov_init(struct mango_mgmt_kiov *kiov, unsigned int num,
			 gfp_t gfp);
void mango_mgmt_kiov_cleanup(struct mango_mgmt_kiov *kiov);

#endif /* _MANGO_MGMT_VRING_HOST_H_ */
