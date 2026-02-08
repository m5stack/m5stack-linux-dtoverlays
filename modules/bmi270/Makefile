KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
ARCH := $(shell uname -m)
Target_ko := bmi270_core
Target_ko1 := bmi270_i2c
Target_dts := bmi270_overlay
# Module name

obj-m += $(Target_ko).o  $(Target_ko1).o 

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
	cp $(Target_ko).ko $(Target_ko1).ko /lib/modules/$(shell uname -r)/extra
	depmod -a
# 	cp $(Target_dts).dtbo /boot/overlays
# 	echo "" >> /boot/firmware/config.txt
# 	echo "dtoverlay=$(Target_dts)" >> /boot/firmware/config.txt


.PHONY: all clean install