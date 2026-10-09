KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
PWD := $(shell pwd)
ARCH := $(shell uname -m)
Target_ko := 
Target_dts := rx8130-overlay
# Module name
obj-m += 

all: $(Target_dts).dtbo # $(Target_ko).ko

$(Target_ko).ko: $(Target_ko).c
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules

$(Target_dts).dtbo: $(Target_dts).dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGS) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dts).dtbo *.tmp

install: all
	if [ -f $(Target_dts).dtbo ]; then cp $(Target_dts).dtbo /boot/firmware/overlays/; fi


.PHONY: all clean install