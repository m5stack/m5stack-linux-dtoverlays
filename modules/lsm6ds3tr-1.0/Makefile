# SPDX-License-Identifier: GPL-2.0-only

ifneq ($(KERNELRELEASE),)

# =========================
# Kernel Kbuild section
# =========================

st_lsm6dsx-y := st_lsm6dsx_core.o st_lsm6dsx_buffer.o \
        st_lsm6dsx_shub.o

obj-m += st_lsm6dsx.o
obj-m += st_lsm6dsx_i2c.o

else

# =========================
# External build section
# =========================

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
PWD := $(shell pwd)

Target_ko := st_lsm6dsx
Target1_ko := st_lsm6dsx_i2c
Target_dts := lsm6ds3tr-overlay

all: modules $(Target_dts).dtbo

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

$(Target_dts).dtbo: $(Target_dts).dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGS) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dts).dtbo *.tmp

install: all
	mkdir -p $(EXTRADIR)
	cp $(Target_ko).ko $(EXTRADIR)
	cp $(Target1_ko).ko $(EXTRADIR)
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ]; then depmod -a ; fi
	if [ -f $(Target_dts).dtbo ]; then cp $(Target_dts).dtbo /boot/firmware/overlays/; fi


.PHONY: all modules clean install

endif