# SPDX-License-Identifier: GPL-2.0
#
# The build id the module stamps into its version string and sends on the
# wire. A generated header rather than a -D, so a new id recompiles only the
# objects that include it, and filechk rewrites it only on new content.
#
# Set BUILD_ID_OBJS to the objects that include it, and include this after the
# module's `ccflags-y :=` / `clean-files :=` assignments.

# Named by the commit, or -- no git in a source copy -- by a hash of the
# package version, so any version string yields a valid id; 0 where a
# build carries neither.
MANGO_GIT_HASH := $(shell git -C $(src) log -1 --format=%H 2>/dev/null | cut -c1-8)
MANGO_PKG_CODE := $(if $(MANGO_VERSION),$(shell printf '%s' '$(MANGO_VERSION)' | \
  cksum | awk '{ printf "%08x", $$1 }'))
MANGO_BUILD_ID := $(or $(MANGO_GIT_HASH),$(MANGO_PKG_CODE),0)

define filechk_build_id
	printf '/* SPDX-License-Identifier: GPL-2.0-only */\n'; \
	printf '/* Generated: how this build is named -- see smi/build_id.mk. */\n'; \
	printf '#ifndef SMI_BUILD_ID_H\n#define SMI_BUILD_ID_H\n'; \
	printf '#define MANGO_BUILD_ID 0x%sU\n' '$(MANGO_BUILD_ID)'; \
	printf '#define MANGO_BUILD_ID_STR "%s"\n' '$(MANGO_BUILD_ID)'; \
	printf '#endif\n'
endef

# Only in the kbuild pass, which $(obj) marks: KERNELRELEASE may be passed on
# the make command line, so it is set in the wrapper pass too.
ifneq ($(obj),)
ccflags-y   += -I$(obj)
clean-files += smi_build_id.h

$(obj)/smi_build_id.h: FORCE
	$(call filechk,build_id)

$(addprefix $(obj)/,$(BUILD_ID_OBJS)): $(obj)/smi_build_id.h
endif
