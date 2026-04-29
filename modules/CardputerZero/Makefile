KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
ARCH := $(shell uname -m)
Target_ko := 
Target_dts := cardputerzero-overlay
# Module name
obj-m += $(Target_ko).o

all: $(Target_dts).dtbo

$(Target_ko).ko: $(Target_ko).c
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules

$(Target_dts).dtbo: $(Target_dts).dts
	dtc -@ -I dts -O dtb -o $@ $<

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dts).dtbo
install: all
	cp $(Target_dts).dtbo /boot/firmware/overlays/
	echo "dtoverlay=$(Target_dts)" | tee -a /boot/firmware/config.txt
	if [ -d '../py32ioexp-1.0' ] ; then $(MAKE) -C ../py32ioexp-1.0 install ; fi
	if [ -d '../st7789v-1.0' ] ; then $(MAKE) -C ../st7789v-1.0 install ; fi
	if [ -d '../tca8418-1.0' ] ; then $(MAKE) -C ../tca8418-1.0 install ; fi
	if [ -d '../es8389-1.0' ] ; then $(MAKE) -C ../es8389-1.0 install ; fi
	if [ -d '../bq27220-1.0' ] ; then $(MAKE) -C ../bq27220-1.0 install ; fi
	echo "ok"


.PHONY: all clean install