# SPDX-License-Identifier: GPL-2.0
#
# libcompat — kernel-compat feature probes shared by the mango modules.
#
# Every flag here is a *semantic* feature test derived from the target
# kernel's own headers (grep/sed content probes), never from
# LINUX_VERSION_CODE: distro kernels backport APIs, so version gating
# misidentifies leaves (RHEL/Rocky/Ubuntu HWE).  Every probe must carry
# its own documentation: which kernel release changed the API and which
# distro kernels are known to backport it (see README.md).
#
# Probe style: always match the DECLARATION FORM (member `(*name)(`,
# call form `name(`, or a typed prefix) — never a bare token.  Bare
# tokens false-positive on comments/backport residue in distro headers.
#
# Header-content probes need the kernel *source* tree.  Yocto/PetaLinux
# passes KERNEL_PATH (separate from KERNEL_BUILD); plain builds set only
# KERNEL_BUILD, which also holds the headers.
KERNEL_PATH ?= $(KERNEL_BUILD)

# Split kernel-headers: $(KERNEL_BUILD) holds only the flavour-specific tree,
# the shared include/ lives elsewhere.  Debian names it in a stub Makefile
# (`include /usr/src/linux-headers-<abi>-common/Makefile`); Ubuntu symlinks
# include/ into the flavour dir, so there this stays empty.  The sibling scan
# covers layouts that do neither.
_KBUILD_SRCTREE := $(shell sed -n 's,^include \(.*\)/Makefile$$,\1,p' \
	$(KERNEL_BUILD)/Makefile 2>/dev/null | head -1)
_KBUILD_COMMON := $(strip $(if $(wildcard $(_KBUILD_SRCTREE)/include),$(_KBUILD_SRCTREE),\
	$(shell d=$$(readlink -f $(KERNEL_BUILD) 2>/dev/null); \
		while [ "$$d" != "$${d%-*}" ]; do \
			d=$${d%-*}; \
			for c in "$$d-common" "$$d"; do \
				[ -d "$$c/include" ] && { echo "$$c"; exit 0; }; \
			done; \
		done)))

# $(call compat-hdr,<relative-path>) -> first existing candidate
compat-hdr = $(firstword $(wildcard $(KERNEL_PATH)/include/$(1) $(KERNEL_BUILD)/include/$(1) $(_KBUILD_COMMON)/include/$(1) $(KERNEL_BUILD)/source/include/$(1)))

MANGO_FLAGS += -I$(TOPLEVEL)/libcompat/include

# Some Ubuntu kernel-headers packages ship a modpost that includes these
# headers without shipping them; empty stubs fill in only when one is missing.
ifeq ($(and $(call compat-hdr,linux/elfnote-lto.h),$(call compat-hdr,linux/export-internal.h)),)
override KCFLAGS += -isystem $(TOPLEVEL)/libcompat/kheaders
endif

# Literal parens for probe patterns: make cannot carry unbalanced ( or )
# inside conditionals/function calls, so patterns reference these instead.
lp := (
rp := )

# $(call compat-member-has,<member>,<needle>,<header>) -> y/empty
# Captures the ops-member declaration `(*<member>)(...);` up to its
# terminating ';' (they span lines) and tests it for <needle>.
compat-member-has = $(shell sed -n '/$(lp)\*$(1)$(rp)$(lp)/,/;/p' $(3) 2>/dev/null | grep -Fq '$(2)' && echo y)

# $(call compat-decl-has,<decl-prefix>,<needle>,<header>) -> y/empty
# Same, for a top-level declaration starting with `<decl-prefix>(`.
compat-decl-has = $(shell sed -n '/$(1)$(lp)/,/;/p' $(3) 2>/dev/null | grep -Fq '$(2)' && echo y)

