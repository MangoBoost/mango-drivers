/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MangoBoost Management Module - MPIPE Hardware Abstraction Layer
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/atomic.h>

#include "hw.h"
#include "../dev.h"
#include "../regmap.h"
#include "../transport/transport.h"

/* Hardware register field layout:
 * Status register: bits [8:0] = fill level (0..256 words)
 * Control register: bits [17:9] = high watermark, bits [8:0] = low watermark */
#define MANGO_MGMT_MPIPE_HW_STS_FILL_LEVEL_MASK GENMASK(8, 0)
#define MANGO_MGMT_MPIPE_HW_CTRL_HIGH_WM_SHIFT 9

/* Register mapping for device type -- SoC and Host use different register
 * addresses for the same logical operations (TX/RX are swapped). Resolved
 * once at init and stored as a const pointer in hw_device. */
struct mango_mgmt_mpipe_hw_regs {
	enum mango_mgmt_reg tx_data; /* write one word to TX queue */
	enum mango_mgmt_reg rx_data; /* read one word from RX queue */
	enum mango_mgmt_reg tx_status; /* TX queue fill level */
	enum mango_mgmt_reg rx_status; /* RX queue fill level */
	enum mango_mgmt_reg tx_ctrl; /* TX watermark thresholds */
	enum mango_mgmt_reg rx_ctrl; /* RX watermark thresholds */
};

/* Register mappings for each device type (const, resolved once at init) */
static const struct mango_mgmt_mpipe_hw_regs mango_mgmt_mpipe_hw_regs_soc = {
	.tx_data = MGMT_REG_SOC_C2H_TX_DATA,
	.rx_data = MGMT_REG_SOC_H2C_RX_DATA,
	.tx_status = MGMT_REG_SOC_C2H_STS,
	.rx_status = MGMT_REG_SOC_H2C_STS,
	.tx_ctrl = MGMT_REG_SOC_C2H_CTRL,
	.rx_ctrl = MGMT_REG_SOC_H2C_CTRL,
};

static const struct mango_mgmt_mpipe_hw_regs mango_mgmt_mpipe_hw_regs_host = {
	.tx_data = MGMT_REG_HOST_H2C_TX_DATA,
	.rx_data = MGMT_REG_HOST_C2H_RX_DATA,
	.tx_status = MGMT_REG_HOST_H2C_STS,
	.rx_status = MGMT_REG_HOST_C2H_STS,
	.tx_ctrl = MGMT_REG_HOST_H2C_CTRL,
	.rx_ctrl = MGMT_REG_HOST_C2H_CTRL,
};

/* Internal structure for hardware device state */
struct mango_mgmt_mpipe_hw_device {
	/* Configuration - const static, held by caller */
	const struct mango_mgmt_mpipe_hw_config *config;

	/* Register mappings - points to device-specific const table */
	const struct mango_mgmt_mpipe_hw_regs *regs;

	/* IRQ counters */
	atomic_long_t irq_count;
	atomic_long_t tx_irq_count;
	atomic_long_t rx_irq_count;

	/* Current watermarks for shared IRQ handler dispatch.
	 * tx_low_wm=0 means TX IRQ disabled; rx_high_wm=QUEUE_SIZE means
	 * RX IRQ disabled.  Updated by enable/disable_*_irq. */
	atomic_t tx_low_wm;
	atomic_t rx_high_wm;

	/* Polling fallback when the transport has no IRQ */
	struct task_struct *poll_thread;

#ifdef DEBUG_MANGO_MGMT_MPIPE
	/* Debug: capture first page of TX/RX words */
	struct {
		struct mango_mgmt_mpipe_hw_trace {
			u32 *buf;
			size_t count;
			bool dumped;
		} TX, RX;
	} trace;
#endif
};

/* ============================================================================
 * Private Watermark Functions
 * ============================================================================
 */

static void mango_mgmt_mpipe_hw_set_tx_watermarks(struct mango_mgmt_dev *dev,
						  u32 high_wm, u32 low_wm)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	enum mango_mgmt_reg reg = hw->regs->tx_ctrl;
	u32 ctrl_val =
		((u32)high_wm << MANGO_MGMT_MPIPE_HW_CTRL_HIGH_WM_SHIFT) |
		low_wm;

	mango_mgmt_regmap_write(dev, reg, ctrl_val);
}

static void mango_mgmt_mpipe_hw_set_rx_watermarks(struct mango_mgmt_dev *dev,
						  u32 high_wm, u32 low_wm)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	enum mango_mgmt_reg reg = hw->regs->rx_ctrl;
	u32 ctrl_val =
		((u32)high_wm << MANGO_MGMT_MPIPE_HW_CTRL_HIGH_WM_SHIFT) |
		low_wm;

	mango_mgmt_regmap_write(dev, reg, ctrl_val);
}

