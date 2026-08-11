KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
ROOTFSPATH ?=
PWD := $(shell pwd)
ARCH := $(shell uname -m)
Target_ko := tca8418_keypad_m5stack
Target_dts := tca8418_m5stack
# Module name
obj-m += $(Target_ko).o

all: $(Target_ko).ko $(Target_dts).dtbo

$(Target_ko).ko: $(Target_ko).c
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules

$(Target_dts).dtbo: $(Target_dts).dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGE) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dts).dtbo *.tmp

install: all
	mkdir -p $(EXTRADIR)
	cp $(Target_ko).ko $(EXTRADIR)
	mkdir -p $(ROOTFSPATH)/usr/share/keymaps/
	cp tca8418_keypad_m5stack_keymap.map $(ROOTFSPATH)/usr/share/keymaps/
	mkdir -p $(ROOTFSPATH)/usr/include/cardputerzero/
	cp cardputerzero_keyboard_state.h $(ROOTFSPATH)/usr/include/cardputerzero/
	cp 99-cardputer-zero-keyboard.rules $(ROOTFSPATH)/etc/udev/rules.d/
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ]; then depmod -a ; fi
	if [ -f $(Target_dts).dtbo ]; then cp $(Target_dts).dtbo /boot/firmware/overlays/; fi


.PHONY: all clean install