COMPAT_NETDEV_H  := $(call compat-hdr,linux/netdevice.h)
COMPAT_EVENTFD_H  := $(call compat-hdr,linux/eventfd.h)
COMPAT_PLATFORM_H := $(call compat-hdr,linux/platform_device.h)
COMPAT_CLASS_H    := $(call compat-hdr,linux/device/class.h)
COMPAT_SYSFS_H    := $(call compat-hdr,linux/sysfs.h)
COMPAT_DEVICE_H   := $(call compat-hdr,linux/device.h)
COMPAT_DEVLINK_H  := $(call compat-hdr,net/devlink.h)
COMPAT_VIRTIO_CONFIG_H := $(call compat-hdr,linux/virtio_config.h)
COMPAT_SOCKET_H   := $(call compat-hdr,linux/socket.h)
COMPAT_SVC_H      := $(call compat-hdr,linux/sunrpc/svc.h)
COMPAT_SVCSOCK_H  := $(call compat-hdr,linux/sunrpc/svcsock.h)
COMPAT_SVC_XPRT_H := $(call compat-hdr,linux/sunrpc/svc_xprt.h)
COMPAT_SYMVERS    := $(firstword $(wildcard $(KERNEL_BUILD)/Module.symvers $(_KBUILD_COMMON)/Module.symvers))