/* ============================================================================
 * Queue Introspection
 * ============================================================================
 */

size_t mango_mgmt_mpipe_hw_tx_available(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	u32 status = mango_mgmt_regmap_read(dev, hw->regs->tx_status);
	u32 fill_level;

	/* Handle hardware error: if register reads as -1, no space available */
	if (status == (u32)-1)
		return 0;

	fill_level = status & MANGO_MGMT_MPIPE_HW_STS_FILL_LEVEL_MASK;

	/* TX available = free space in queue (queue size - current fill level) */
	return (MANGO_MGMT_MPIPE_QUEUE_SIZE - fill_level);
}

size_t mango_mgmt_mpipe_hw_rx_available(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	u32 status = mango_mgmt_regmap_read(dev, hw->regs->rx_status);

	/* Handle hardware error: if register reads as -1, no data available */
	if (status == (u32)-1)
		return 0;

	/* RX available = data currently in queue (fill level) */
	return (status & MANGO_MGMT_MPIPE_HW_STS_FILL_LEVEL_MASK);
}

/* ============================================================================
 * Raw Hardware Operations
 * ============================================================================
 */

#ifdef DEBUG_MANGO_MGMT_MPIPE
#define MPIPE_TRACE_WORDS (PAGE_SIZE / sizeof(u32))

#define mpipe_hw_trace_dump(dev, hw, dir)                                      \
	do {                                                                   \
		struct mango_mgmt_mpipe_hw_trace *const _t = &(hw)->trace.dir; \
		if (_t->buf && _t->count > 0 && !_t->dumped) {                 \
			_t->dumped = true;                                     \
			dev_info((dev)->dev,                                   \
				 "MPIPE " #dir " trace (%zu words):\n",        \
				 _t->count);                                   \
			print_hex_dump(KERN_INFO, "  " #dir ": ",              \
				       DUMP_PREFIX_OFFSET, 32, 4, _t->buf,     \
				       _t->count * sizeof(u32), false);        \
		}                                                              \
	} while (0)

#define mpipe_hw_trace_record(dev, hw, dir, word)                              \
	do {                                                                   \
		struct mango_mgmt_mpipe_hw_trace *const _t = &(hw)->trace.dir; \
		if (_t->buf && !_t->dumped) {                                  \
			if (_t->count < MPIPE_TRACE_WORDS)                     \
				_t->buf[_t->count++] = (word);                 \
			else                                                   \
				mpipe_hw_trace_dump(dev, hw, dir);             \
		}                                                              \
	} while (0)
#endif

/* Wire-XOR: applied to every word entering/leaving the FIFO.
 *
 * The MPIPE HW returns this value when reading an empty FIFO (verified
 * in MB-Shell mpipe.cpp); choosing it as the wire mask makes any stray
 * empty-read decode as the upper-layer sentinel (0) — recoverable via
 * resync, never data corruption. */
#define MPIPE_WIRE_XOR 0xDEADBEEFu

void mango_mgmt_mpipe_hw_send(struct mango_mgmt_dev *dev, u32 data)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	u32 wire = data ^ MPIPE_WIRE_XOR;

#ifdef DEBUG_MANGO_MGMT_MPIPE
	if (unlikely(mango_mgmt_mpipe_hw_tx_available(dev) == 0))
		dev_crit_once(dev->dev, "MPIPE TX: writing to full FIFO!\n");

	mpipe_hw_trace_record(dev, hw, TX, wire);
#endif

	mango_mgmt_regmap_write(dev, hw->regs->tx_data, wire);
}

u32 mango_mgmt_mpipe_hw_recv(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	u32 wire;

#ifdef DEBUG_MANGO_MGMT_MPIPE
	if (unlikely(mango_mgmt_mpipe_hw_rx_available(dev) == 0))
		dev_crit_once(dev->dev, "MPIPE RX: reading from empty FIFO!\n");
#endif

	wire = mango_mgmt_regmap_read(dev, hw->regs->rx_data);

#ifdef DEBUG_MANGO_MGMT_MPIPE
	mpipe_hw_trace_record(dev, hw, RX, wire);
#endif

	return wire ^ MPIPE_WIRE_XOR;
}

/* ============================================================================
 * IRQ Handler and Event Dispatchers
 * ============================================================================
 */

/* Event handler: TX - called by IRQ handler or TX poller */
static void mango_mgmt_mpipe_hw_event_handler_tx(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;

	/* Call registered TX callback (netdev layer) to wake the TX kthread */
	if (hw->config->tx_callback) {
		hw->config->tx_callback(dev);
	}
}

