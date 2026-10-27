# SPDX-License-Identifier: GPL-2.0

# The top-level makefile defines required variables and flags.
ifeq ($(MAKELEVEL),0)
  $(error Please use the top-level Makefile to build this driver)
endif

MOD_NAME := mango-mgmt
obj-m += $(MOD_NAME).o

# Include roots the SMI sources name their headers from.
LIBSMI_CFLAGS := -I$(src)/smi -I$(src)/smi/frontend \
                 -I$(src)/smi/backend/transport/fal \
                 -I$(src)/smi/backend/transport/fal/gcq \
                 -I$(src)/smi/backend/transport/fal/rpmsg \
                 -I$(src)/smi/backend/transport/gcq-driver

ccflags-y := -Werror -I$(src) $(MANGO_FLAGS) $(LIBSMI_CFLAGS)

# The build id in this module's version string and in what libsmi sends on the
# wire.
BUILD_ID_OBJS := mod.o smi/backend/smi_v2_cmd.o
include $(TOPLEVEL)/mango_mgmt/smi/build_id.mk

# Every source in the tree, less the .mod.c modpost generates.
BASE_OBJS := $(patsubst $(src)/%.c,%.o,\
  $(filter-out %.mod.c,\
    $(wildcard $(src)/*.c $(src)/*/*.c $(src)/smi/*/*.c $(src)/smi/*/*/*.c \
              $(src)/smi/*/*/*/*.c $(src)/smi/*/*/*/*/*.c)))
$(MOD_NAME)-objs = $(BASE_OBJS)

# Symbols of modules built outside this tree (e.g. by a Yocto recipe).
EXTRA_SYMBOLS := $(KBUILD_EXTRA_SYMBOLS)

all:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) modules KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)"

install:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) modules_install KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)"

clean:
	$(MAKE) -C $(KERNEL_BUILD) M=$(CURDIR) clean