# --- eventfd ----------------------------------------------------------
# eventfd_signal() lost its counter arg in v6.8 (RHEL 9.5).
ifneq ($(shell grep -Fqs 'eventfd_signal$(lp)struct eventfd_ctx *ctx$(rp)' $(COMPAT_EVENTFD_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_EVENTFD_SIGNAL_NO_COUNTER
endif

# --- platform ---------------------------------------------------------
# platform_driver::remove returns void from v6.11.
ifneq ($(shell grep -Fqs 'void $(lp)*remove$(rp)$(lp)struct platform_device' $(COMPAT_PLATFORM_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_PLATFORM_REMOVE_RETURNS_VOID
endif

# --- driver core ------------------------------------------------------
# class_create() lost its module arg in v6.4 (RHEL 9's 5.14 has the new form).
ifneq ($(shell grep -Fqs 'class_create$(lp)const char' $(COMPAT_CLASS_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_CLASS_CREATE_NO_MODULE
endif

# bin_attribute::read takes const struct bin_attribute * from v6.13 (the
# constification of sysfs attribute ops). Ubuntu jammy and RHEL 9 carry the
# older mutable form.
ifeq ($(call compat-member-has,read,const struct bin_attribute,$(COMPAT_SYSFS_H)),y)
MANGO_FLAGS += -DMANGO_COMPAT_BIN_ATTR_READ_CONST
endif

# class::devnode takes const struct device * from v6.2 (RHEL 9 backport).
ifneq ($(shell grep -Fqs 'devnode$(rp)$(lp)const struct device' $(COMPAT_CLASS_H) $(COMPAT_DEVICE_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_CLASS_DEVNODE_CONST_DEV
endif

# --- netdev -----------------------------------------------------------
# The plain netif_napi_add() only lost its weight arg in v6.1 (v5.19/v6.0
# have _weight AND the 4-arg plain form) — probe the 3-arg form itself:
# capture the prototype up to its opening brace and test for 'weight'.
ifeq ($(shell sed -n '/netif_napi_add$(lp)struct net_device/,/{/p' $(COMPAT_NETDEV_H) 2>/dev/null | grep -Fq 'weight' || echo y),y)
ifneq ($(shell grep -Fqs 'netif_napi_add$(lp)struct net_device' $(COMPAT_NETDEV_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_NETIF_NAPI_ADD_NO_WEIGHT
endif
endif

# --- devlink ----------------------------------------------------------
# devlink_info_driver_name_put() was removed in v6.2; the core fills it.
ifneq ($(shell grep -Fqs 'devlink_info_driver_name_put$(lp)' $(COMPAT_DEVLINK_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_HAS_DEVLINK_DRIVER_NAME_PUT
endif

# devlink_register() became void in v5.16 (RHEL 9 has it).
ifneq ($(shell grep -Fqs 'void devlink_register$(lp)struct devlink' $(COMPAT_DEVLINK_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_DEVLINK_REGISTER_RETURNS_VOID
endif

# devlink_health_reporter_create() lost its u64 graceful_period arg in v6.18
# (d2b007374551); the value now lives in ops->default_graceful_period.
# RHEL 9.8 (5.14) and RHEL 10.2 (6.12) backport it.  Probe the ops member,
# which only exists in the new form.
ifneq ($(shell grep -Fqs 'u64 default_graceful_period;' $(COMPAT_DEVLINK_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_DEVLINK_HEALTH_GRACE_IN_OPS
endif

# --- virtio -----------------------------------------------------------
# find_vqs takes struct virtqueue_info[] from v6.11.
ifneq ($(shell grep -Fqs 'struct virtqueue_info' $(COMPAT_VIRTIO_CONFIG_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_HAS_VIRTQUEUE_INFO
endif

# --- socket -----------------------------------------------------------
# kernel_bind() takes struct sockaddr_unsized from v7.0.
ifneq ($(shell grep -Fqs 'struct sockaddr_unsized' $(COMPAT_SOCKET_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_KERNEL_BIND_TAKES_UNSIZED
endif

# --- sunrpc -----------------------------------------------------------
# The svc API changed shape many times across 5.15 - 7.0 and vendor kernels;
# each probe below keys on one declaration form (see mango_compat/sunrpc.h).
ifeq ($(call compat-decl-has,void svc_destroy,struct svc_serv **,$(COMPAT_SVC_H)),y)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_DESTROY_TAKES_SVCP
endif

ifneq ($(shell grep -Fqs 'sv_refcnt' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_DESTROY_TAKES_KREF
endif

ifneq ($(shell grep -Fqs 'struct svc_serv_ops' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_CREATE_TAKES_OPS
endif

ifneq ($(shell grep -qsw 'svc_xprt_create' $(COMPAT_SYMVERS) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_HAS_SVC_XPRT_CREATE
endif

ifeq ($(call compat-decl-has,int svc_addsock,struct net,$(COMPAT_SVCSOCK_H)),y)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_ADDSOCK_TAKES_NET
endif

ifneq ($(shell grep -Eqs 'void[[:space:]]+svc_recv' $(COMPAT_SVCSOCK_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_RECV_IS_VOID
endif

ifneq ($(shell grep -Eqs 'svc_recv.*long' $(COMPAT_SVCSOCK_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_RECV_TAKES_TIMEOUT
endif

ifneq ($(shell grep -qsw 'svc_process' $(COMPAT_SYMVERS) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_HAS_SVC_PROCESS
endif

ifneq ($(shell grep -Fqs 'svc_thread_should_stop' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_HAS_SVC_THREAD_SHOULD_STOP
endif

ifneq ($(shell grep -Fqs 'svc_thread_init_status' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_HAS_SVC_THREAD_INIT_STATUS
endif

ifneq ($(shell grep -Fqs 'pc_argzero' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_PROC_HAS_ARGZERO
endif

ifneq ($(shell grep -Fqs 'pc_name' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_PROC_HAS_NAME
endif

ifneq ($(shell grep -Eqs 'vs_dispatch.*__be32' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_DISPATCH_TAKES_STATP
endif

ifneq ($(shell grep -Eqs 'enum svc_auth_status.*pg_authenticate' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_PG_AUTH_RETURNS_ENUM
endif

ifneq ($(shell grep -Fqs 'pg_stats' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_PROGRAM_HAS_PG_STATS
endif

ifneq ($(shell grep -Eqs 'unsigned long.*vs_count|vs_count.*unsigned long' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_VS_COUNT_IS_LONG
endif

ifneq ($(shell grep -Eqs 'svc_set_num_threads.*unsigned int min_threads' $(COMPAT_SVC_H) && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_SET_NUM_THREADS_TAKES_INT
endif

ifneq ($(shell grep -A1 -s 'svc_xprt_destroy_all' $(COMPAT_SVC_XPRT_H) 2>/dev/null | grep -q bool && echo y),)
MANGO_FLAGS += -DMANGO_COMPAT_SVC_XPRT_DESTROY_ALL_HAS_UNREGISTER
endif