/* Event handler: RX - called by IRQ handler or RX poller */
static void mango_mgmt_mpipe_hw_event_handler_rx(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;

	/* Call registered RX callback (netdev layer) to wake the RX kthread */
	if (hw->config->rx_callback) {
		hw->config->rx_callback(dev);
	}
}

/* Shared IRQ handler: checks TX/RX fill levels against stored watermarks.
 * Used by MMIO (single IRQ) and the polling fallback kthread.
 *
 * The MMIO IRQ line carries all FIFO events (TX Low-WM, RX High-WM,
 * etc.) ORed together.  When the netdev layer isn't actively waiting
 * on a direction it disarms the watermark; we check the atomic first
 * and skip the corresponding *_status MMIO read when disabled, so an
 * RX-only interrupt doesn't pay for a TX status read (and vice
 * versa). */
void mango_mgmt_mpipe_hw_irq_handler(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	int tx_wm, rx_wm;

	atomic_long_inc(&hw->irq_count);

	/* TX: watermark crossed? (fill dropped below low_wm → space freed) */
	tx_wm = atomic_read(&hw->tx_low_wm);
	if (tx_wm > 0) {
		u32 tx_fill = mango_mgmt_regmap_read(dev, hw->regs->tx_status) &
			      MANGO_MGMT_MPIPE_HW_STS_FILL_LEVEL_MASK;

		if (tx_fill < (u32)tx_wm) {
			atomic_long_inc(&hw->tx_irq_count);
			mango_mgmt_mpipe_hw_event_handler_tx(dev);
		}
	}

	/* RX: watermark crossed? (fill rose above high_wm → data arrived) */
	rx_wm = atomic_read(&hw->rx_high_wm);
	if ((u32)rx_wm < MANGO_MGMT_MPIPE_QUEUE_SIZE) {
		u32 rx_fill = mango_mgmt_regmap_read(dev, hw->regs->rx_status) &
			      MANGO_MGMT_MPIPE_HW_STS_FILL_LEVEL_MASK;

		if (rx_fill > (u32)rx_wm) {
			atomic_long_inc(&hw->rx_irq_count);
			mango_mgmt_mpipe_hw_event_handler_rx(dev);
		}
	}
}

/* ============================================================================
 * IRQ Enable Functions
 * ============================================================================
 */

void mango_mgmt_mpipe_hw_enable_tx_irq(struct mango_mgmt_dev *dev,
				       u16 threshold)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	u32 low_wm;

	if (!hw->config->tx_callback)
		return;

	threshold = clamp_t(u16, threshold, 1, MANGO_MGMT_MPIPE_QUEUE_SIZE);
	low_wm = MANGO_MGMT_MPIPE_QUEUE_SIZE - threshold + 1;

	/* Store watermark for shared IRQ handler + poll thread dispatch */
	atomic_set(&hw->tx_low_wm, (int)low_wm);
	mango_mgmt_mpipe_hw_set_tx_watermarks(dev, MANGO_MGMT_MPIPE_QUEUE_SIZE,
					      low_wm);
}

void mango_mgmt_mpipe_hw_enable_rx_irq(struct mango_mgmt_dev *dev,
				       u16 threshold)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	u32 high_wm;

	if (!hw->config->rx_callback)
		return;

	threshold = clamp_t(u16, threshold, 1, MANGO_MGMT_MPIPE_QUEUE_SIZE);
	high_wm = threshold - 1;

	atomic_set(&hw->rx_high_wm, (int)high_wm);
	mango_mgmt_mpipe_hw_set_rx_watermarks(dev, high_wm, 0);
}

void mango_mgmt_mpipe_hw_disable_tx_irq(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;

	atomic_set(&hw->tx_low_wm, 0);
	mango_mgmt_mpipe_hw_set_tx_watermarks(dev, MANGO_MGMT_MPIPE_QUEUE_SIZE,
					      0);
}

void mango_mgmt_mpipe_hw_disable_rx_irq(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;

	atomic_set(&hw->rx_high_wm, (int)MANGO_MGMT_MPIPE_QUEUE_SIZE);
	mango_mgmt_mpipe_hw_set_rx_watermarks(dev, MANGO_MGMT_MPIPE_QUEUE_SIZE,
					      0);
}

/* ============================================================================
 * Debug Stats Accessors
 * ============================================================================
 */

size_t mango_mgmt_mpipe_hw_tx_fill(struct mango_mgmt_dev *dev)
{
	return MANGO_MGMT_MPIPE_QUEUE_SIZE -
	       mango_mgmt_mpipe_hw_tx_available(dev);
}

