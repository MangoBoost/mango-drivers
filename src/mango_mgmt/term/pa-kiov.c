// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost VirtIO - PA-addressed kiov helpers
 *
 * Copies data between PA-addressed descriptor chains (struct
 * mango_mgmt_kiov, filled by vring_host.c) and flat kernel buffers.
 * Used by the virtio-console bridge kthreads.
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include "pa-kiov.h"

/* ===== Utilities ===== */

/**
 * mango_mgmt_kiov_total_len - sum remaining bytes in a chain
 */
size_t mango_mgmt_kiov_total_len(const struct mango_mgmt_kiov *kiov)
{
	size_t total = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used; i++)
		total += kiov->iov[i].iov_len;
	return total;
}

/* ===== kiov advance helper (shared by to_buf / from_buf) =====
 *
 * After processing a segment fully (iov_len drops to 0), restore the
 * original base/len so the chain reads again, and step kiov->i forward.
 * kiov->consumed tracks the running total within the current segment. */

static inline void kiov_maybe_advance(struct mango_mgmt_kiov *kiov)
{
	if (!kiov->iov[kiov->i].iov_len) {
		kiov->iov[kiov->i].iov_len = kiov->consumed;
		kiov->iov[kiov->i].iov_base -= kiov->consumed;
		kiov->consumed = 0;
		kiov->i++;
	}
}

/* ===== kiov → flat buffer ===== */

/**
 * mango_mgmt_pa_kiov_to_buf - Copy from PA-addressed kiov to buffer.
 * @buf:  destination kernel buffer
 * @kiov: source scatter-gather list (PA addresses)
 * @len:  max bytes to copy
 *
 * Returns bytes copied (< len if kiov is exhausted or page invalid).
 */
ssize_t mango_mgmt_pa_kiov_to_buf(void *buf, struct mango_mgmt_kiov *kiov,
				  size_t len)
{
	size_t remaining = len;
	u8 *dst = buf;

	while (kiov->i < kiov->used && remaining > 0) {
		while (kiov->iov[kiov->i].iov_len > 0 && remaining > 0) {
			unsigned long pfn =
				(unsigned long)(((phys_addr_t)kiov->iov[kiov->i]
							 .iov_base) >>
						PAGE_SHIFT);
			phys_addr_t page_offset;
			size_t chunk;
			struct page *page;
			void *virt;

			if (!pfn_valid(pfn))
				return len - remaining;

			page_offset =
				((phys_addr_t)kiov->iov[kiov->i].iov_base) &
				~PAGE_MASK;
			chunk = min_t(size_t, kiov->iov[kiov->i].iov_len,
				      min_t(size_t, remaining,
					    PAGE_SIZE - page_offset));
			page = pfn_to_page(pfn);
			virt = kmap_local_page(page);
			memcpy(dst, (u8 *)virt + page_offset, chunk);
			kunmap_local(virt);

			dst += chunk;
			remaining -= chunk;
			kiov->consumed += chunk;
			kiov->iov[kiov->i].iov_len -= chunk;
			kiov->iov[kiov->i].iov_base += chunk;
		}
		kiov_maybe_advance(kiov);
	}
	return len - remaining;
}

/* ===== flat buffer → kiov ===== */

/**
 * mango_mgmt_pa_kiov_from_buf - Copy from buffer into PA-addressed kiov.
 * @kiov: destination scatter-gather list (PA addresses)
 * @buf:  source kernel buffer
 * @len:  max bytes to copy
 *
 * Returns bytes copied (< len if kiov is exhausted or page invalid).
 */
ssize_t mango_mgmt_pa_kiov_from_buf(struct mango_mgmt_kiov *kiov,
				    const void *buf, size_t len)
{
	size_t remaining = len;
	const u8 *src = buf;

	while (kiov->i < kiov->used && remaining > 0) {
		while (kiov->iov[kiov->i].iov_len > 0 && remaining > 0) {
			unsigned long pfn =
				(unsigned long)(((phys_addr_t)kiov->iov[kiov->i]
							 .iov_base) >>
						PAGE_SHIFT);
			phys_addr_t page_offset;
			size_t chunk;
			struct page *page;
			void *virt;

			if (!pfn_valid(pfn))
				return len - remaining;

			page_offset =
				((phys_addr_t)kiov->iov[kiov->i].iov_base) &
				~PAGE_MASK;
			chunk = min_t(size_t, kiov->iov[kiov->i].iov_len,
				      min_t(size_t, remaining,
					    PAGE_SIZE - page_offset));
			page = pfn_to_page(pfn);
			virt = kmap_local_page(page);
			memcpy((u8 *)virt + page_offset, src, chunk);
			kunmap_local(virt);

			src += chunk;
			remaining -= chunk;
			kiov->consumed += chunk;
			kiov->iov[kiov->i].iov_len -= chunk;
			kiov->iov[kiov->i].iov_base += chunk;
		}
		kiov_maybe_advance(kiov);
	}
	return len - remaining;
}
