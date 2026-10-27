// SPDX-License-Identifier: GPL-2.0
/*
 * MangoBoost Management Module - Public API
 *
 * Host-side callers use mango_mgmt_rpc_call() (or the typed per-op
 * wrappers generated in each project's *_rpc.h) to talk to the SoC.
 * SoC-side modules register a project descriptor via
 * mango_mgmt_register_project() and attach per-devid / per-port priv
 * via mango_mgmt_attach_{devid,port}().
 *
 * Copyright (c) 2025, MangoBoost, Inc. All rights reserved.
 */

#ifndef _MANGO_MGMT_API_H_
#define _MANGO_MGMT_API_H_

#include <linux/types.h>

struct pci_dev;

struct mango_mgmt_dev;
struct xdr_stream;

/* ===========================================================
 * Project identifiers
 * =========================================================== */

enum mango_mgmt_prj {
	/* mgmt's own project — introspection meta-ops.  Registered from
	 * api/meta.c; acts as the reference example of how a project is
	 * wired up (handlers, op table, attach, dispatch). */
	MANGO_MGMT_PRJ_META = 0,
	MANGO_MGMT_PRJ_PCC = 1,
	/* New projects go above this line */
	MANGO_MGMT_PRJ_MAX,
};

/* Internal array sizes — not part of the wire format. */
#define MANGO_MGMT_MAX_PORTS 2

/* Wire-level upper bound on the payload (after the prj_id/proj_op
 * routing header) carried by a single RPC_CALL in either direction,
 * in u32 words.  This sizes the SunRPC reply buffer (pc_xdrressize)
 * and the client-side per-proc argbuf / resbuf.
 *
 * The server-side in-memory arg/res structs are NOT bounded by this
 * — rpc_xdr.c kmalloc's them per request based on desc->{arg,res}_size,
 * so project authors don't need to worry about staying under a struct-
 * size cap.  The static_assert in MANGO_MGMT_RPC_DECLARE_OP[_PORT]
 * still gates the wire side so an oversize op fails the build with
 * the op name. */
#define MANGO_MGMT_RPC_MAX_BODY_WORDS 256

/* ===========================================================
 * Generic reply base
 *
 * Every typed res struct (including the ones generated from X-macro
 * schemas) begins with s32 status.  Direct callers that don't have a
 * typed res can use this type. */
struct mango_mgmt_rpc_res {
	s32 status;
};

/* ===========================================================
 * Op descriptor — filled by MANGO_MGMT_OP_ENTRY()
 *
 * Codecs operate on typed arg/res buffers via void pointers; the
 * macro framework in mango_mgmt_rpc_macros.h generates them from an
 * X-macro schema.
 * =========================================================== */

enum mango_mgmt_rpc_scope {
	MANGO_MGMT_SCOPE_DEVID, /* priv = attach_devid(prj, devid)      */
	MANGO_MGMT_SCOPE_PORT, /* priv = attach_port(prj, devid, port) */
};

struct mango_mgmt_proc_desc {
	enum mango_mgmt_rpc_scope scope;
	int (*handler)(void *priv, const void *args, void *res);
	int (*arg_decode)(struct xdr_stream *xdr, void *out);
	int (*arg_encode)(struct xdr_stream *xdr, const void *in);
	int (*res_decode)(struct xdr_stream *xdr, void *out);
	int (*res_encode)(struct xdr_stream *xdr, const void *in);
	size_t arg_size;
	size_t res_size;
	const char *name;
};

/* ===========================================================
 * Project lifecycle (SoC side)
 * =========================================================== */

/**
 * mango_mgmt_register_project - Register a project's op table
 * @prj_id:  Project ID (MANGO_MGMT_PRJ_*).  Must be < MANGO_MGMT_PRJ_MAX.
 * @ops:     Array of op descriptors, indexed by project-local op id.
 *           Must remain valid until mango_mgmt_unregister_project returns.
 * @n_ops:   Number of entries in @ops.
 *
 * One call per project per module lifetime, typically from module_init.
 * Implemented as a macro that captures the caller's THIS_MODULE so the
 * server-side dispatcher can pin the owner across a dispatch (keeping
 * handler / codec pointers valid on kernels that don't expose an
 * xdr_stream across pc_decode → pc_func → pc_encode boundaries).
 *
 * Returns: 0 on success, -EINVAL on bad args, -EEXIST if @prj_id already
 *          registered, -ENOMEM on allocation failure.
 */
int __mango_mgmt_register_project(u32 prj_id,
				  const struct mango_mgmt_proc_desc *ops,
				  unsigned int n_ops, struct module *owner);
#define mango_mgmt_register_project(prj_id, ops, n_ops)                        \
	__mango_mgmt_register_project((prj_id), (ops), (n_ops), THIS_MODULE)

/**
 * mango_mgmt_unregister_project - Unregister a project
 *
 * Auto-detaches any remaining per-devid and per-port instances (with
 * WARN for each) and synchronizes RCU before returning, so the caller
 * can immediately free the ops table and any attached priv.
 */
void mango_mgmt_unregister_project(u32 prj_id);

/**
 * mango_mgmt_attach_devid - Bind a per-devid priv to (prj_id, devid)
 * @priv: Opaque pointer passed to SCOPE_DEVID handlers.  Must not be NULL.
 *
 * Returns: 0 on success, -EINVAL on bad args, -ENODEV if no such mgmt
 *          device, -EEXIST if already attached.
 */
int mango_mgmt_attach_devid(u32 prj_id, int devid, void *priv);

