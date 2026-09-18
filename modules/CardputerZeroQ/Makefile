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
DTS_INCLUDE ?= $(KERNELDIR)/include
DTS_TARGETS := spi0-overlay.dtbo spi0-spidev-overlay.dtbo spi0-m5io-hub-runtime.dtbo tca8418-m5io-overlay.dtbo

.PHONY: dtbs clean-dtbs

dtbs: $(DTS_TARGETS)

%.dtbo: %.dts
	$(CPP) -nostdinc -undef -D__DTS__ -x assembler-with-cpp \
		-I$(DTS_INCLUDE) -o $@.tmp $<
	$(DTC) -@ -I dts -O dtb -o $@ $@.tmp

clean-dtbs:
	$(RM) $(DTS_TARGETS) $(addsuffix .tmp,$(DTS_TARGETS))
