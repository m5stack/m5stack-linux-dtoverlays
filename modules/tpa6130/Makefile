KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
PWD := $(shell pwd)
ARCH := $(shell uname -m)
Target_ko := es8389_m5stack
Target_dts := es8389_m5stack
# Module name
obj-m += $(Target_ko).o

all: $(Target_ko).ko $(Target_dts).dtbo

$(Target_ko).ko: $(Target_ko).c
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules

$(Target_dts).dtbo: $(Target_dts).dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGS) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dts).dtbo *.tmp

install: all
	mkdir -p $(EXTRADIR)
	cp $(Target_ko).ko $(EXTRADIR)
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ]; then depmod -a ; fi
# 	echo "dtoverlay=$(Target_dts)" | tee -a /boot/firmware/config.txt


.PHONY: all clean install