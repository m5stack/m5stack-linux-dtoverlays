ifneq ($(KERNELRELEASE),)

obj-m := panel-elida-jd9852a.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build
ARCH ?= arm64
ifeq ($(shell uname -m),aarch64)
CROSS_COMPILE ?=
else
CROSS_COMPILE ?= aarch64-linux-gnu-
endif
DTC ?= dtc
PKG_CONFIG ?= pkg-config
HOSTCC ?= cc

MODULE := panel-elida-jd9852a.ko
OVERLAY := jd9852a.dtbo
COLOR_TEST := drm-color-test
DRM_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdrm 2>/dev/null)
DRM_LIBS := $(shell $(PKG_CONFIG) --libs libdrm 2>/dev/null)

.PHONY: all module overlay color-test clean help check-kernel

all: module overlay

module: check-kernel
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" \
		ARCH="$(ARCH)" CROSS_COMPILE="$(CROSS_COMPILE)" modules

overlay: $(OVERLAY)

color-test: $(COLOR_TEST)

$(COLOR_TEST): drm-color-test.c
	@test -n "$(DRM_LIBS)" || { \
		echo "libdrm development files not found (install libdrm-dev)." >&2; \
		exit 1; \
	}
	$(HOSTCC) $(CPPFLAGS) $(CFLAGS) $(DRM_CFLAGS) -O2 -Wall -Wextra \
		-o $@ $< $(LDFLAGS) $(DRM_LIBS)

$(OVERLAY): jd9852a.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

check-kernel:
	@test -f "$(KDIR)/Makefile" || { \
		echo "Kernel tree not found: $(KDIR)" >&2; \
		echo "Set it with: make KDIR=/path/to/kernel" >&2; \
		exit 1; \
	}
	@test -f "$(KDIR)/include/config/auto.conf" && \
	 test -f "$(KDIR)/include/generated/autoconf.h" || { \
		echo "Kernel build files are incomplete: $(KDIR)" >&2; \
		echo "Install headers for the running kernel or prepare the kernel tree." >&2; \
		exit 1; \
	}

clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" \
		ARCH="$(ARCH)" CROSS_COMPILE="$(CROSS_COMPILE)" clean
	$(RM) $(OVERLAY)
	$(RM) $(COLOR_TEST)

help:
	@echo "Targets:"
	@echo "  all      Build $(MODULE) and $(OVERLAY) (default)"
	@echo "  module   Build the panel kernel module"
	@echo "  overlay  Build the device-tree overlay"
	@echo "  color-test  Build the DRM/KMS color-bar test"
	@echo "  clean    Remove generated files"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR=$(KDIR)"
	@echo "  ARCH=$(ARCH)"
	@echo "  CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "  DTC=$(DTC)"

endif
