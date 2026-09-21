KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
ROOTFSPATH ?=
PWD := $(CURDIR)
ARCH ?= $(shell uname -m)
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

Target_ko   := panel-elida-jd9852a
Target_dts  := jd9852a
Target_dtbo := $(addsuffix .dtbo,$(Target_dts))

# Module name
obj-m += $(Target_ko).o

all: $(Target_ko).ko $(Target_dtbo)

$(Target_ko).ko: $(Target_ko).c
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules

%.dtbo: %.dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGE) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

color-test: drm-color-test.c
	$(CC) $(CFLAGS) -o drm-color-test $< $(shell pkg-config --cflags --libs libdrm 2>/dev/null)

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) clean
	rm -f $(Target_dtbo) *.tmp drm-color-test

install: all
	mkdir -p $(ROOTFSPATH)$(EXTRADIR)
	cp $(Target_ko).ko $(ROOTFSPATH)$(EXTRADIR)
# 	cp $(Target_dtbo) $(ROOTFSPATH)/boot/firmware/overlays/
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ] && [ -z "$(ROOTFSPATH)" ]; then depmod -a ; fi

.PHONY: all clean install color-test
