KERNELDIR ?= /lib/modules/$(shell uname -r)/build
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
	dtc -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dts).dtbo
install: all
	mkdir -p /lib/modules/$(shell uname -r)/extra
	cp $(Target_ko).ko /lib/modules/$(shell uname -r)/extra
	mkdir -p /usr/share/keymaps/
	cp tca8418_keypad_m5stack_keymap.map /usr/share/keymaps/
	depmod -a
	loadkeys /usr/share/keymaps/tca8418_keypad_m5stack_keymap.map
# 	echo "dtoverlay=$(Target_dts)" | tee -a /boot/firmware/config.txt


.PHONY: all clean install