# M5IO-HUB external modules.
obj-m += m5io-hub-core.o
obj-m += m5io-hub-gpio.o
obj-m += m5hub-gpio-leds.o
obj-m += m5io-hub-i2c.o
obj-m += m5io-hub-spi.o

# M5IO-HUB detailed debug logging.
# 0 = off (default), 1 = on. Enable with: make M5IO_HUB_DEBUG=1
M5IO_HUB_DEBUG ?= 0
ccflags-y += -DM5IO_HUB_DEBUG=$(M5IO_HUB_DEBUG)

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_OUT ?=
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra/m5io
ROOTFSPATH ?=
OVERLAYDIR ?= $(ROOTFSPATH)/boot/firmware/overlays
ARCH ?= $(shell uname -m)
ifeq ($(ARCH),aarch64)
ARCH = arm64
endif
ifeq ($(ARCH),arm64)
CROSS_COMPILE ?=
else
CROSS_COMPILE ?= aarch64-linux-gnu-
endif

DTC ?= dtc
CPP ?= cpp
DTS_INCLUDE ?= $(if $(KERNEL_OUT),$(KERNEL_OUT)/include,$(KERNELDIR)/include)
KBUILD_MODPOST_WARN ?= 1

MODULES := m5io-hub-core m5io-hub-gpio m5hub-gpio-leds m5io-hub-i2c m5io-hub-spi
MODULE_KOS := $(addsuffix .ko,$(MODULES))
DTS_TARGETS := cardputerzeroq-overlay.dtbo spi0-spidev-overlay.dtbo \
	spi0-m5io-hub-runtime.dtbo tca8418-m5io-overlay.dtbo
RULES := 99-cardputerzeroq-keyboard.rules
TCA8418_DIR ?= ../tca8418-1.0

# A complete DTB is intentionally installed only through install-dtb. The
# current QRB2210 boot flow does not dynamically load DTBO files.
DTB ?=
DTB_INSTALL_PATH ?=

KERNEL_BUILD_DIR := $(if $(KERNEL_OUT),$(KERNEL_OUT),$(KERNELDIR))
KERNEL_ARGS := $(if $(KERNEL_OUT),O=$(KERNEL_OUT)) M=$(CURDIR) \
	ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
	KBUILD_MODPOST_WARN=$(KBUILD_MODPOST_WARN)

.PHONY: all module modules dtbs install install-module install-dtbs \
	install-rules install-dtb install-tca8418 clean clean-dtbs help check-kernel

all: module dtbs

module modules: check-kernel
	$(MAKE) -C "$(KERNELDIR)" $(KERNEL_ARGS) modules

dtbs: $(DTS_TARGETS)

%.dtbo: %.dts
	$(CPP) -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I"$(DTS_INCLUDE)" -o $@.tmp $<
	$(DTC) -@ -I dts -O dtb -o $@ $@.tmp

check-kernel:
	@test -f "$(KERNELDIR)/Makefile" || { \
		echo "Kernel source tree not found: $(KERNELDIR)" >&2; \
		echo "Set it with: make KERNELDIR=/path/to/kernel" >&2; \
		exit 1; \
	}
	@test -f "$(KERNEL_BUILD_DIR)/include/config/auto.conf" || { \
		echo "Kernel build files are incomplete: $(KERNEL_BUILD_DIR)" >&2; \
		echo "Set KERNEL_OUT to a prepared kernel output directory." >&2; \
		exit 1; \
	}

install-module: module
	install -d "$(ROOTFSPATH)$(EXTRADIR)"
	for ko in $(MODULE_KOS); do install -m 0644 "$$ko" "$(ROOTFSPATH)$(EXTRADIR)/"; done
	@if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ] && \
		[ -z "$(ROOTFSPATH)" ]; then depmod -a; fi

install-dtbs: dtbs
	install -d "$(OVERLAYDIR)"
	for dtbo in $(DTS_TARGETS); do install -m 0644 "$$dtbo" "$(OVERLAYDIR)/"; done

install-rules:
	install -D -m 0644 "$(RULES)" "$(ROOTFSPATH)/etc/udev/rules.d/$(RULES)"

install-tca8418:
	@test -d "$(TCA8418_DIR)" || { \
		echo "TCA8418 module directory not found: $(TCA8418_DIR)" >&2; \
		exit 1; \
	}
	$(MAKE) -C "$(TCA8418_DIR)" all \
		KERNELDIR="$(if $(KERNEL_OUT),$(KERNEL_OUT),$(KERNELDIR))" \
		ARCH="$(ARCH)" CROSS_COMPILE="$(CROSS_COMPILE)" \
		KBUILD_MODPOST_WARN="$(KBUILD_MODPOST_WARN)"
	install -d "$(ROOTFSPATH)$(EXTRADIR)"
	install -m 0644 "$(TCA8418_DIR)/tca8418_keypad_m5stack.ko" \
		"$(ROOTFSPATH)$(EXTRADIR)/"
	install -D -m 0644 "$(TCA8418_DIR)/tca8418_keypad_m5stack_keymap.map" \
		"$(ROOTFSPATH)/usr/share/keymaps/tca8418_keypad_m5stack_keymap.map"
	install -D -m 0644 "$(TCA8418_DIR)/99-cardputer-zero-keyboard.rules" \
		"$(ROOTFSPATH)/etc/udev/rules.d/99-cardputer-zero-keyboard.rules"
	install -d "$(OVERLAYDIR)"
	install -m 0644 "$(TCA8418_DIR)/tca8418_m5stack.dtbo" "$(OVERLAYDIR)/"

# Install a prebuilt, already merged DTB only when both paths are supplied:
# make install-dtb DTB=merged.dtb DTB_INSTALL_PATH=/boot/efi/dtb/qcom/board.dtb
install-dtb:
	@test -n "$(DTB)" || { echo "Set DTB=/path/to/merged.dtb" >&2; exit 1; }
	@test -n "$(DTB_INSTALL_PATH)" || { echo "Set DTB_INSTALL_PATH=/boot/efi/dtb/qcom/board.dtb" >&2; exit 1; }
	install -D -m 0644 "$(DTB)" "$(ROOTFSPATH)$(DTB_INSTALL_PATH)"

install: all install-module install-dtbs install-rules install-tca8418

clean:
	@if [ -f "$(KERNELDIR)/Makefile" ]; then \
		$(MAKE) -C "$(KERNELDIR)" $(KERNEL_ARGS) clean; \
	fi
	$(RM) $(DTS_TARGETS) $(addsuffix .tmp,$(DTS_TARGETS))

clean-dtbs:
	$(RM) $(DTS_TARGETS) $(addsuffix .tmp,$(DTS_TARGETS))

help:
	@echo "Targets:"
	@echo "  all             Build modules and DT overlays"
	@echo "  module          Build M5IO-HUB kernel modules"
	@echo "  dtbs            Build all DT overlays"
	@echo "  install         Install modules, overlays and udev rules"
	@echo "  install-tca8418 Install the sibling TCA8418 keyboard module"
	@echo "  install-dtb     Install a supplied, already merged complete DTB"
	@echo "  clean           Remove generated files"
	@echo ""
	@echo "Variables:"
	@echo "  KERNELDIR=$(KERNELDIR)"
	@echo "  KERNEL_OUT=$(KERNEL_OUT)"
	@echo "  ARCH=$(ARCH)"
	@echo "  CROSS_COMPILE=$(CROSS_COMPILE)"
	@echo "  ROOTFSPATH=$(ROOTFSPATH)"
	@echo "  EXTRADIR=$(EXTRADIR)"
	@echo "  OVERLAYDIR=$(OVERLAYDIR)"
