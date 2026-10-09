KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
ROOTFSPATH ?=
PWD := $(CURDIR)
ARCH ?= $(shell uname -m)


kernel_module := magnetometer bmi270
kernel_module_flags  := CONFIG_BMC150_MAGN=m CONFIG_BMC150_MAGN_BMI270=m CONFIG_BMI270=m CONFIG_BMI270_I2C=m
kernel_ko     := 
kernel_dtbo   := bmi270_bmm150_overlay.dtbo bmi270_overlay.dtbo


define kernel_module_template
$(1): $$(wildcard $(1)/*.c)
	$$(kernel_module_flags) $$(MAKE) -C $$(KERNELDIR) M=$$(PWD)/$(1) ARCH=$$(ARCH) modules
	cp $$(PWD)/$(1)/*.ko ./
endef

$(foreach m,$(kernel_module),$(eval $(call kernel_module_template,$(m))))


%.dtbo: %.dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGS) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp


all: $(kernel_module) $(kernel_dtbo)


clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dts).dtbo

install: all
	mkdir -p $(EXTRADIR)
	cp bmi270-init-data.fw $(ROOTFSPATH)/lib/firmware/
	cp ./*.ko $(EXTRADIR) || true
	cp ./*.dtbo /boot/firmware/overlays/  || true
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ]; then depmod -a ; fi


.PHONY: all clean install bmi270 magnetometer
