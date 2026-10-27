/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - MPIPE Hardware Abstraction Layer
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_MPIPE_HW_H_
#define _MANGO_MGMT_MPIPE_HW_H_

#include <linux/types.h>

/* Forward declaration */
struct mango_mgmt_dev;

/* HW constants */
#define MANGO_MGMT_MPIPE_BLOCK_SIZE 4 /* 32-bit word */
#define MANGO_MGMT_MPIPE_QUEUE_SIZE 256 /* Total capacity in words */
#define MANGO_MGMT_MPIPE_QUEUE_BYTES (256 * 4) /* 1024 bytes total */

/* Hardware queue operations - single word at a time, caller must check
 * availability first.  hw.c folds the wire-XOR (0xDEADBEEF, matching the
 * MPIPE empty-FIFO read value) so upper layers see clean values:
 * the discovery-domain sentinel is just 0. */
void mango_mgmt_mpipe_hw_send(struct mango_mgmt_dev *dev, u32 data);
u32 mango_mgmt_mpipe_hw_recv(struct mango_mgmt_dev *dev);

/* Queue introspection */
size_t mango_mgmt_mpipe_hw_tx_available(struct mango_mgmt_dev *dev);
size_t mango_mgmt_mpipe_hw_rx_available(struct mango_mgmt_dev *dev);

/* TX/RX interrupt enable/disable (high-level API) */
/* Enable: sets watermarks to fire on data/space available */
/* Disable: sets watermarks so they never fire */
void mango_mgmt_mpipe_hw_enable_tx_irq(struct mango_mgmt_dev *dev,
				       u16 threshold);
void mango_mgmt_mpipe_hw_enable_rx_irq(struct mango_mgmt_dev *dev,
				       u16 threshold);
void mango_mgmt_mpipe_hw_disable_tx_irq(struct mango_mgmt_dev *dev);
void mango_mgmt_mpipe_hw_disable_rx_irq(struct mango_mgmt_dev *dev);

/* IRQ callback type - separate for TX and RX */
typedef void (*mango_mgmt_mpipe_irq_callback_t)(struct mango_mgmt_dev *dev);

/* Hardware configuration - const static, supplied by user at init */
struct mango_mgmt_mpipe_hw_config {
	mango_mgmt_mpipe_irq_callback_t tx_callback; /* NULL if TX not used */
	mango_mgmt_mpipe_irq_callback_t rx_callback; /* NULL if RX not used */
};

/* Shared IRQ handler — checks watermarks, dispatches TX/RX.
 * Used by MMIO (single IRQ) and the polling fallback kthread. */
void mango_mgmt_mpipe_hw_irq_handler(struct mango_mgmt_dev *dev);

/* Debug stats (sysfs) — each reads a single value on demand */
size_t mango_mgmt_mpipe_hw_tx_fill(struct mango_mgmt_dev *dev);
size_t mango_mgmt_mpipe_hw_rx_fill(struct mango_mgmt_dev *dev);
long mango_mgmt_mpipe_hw_irq_count(struct mango_mgmt_dev *dev);
long mango_mgmt_mpipe_hw_tx_irq_count(struct mango_mgmt_dev *dev);
long mango_mgmt_mpipe_hw_rx_irq_count(struct mango_mgmt_dev *dev);

/* Initialization and cleanup */
int mango_mgmt_mpipe_hw_init(struct mango_mgmt_dev *dev,
			     const struct mango_mgmt_mpipe_hw_config *config);
void mango_mgmt_mpipe_hw_exit(struct mango_mgmt_dev *dev);

/* Polling fallback for a transport without an IRQ: a kthread runs the
 * shared handler at ~100 us. */
int mango_mgmt_mpipe_hw_poll_start(struct mango_mgmt_dev *dev);
void mango_mgmt_mpipe_hw_poll_stop(struct mango_mgmt_dev *dev);

#endif /* _MANGO_MGMT_MPIPE_HW_H_ */
