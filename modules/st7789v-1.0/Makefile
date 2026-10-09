KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
ROOTFSPATH ?=
PWD := $(CURDIR)
ARCH ?= $(shell uname -m)
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra


kernel_module := 
kernel_module_flags  := 
kernel_ko     := st7789v_m5stack.ko pwm_bl_m5stack.ko panel-mipi-dbi-m.ko
kernel_dtbo   := st7789v_m5stack.dtbo

all:  $(kernel_dtbo) kernel_ko_t $(kernel_module) fb_load

fb_load: fb_load.c
	$(CC) $(CFLAGS) -o $@ $<

obj-m += $(patsubst %.ko,%.o,$(kernel_ko))

%.dtbo: %.dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGS) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

kernel_ko_t: $(patsubst %.ko,%.c,$(kernel_ko))
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules


clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(kernel_dtbo) *.tmp fb_load

install: all
	mkdir -p $(EXTRADIR)
	cp cardputerzero,st7789v_lcd.bin $(ROOTFSPATH)/lib/firmware/
	cp 99-cardputer-zero-lcd.rules $(ROOTFSPATH)/etc/udev/rules.d/
	cp fb_load.service $(ROOTFSPATH)/etc/systemd/system/
	ln -sfv ../fb_load.service $(ROOTFSPATH)/etc/systemd/system/sysinit.target.wants/fb_load.service
	cp fb_load $(ROOTFSPATH)/usr/sbin/
	cp ./*.ko $(EXTRADIR) || true
	cp ./*.dtbo /boot/firmware/overlays/  || true
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ]; then depmod -a ; fi


.PHONY: all clean install bmi270 magnetometer
