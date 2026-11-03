# SPDX-License-Identifier: GPL-2.0-only

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_OUT ?=
ARCH ?= $(shell uname -m)
CROSS_COMPILE ?=

ifeq ($(ARCH),aarch64)
ARCH := arm64
endif

obj-m += m5pmic.o

KBUILD_ARGS := $(if $(KERNEL_OUT),O=$(KERNEL_OUT)) M=$(CURDIR) \
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)

.PHONY: all module clean help

all: module

module:
	$(MAKE) -C "$(KERNELDIR)" $(KBUILD_ARGS) modules

clean:
	@if [ -f "$(KERNELDIR)/Makefile" ]; then \
		$(MAKE) -C "$(KERNELDIR)" $(KBUILD_ARGS) clean; \
	fi

help:
	@echo "make KERNELDIR=/path/to/linux [KERNEL_OUT=/path/to/output]"
