KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
ROOTFSPATH ?=
PWD := $(shell pwd)
ARCH := $(shell uname -m)
Target_ko := bq27xxx_battery
Target1_ko := bq27xxx_battery_i2c
Target_dts := bq27220_v5 bq27220_v3
Target_dtbo := $(addsuffix .dtbo,$(Target_dts))

# Module name
obj-m += $(Target_ko).o $(Target1_ko).o 

all: $(Target_ko).ko $(Target_dtbo)

$(Target_ko).ko: $(Target_ko).c
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules

%.dtbo: %.dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGS) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dtbo) *.tmp

install: all
	mkdir -p $(EXTRADIR)
	cp $(Target_ko).ko $(Target1_ko).ko $(EXTRADIR)
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ]; then depmod -a ; fi
	cp $(Target_dtbo) /boot/firmware/overlays/



.PHONY: all clean install