size_t mango_mgmt_mpipe_hw_rx_fill(struct mango_mgmt_dev *dev)
{
	return mango_mgmt_mpipe_hw_rx_available(dev);
}

long mango_mgmt_mpipe_hw_irq_count(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	return hw ? atomic_long_read(&hw->irq_count) : 0;
}

long mango_mgmt_mpipe_hw_tx_irq_count(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	return hw ? atomic_long_read(&hw->tx_irq_count) : 0;
}

long mango_mgmt_mpipe_hw_rx_irq_count(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	return hw ? atomic_long_read(&hw->rx_irq_count) : 0;
}

/* ============================================================================
 * Initialization and Cleanup
 * ============================================================================
 */

/* Default config - all callbacks disabled */
static const struct mango_mgmt_mpipe_hw_config
	mango_mgmt_mpipe_hw_default_config = {
		.tx_callback = NULL,
		.rx_callback = NULL,
	};

/* Polling fallback: call shared IRQ handler at ~100μs intervals.
 * Used when transport has no HW IRQ (e.g., MMIO without DT interrupt). */
static int mpipe_hw_poll_thread(void *data)
{
	struct mango_mgmt_dev *dev = data;

	while (!kthread_should_stop()) {
		usleep_range(100, 200);
		mango_mgmt_mpipe_hw_irq_handler(dev);
	}
	return 0;
}

int mango_mgmt_mpipe_hw_init(struct mango_mgmt_dev *dev,
			     const struct mango_mgmt_mpipe_hw_config *config)
{
	struct mango_mgmt_mpipe_hw_device *hw =
		kzalloc(sizeof(*hw), GFP_KERNEL);

	if (!hw)
		return -ENOMEM;

	/* Use provided config or default (all callbacks disabled) */
	hw->config = config ?: &mango_mgmt_mpipe_hw_default_config;

	/* Select register mapping based on device type (const table) */
	hw->regs = mango_mgmt_transport_is_soc_device(dev)
			   ? &mango_mgmt_mpipe_hw_regs_soc
			   : &mango_mgmt_mpipe_hw_regs_host;

	/* Watermarks: disabled by default (kthreads arm them on demand) */
	atomic_set(&hw->tx_low_wm, 0);
	atomic_set(&hw->rx_high_wm, (int)MANGO_MGMT_MPIPE_QUEUE_SIZE);

	dev->mgmt.hw = hw;

#ifdef DEBUG_MANGO_MGMT_MPIPE
	/* Allocate trace pages for first-page capture (debug) */
	hw->trace.TX.buf = (u32 *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	hw->trace.RX.buf = (u32 *)__get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!hw->trace.TX.buf || !hw->trace.RX.buf)
		dev_warn(dev->dev,
			 "MPIPE trace alloc failed, tracing disabled\n");
#endif

	return 0;
}

void mango_mgmt_mpipe_hw_exit(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;

	if (!hw)
		return;

	/* Leave the watermarks disarmed for the next owner. */
	mango_mgmt_mpipe_hw_disable_tx_irq(dev);
	mango_mgmt_mpipe_hw_disable_rx_irq(dev);

#ifdef DEBUG_MANGO_MGMT_MPIPE
	/* Dump trace pages on unload (skip if already dumped inline) */
	mpipe_hw_trace_dump(dev, hw, TX);
	mpipe_hw_trace_dump(dev, hw, RX);

	/* Free trace pages */
	free_page((unsigned long)hw->trace.TX.buf);
	free_page((unsigned long)hw->trace.RX.buf);
#endif

	/* Cleanup */
	kfree(hw);
	dev->mgmt.hw = NULL;
}

/* Without an IRQ source or this thread nothing drains the FIFO, so a
 * failure here is the link's failure. */
int mango_mgmt_mpipe_hw_poll_start(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;
	struct task_struct *thread;
	int node = dev_to_node(dev->dev);

	thread = kthread_run(mpipe_hw_poll_thread, dev, "mango_mpipe_poll");
	if (IS_ERR(thread)) {
		dev_err(dev->dev, "MPIPE poll thread failed: %ld\n",
			PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	if (node >= 0)
		set_cpus_allowed_ptr(thread, cpumask_of_node(node));
	hw->poll_thread = thread;
	return 0;
}

void mango_mgmt_mpipe_hw_poll_stop(struct mango_mgmt_dev *dev)
{
	struct mango_mgmt_mpipe_hw_device *hw = dev->mgmt.hw;

	if (!hw->poll_thread)
		return;

	kthread_stop(hw->poll_thread);
	hw->poll_thread = NULL;
}
