# drivers/mfd/Makefile
obj-m += m5io-hub-core.o

# drivers/gpio/Makefile
obj-m += m5io-hub-gpio.o

# drivers/leds/Makefile
obj-m += m5hub-gpio-leds.o

# # drivers/tty/serial/Makefile
# obj-$(CONFIG_SERIAL_M5IO_HUB) += m5io-hub-uart.o

# drivers/i2c/busses/Makefile
obj-m += m5io-hub-i2c.o

# # drivers/spi/Makefile
# obj-$(CONFIG_SPI_M5IO_HUB) += m5io-hub-spi.o

DTC ?= dtc
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_OUT ?=
DTS_INCLUDE ?= $(KERNELDIR)/include
DTS_TARGETS := spi0-overlay.dtbo spi0-spidev-overlay.dtbo spi0-m5io-hub-runtime.dtbo tca8418-m5io-overlay.dtbo
KERNEL_ARGS := $(if $(KERNEL_OUT),O=$(KERNEL_OUT)) M=$(CURDIR)

.PHONY: dtbs clean clean-dtbs

dtbs: $(DTS_TARGETS)

%.dtbo: %.dts
	$(CPP) -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I$(DTS_INCLUDE) -o $@.tmp $<
	$(DTC) -@ -I dts -O dtb -o $@ $@.tmp

# Remove the compiled modules (via the kernel tree) and the overlays.
clean:
	@if [ -f "$(KERNELDIR)/Makefile" ]; then \
		$(MAKE) -C "$(KERNELDIR)" $(KERNEL_ARGS) clean; \
	else \
		echo "Kernel tree not found: $(KERNELDIR)" >&2; \
		echo "Removing local build artifacts only" >&2; \
		find . -maxdepth 1 -type f \
			\( -name '*.o' -o -name '*.ko' -o -name '*.mod' \
			-o -name '*.mod.c' -o -name '.*.cmd' \
			-o -name 'modules.order' -o -name 'Module.symvers' \) \
			-delete; \
	fi
	$(RM) $(DTS_TARGETS) $(addsuffix .tmp,$(DTS_TARGETS))

clean-dtbs:
	$(RM) $(DTS_TARGETS) $(addsuffix .tmp,$(DTS_TARGETS))
