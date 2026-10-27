# SPDX-License-Identifier: GPL-2.0

TOPLEVEL := $(shell pwd)
KVERSION := $(shell uname -r)
KERNEL_BUILD ?= /lib/modules/$(KVERSION)/build
MANGO_FLAGS :=

# Version string surfaced by modinfo and sysfs.
MANGO_VERSION ?= 1.0.0
MANGO_FLAGS += -DMANGO_DRV_VERSION='"$(MANGO_VERSION)"'

# Kernel API probes, the mango_compat/ headers and the kernel-header stubs
include $(TOPLEVEL)/libcompat/compat.mk

export TOPLEVEL
export KERNEL_BUILD
export MANGO_FLAGS
export MANGO_VERSION
export KCFLAGS

# Every driver directory present in the tree.
MODULES := $(notdir $(patsubst %/,%,$(dir $(wildcard $(TOPLEVEL)/mango_*/Makefile))))

.PHONY: all clean modules_install $(MODULES)

all: $(MODULES)

$(MODULES):
	$(MAKE) -C $(TOPLEVEL)/$@
	cp $(TOPLEVEL)/$@/$(subst _,-,$@).ko $(TOPLEVEL)

clean:
	set -e; $(foreach m,$(MODULES),$(MAKE) -C $(TOPLEVEL)/$(m) clean;)
	rm -f $(TOPLEVEL)/*.ko

modules_install:
	set -e; $(foreach m,$(MODULES),$(MAKE) -C $(TOPLEVEL)/$(m) install;)
