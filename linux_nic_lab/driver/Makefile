# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree Kbuild Makefile for the edunic educational PCIe NIC driver.
#
# Usage:
#   make            # build edunic.ko against the running kernel's headers
#   make KDIR=/path/to/kernel/build   # build against a different kernel tree
#   make clean
#   make install    # install to /lib/modules/$(uname -r)/extra and depmod
#   make load       # insmod the freshly built module
#   make unload     # rmmod it
#
# This follows the standard two-pass Kbuild pattern every out-of-tree
# Linux kernel module uses: this Makefile, run directly by the user, just
# re-invokes the kernel's own top-level Makefile with M=<this dir>, which
# is what actually knows how to build a .ko with the right compiler flags,
# module versioning, etc.

obj-m += edunic.o
edunic-y := nic_main.o nic_pci.o nic_hw.o nic_tx.o nic_rx.o nic_irq.o

KVERSION ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVERSION)/build
PWD  := $(shell pwd)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# `make C=1` (sparse) is the recommended way to check __iomem correctness
# on the MMIO accessors in nic_hw.c -- see the README section on why
# readl()/writel() are used instead of raw pointer dereferences.
sparse: clean
	$(MAKE) -C $(KDIR) M=$(PWD) C=2 CF="-D__CHECK_ENDIAN__" modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -A

load: modules
	sudo insmod edunic.ko

unload:
	sudo rmmod edunic

.PHONY: all modules sparse clean install load unload
