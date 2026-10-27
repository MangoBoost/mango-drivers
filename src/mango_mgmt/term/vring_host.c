// SPDX-License-Identifier: GPL-2.0
/*
 * vring_host.c - The device side of a split virtio ring, for the console.
 *
 * See vring_host.h for what this is and is not. The ring lives in kernel
 * memory the driver side allocated (vring_create_virtqueue()), so every
 * access is a plain load or store behind the barriers the specification
 * asks for; there is no user memory and no IOTLB here.
 *
 * Derived from the kernel-address path of drivers/vhost/vringh.c (GPL-2.0).
 *
 * Copyright (c) 2026, MangoBoost, Inc. All rights reserved.
 */

#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/virtio_byteorder.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>

#include "vring_host.h"

/* The features this implementation would misread a ring under. */
#define MANGO_MGMT_VRH_UNSUPPORTED                                             \
	(BIT_ULL(VIRTIO_RING_F_INDIRECT_DESC) |                                \
	 BIT_ULL(VIRTIO_RING_F_EVENT_IDX) | BIT_ULL(VIRTIO_F_RING_PACKED))

/* A malformed ring is the driver's bug, not ours: say so, once in a while. */
#define vrh_bad(fmt, ...)                                                      \
	pr_err_ratelimited("mango_mgmt vring_host: " fmt "\n", ##__VA_ARGS__)

static inline u16 vrh_get16(const struct mango_mgmt_vrh *vrh,
			    const __virtio16 *p)
{
	return __virtio16_to_cpu(vrh->little_endian, READ_ONCE(*p));
}

static inline void vrh_put16(const struct mango_mgmt_vrh *vrh, __virtio16 *p,
			     u16 val)
{
	WRITE_ONCE(*p, __cpu_to_virtio16(vrh->little_endian, val));
}

int mango_mgmt_vrh_init(struct mango_mgmt_vrh *vrh, u64 features,
			const struct vring *vring,
			void (*notify)(struct mango_mgmt_vrh *vrh))
{
	unsigned int num = vring->num;

	if (!num || num > 0xffff || (num & (num - 1))) {
		vrh_bad("bad ring size %u", num);
		return -EINVAL;
	}
	if (features & MANGO_MGMT_VRH_UNSUPPORTED) {
		vrh_bad("unsupported ring features in %#llx", features);
		return -EOPNOTSUPP;
	}

	vrh->vring = *vring;
	vrh->little_endian = !!(features & BIT_ULL(VIRTIO_F_VERSION_1));
	vrh->last_avail_idx = 0;
	vrh->used_idx = 0;
	vrh->notify = notify;
	return 0;
}

/* The head of the next available chain, or the ring size if there is none. */
static int vrh_get_head(struct mango_mgmt_vrh *vrh)
{
	const struct vring *vr = &vrh->vring;
	u16 avail_idx, head;

	avail_idx = vrh_get16(vrh, &vr->avail->idx);
	if (avail_idx == vrh->last_avail_idx)
		return vr->num;

	/* Read the entry only after the driver's publish of the index. */
	virtio_rmb(true);

	head = vrh_get16(vrh,
			 &vr->avail->ring[vrh->last_avail_idx & (vr->num - 1)]);
	if (head >= vr->num) {
		vrh_bad("driver says index %u >= %u is available", head,
			vr->num);
		return -EINVAL;
	}

	vrh->last_avail_idx++;
	return head;
}

static int vrh_kiov_add(struct mango_mgmt_kiov *kiov, u64 addr, u32 len)
{
	if (kiov->used == kiov->max_num)
		return -ENOBUFS;

	kiov->iov[kiov->used].iov_base = (void *)(unsigned long)addr;
	kiov->iov[kiov->used].iov_len = len;
	kiov->used++;
	return 0;
}

/* Walk the chain from @i into the kiovs. Readable segments must all come
 * before writable ones (virtio 1.x, 2.7.4). */
static int vrh_walk(struct mango_mgmt_vrh *vrh, u16 i,
		    struct mango_mgmt_kiov *riov, struct mango_mgmt_kiov *wiov)
{
	const struct vring *vr = &vrh->vring;
	unsigned int count = 0;

	if (riov)
		riov->i = riov->used = riov->consumed = 0;
	if (wiov)
		wiov->i = wiov->used = wiov->consumed = 0;

	for (;;) {
		struct vring_desc desc;
		struct mango_mgmt_kiov *kiov;
		u16 flags;
		int err;

		memcpy(&desc, &vr->desc[i], sizeof(desc));
		flags = __virtio16_to_cpu(vrh->little_endian, desc.flags);

		if (flags & VRING_DESC_F_INDIRECT) {
			vrh_bad("indirect descriptor %u without the feature",
				i);
			return -EINVAL;
		}

		if (++count > vr->num) {
			vrh_bad("descriptor loop through %u", i);
			return -ELOOP;
		}

		if (flags & VRING_DESC_F_WRITE) {
			kiov = wiov;
		} else {
			kiov = riov;
			if (wiov && wiov->used) {
				vrh_bad("readable descriptor %u after a writable one",
					i);
				return -EINVAL;
			}
		}
		if (!kiov) {
			vrh_bad("unexpected %s descriptor %u",
				wiov ? "readable" : "writable", i);
			return -EPROTO;
		}

		err = vrh_kiov_add(
			kiov, __virtio64_to_cpu(vrh->little_endian, desc.addr),
			__virtio32_to_cpu(vrh->little_endian, desc.len));
		if (err)
			return err;

		if (!(flags & VRING_DESC_F_NEXT))
			return 0;

		i = __virtio16_to_cpu(vrh->little_endian, desc.next);
		if (i >= vr->num) {
			vrh_bad("chained index %u >= %u", i, vr->num);
			return -EINVAL;
		}
	}
}

int mango_mgmt_vrh_getdesc(struct mango_mgmt_vrh *vrh,
			   struct mango_mgmt_kiov *riov,
			   struct mango_mgmt_kiov *wiov, u16 *head)
{
	int err;

	if (WARN_ON_ONCE(!riov && !wiov))
		return -EINVAL;

	err = vrh_get_head(vrh);
	if (err < 0)
		return err;
	if (err == vrh->vring.num)
		return 0;

	*head = err;
	err = vrh_walk(vrh, *head, riov, wiov);
	if (err)
		return err;

	return 1;
}

void mango_mgmt_vrh_abandon(struct mango_mgmt_vrh *vrh)
{
	vrh->last_avail_idx--;
}

void mango_mgmt_vrh_complete(struct mango_mgmt_vrh *vrh, u16 head, u32 len)
{
	struct vring_used *used = vrh->vring.used;
	struct vring_used_elem *elem =
		&used->ring[vrh->used_idx & (vrh->vring.num - 1)];

	WRITE_ONCE(elem->id, __cpu_to_virtio32(vrh->little_endian, head));
	WRITE_ONCE(elem->len, __cpu_to_virtio32(vrh->little_endian, len));

	/* The entry is in place before the index says so. */
	virtio_wmb(true);

	vrh->used_idx++;
	vrh_put16(vrh, &used->idx, vrh->used_idx);
}

bool mango_mgmt_vrh_need_notify(struct mango_mgmt_vrh *vrh)
{
	u16 flags;

	/* Our used index is out before we look at the driver's flag; the
	 * driver orders its flag write before its re-check of used->idx. */
	virtio_mb(true);

	flags = vrh_get16(vrh, &vrh->vring.avail->flags);
	return !(flags & VRING_AVAIL_F_NO_INTERRUPT);
}

int mango_mgmt_kiov_init(struct mango_mgmt_kiov *kiov, unsigned int num,
			 gfp_t gfp)
{
	kiov->iov = kcalloc(num, sizeof(*kiov->iov), gfp);
	if (!kiov->iov)
		return -ENOMEM;

	kiov->max_num = num;
	kiov->i = kiov->used = 0;
	kiov->consumed = 0;
	return 0;
}

void mango_mgmt_kiov_cleanup(struct mango_mgmt_kiov *kiov)
{
	kfree(kiov->iov);
	kiov->iov = NULL;
	kiov->max_num = kiov->i = kiov->used = 0;
	kiov->consumed = 0;
}