/**
 * mango_mgmt_detach_devid - Unbind the per-devid priv
 *
 * Synchronizes RCU before returning.  After this call the caller's
 * priv is no longer reachable from any in-flight dispatch.
 */
void mango_mgmt_detach_devid(u32 prj_id, int devid);

/**
 * mango_mgmt_attach_port - Bind a per-port priv to (prj_id, devid, port)
 * @port: Must be < MANGO_MGMT_MAX_PORTS.
 */
int mango_mgmt_attach_port(u32 prj_id, int devid, u32 port, void *priv);

/** mango_mgmt_detach_port - Unbind the per-port priv */
void mango_mgmt_detach_port(u32 prj_id, int devid, u32 port);

/* ===========================================================
 * Host-side RPC
 * =========================================================== */

/**
 * mango_mgmt_rpc_call - Generic Host->SoC RPC call
 * @devid:    Management device ID (routing key).
 * @prj_id:   Target project ID (forwarded to the SoC dispatcher).
 * @proj_op:  Op number within the target project's op enum.
 * @desc:     Op descriptor — only @arg_encode and @res_decode are read
 *            on the client side (the typed wrapper supplies the rest
 *            with NULL).  The descriptor lives in the *caller's*
 *            module so the project does not need to be registered on
 *            the host: a host-only aux module can call into a
 *            SoC-only project's _rpc() wrappers and still link.
 * @args:     Typed arg struct (layout defined by the op's schema).
 * @res:      Typed res struct (first field must be s32 status).
 *
 * Typed per-op wrappers generated from each project's *_rpc.h file
 * are the intended callers; they hard-code (prj_id, proj_op, desc) at
 * compile time.
 *
 * Returns: 0 on success, transport error or handler-returned -errno
 *          on failure.  -ENOPROTOOPT comes back from the SoC if the
 *          project isn't registered there.
 */
int mango_mgmt_rpc_call(int devid, u32 prj_id, u32 proj_op,
			const struct mango_mgmt_proc_desc *desc,
			const void *args, void *res);

/**
 * mango_mgmt_check_soc_prj_devid - Is @prj_id attached at devid scope on SoC?
 *
 * Returns true iff the SoC has called both register_project(prj_id, ...)
 * and attach_devid(prj_id, ...) for its single mgmt device.
 * Returns false on transport error.
 */
bool mango_mgmt_check_soc_prj_devid(int devid, u32 prj_id);

/**
 * mango_mgmt_check_soc_prj_port - Is @prj_id attached at port scope on SoC?
 * @port: HW port index, < MANGO_MGMT_MAX_PORTS.
 */
bool mango_mgmt_check_soc_prj_port(int devid, u32 prj_id, u32 port);

/* ===========================================================
 * Device utilities
 * =========================================================== */

int mango_mgmt_get_device_count(void);
bool mango_mgmt_device_is_ready(int devid);

/**
 * mango_mgmt_get_devid - Resolve a transport-level address to a device ID
 * @mgmt_addr: Transport-level identifier as a string.  For PCIe-attached
 *             management devices this is the PCI BDF (e.g. "0000:01:00.0");
 *             for platform devices it is the platform-bus device name.
 *
 * Returns: The device ID (0-based index) of the matching mango_mgmt device
 *          on success, -EINVAL if @mgmt_addr is NULL, -ENODEV if no device
 *          matches.  On SoC, where only one mgmt device exists, returns
 *          the lone device's ID regardless of @mgmt_addr.
 */
int mango_mgmt_get_devid(const char *mgmt_addr);

/**
 * mango_mgmt_pcie_is_mgmt - Whether @pdev is a card's management function
 * @pdev: PCI function to classify, bound or not. Answered from this driver's
 *        device table, so a sibling need not repeat the management IDs.
 */
bool mango_mgmt_pcie_is_mgmt(struct pci_dev *pdev);

/**
 * mango_mgmt_pcie_is_soc - Whether @pdev is the SoC side's management function
 * @pdev: PCI function to classify, bound or not.
 *
 * The SoC behind the card's second PCIe port sees its own management
 * function, under a device ID of its own, so a sibling on the same slot can
 * learn which side of the card it is driven from by asking about that
 * function. False for the Host's management function and for anything that
 * is not one.
 */
bool mango_mgmt_pcie_is_soc(struct pci_dev *pdev);

/**
 * mango_mgmt_get_soc_ip - Get SoC management IP address
 * @ip_addr: Output: IP address (network byte order)
 * @netmask: Output: Netmask (network byte order)
 *
 * Returns: 0 on success, -ENODEV / -ENOLINK on failure.
 */
int mango_mgmt_get_soc_ip(int devid, __be32 *ip_addr, __be32 *netmask);

int mango_mgmt_read_eeprom(int devid, u8 *buf, u16 len, u8 offset);
int mango_mgmt_write_eeprom(int devid, u8 *buf, u16 len, u8 offset);

/**
 * mango_mgmt_update_eeprom - Set the bits of one EEPROM byte, keeping the rest
 * @offset: the byte, from the EEPROM base.
 * @mask: the bits to set.
 * @bits: their new values; bits outside @mask are ignored.
 *
 * Whole against every other EEPROM access on this host: two updates of one
 * byte both land. Written only where the bits differ.
 *
 * Returns: 0, or a negative errno.
 */
int mango_mgmt_update_eeprom(int devid, u8 offset, u8 mask, u8 bits);

#endif /* _MANGO_MGMT_API_H_ */
