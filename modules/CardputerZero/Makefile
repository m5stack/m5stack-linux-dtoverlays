KERNELDIR ?= /lib/modules/$(shell uname -r)/build
EXTRADIR ?= /lib/modules/$(shell uname -r)/extra
ROOTFSPATH ?=
PWD := $(CURDIR)
ARCH ?= $(shell uname -m)

# ifndef CONFIG_CARDPUTERO_V0_3
# CONFIG_CARDPUTERO_V0_5=y
# endif


kermel_ko := powerfail_suo.ko cardputerzero-audio.ko gpio-forwarder.ko

# Multiple dts targets, separated by spaces; omit the .dts suffix
Target_dts := cardputerzero-v5-overlay cardputerzero-v3-overlay spk-gpio24-high-overlay spi0-spidev2-gpio22-overlay camera-gpio16-high-overlay camera-py12-high-overlay mic-pyg4-high-overlay

Target_dtbo := $(addsuffix .dtbo,$(Target_dts))

ifeq ($(CONFIG_CARDPUTERO_V0_5),y)
DTS_CC_FLAGE := -DCONFIG_CARDPUTERO_V0_5=1
export CONFIG_CARDPUTERO_V0_5
export DTS_CC_FLAGE
endif

obj-m += $(patsubst %.ko,%.o,$(kermel_ko))

all: $(Target_dtbo) kermel_ko_t

kermel_ko_t: $(patsubst %.ko,%.c,$(kermel_ko))
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ARCH=$(ARCH) modules

%.dtbo: %.dts
	cpp -nostdinc -undef -x assembler-with-cpp $(DTS_CC_FLAGE) $< $<.tmp
	dtc -@ -I dts -O dtb -o $@ $<.tmp

install_dts: $(Target_dtbo)
	cp $(Target_dtbo) /boot/firmware/overlays/

install_ko: $(Target_dtbo)
	mkdir -p $(EXTRADIR)
	cp ./*.ko $(EXTRADIR) || true
	if [ "$(KERNELDIR)" = "/lib/modules/$(shell uname -r)/build" ]; then depmod -a ; fi

install_led_permissions:
	mkdir -p $(ROOTFSPATH)/usr/lib/tmpfiles.d/
	mkdir -p $(ROOTFSPATH)/usr/lib/udev/rules.d/
	install -m 0644 cardputerzero-leds.conf $(ROOTFSPATH)/usr/lib/tmpfiles.d/cardputerzero-leds.conf
	install -m 0644 60-cardputerzero-leds.rules $(ROOTFSPATH)/usr/lib/udev/rules.d/60-cardputerzero-leds.rules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(Target_dtbo) *.tmp

install: all install_dts install_ko
	if [ -d '../bq27220-1.0' ] ; then $(MAKE) -C ../bq27220-1.0 install ; fi
	if [ -d '../es8389-1.0' ] ; then $(MAKE) -C ../es8389-1.0 install ; fi
	if [ -d '../m5ioe1-1.0' ] ; then $(MAKE) -C ../m5ioe1-1.0 install ; fi
	if [ -d '../st7789v-1.0' ] ; then $(MAKE) -C ../st7789v-1.0 install ; fi
	if [ -d '../tca8418-1.0' ] ; then $(MAKE) -C ../tca8418-1.0 install ; fi
ifeq ($(CONFIG_CARDPUTERO_V0_5),y)
	if [ -d '../imu-1.0' ] ; 		then cd ../imu-1.0 ; $(MAKE)  install ; fi
# 	if [ -d '../lsm6ds3tr-1.0' ] ; then $(MAKE) -C ../lsm6ds3tr-1.0 install ; fi
endif
	echo "ok"

config_setup: install_led_permissions
	[ -f "/opt/rpi-config.py" ] || curl -fsSL -o "/opt/rpi-config.py" -L "https://gist.githubusercontent.com/dianjixz/16b2fabbc4a2a573a7c10f212d87f282/raw/rpi-config.py"
	python3 /opt/rpi-config.py --no-backup set camera_auto_detect 0
	python3 /opt/rpi-config.py --no-backup add dtoverlay imx219
	python3 /opt/rpi-config.py --no-backup set dtparam=i2c_arm on
	python3 /opt/rpi-config.py --no-backup set dtparam=i2s on
	python3 /opt/rpi-config.py --no-backup set dtparam=spi on
	python3 /opt/rpi-config.py --no-backup set dtparam=audio off
	python3 /opt/rpi-config.py --no-backup section-set all enable_uart 1
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay dwc2

	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay spk-gpio24-high-overlay
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay lsm6ds3tr-overlay
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay pwm-ir-tx,gpio_pin=12,func=4
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay cardputerzero-overlay
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay cardputerzero-v3-overlay
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay cardputerzero-v5-overlay
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay camera-gpio16-high-overlay
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay bq27220
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay bq27220_v3
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay bq27220_v5
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay gpio-ir,gpio_pin=13,gpio_pull=up
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay gpio-ir-tx,gpio_pin=12
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay bmi270_overlay
	python3 /opt/rpi-config.py --no-backup section-del all dtoverlay bmi270_bmm150_overlay

ifeq ($(CONFIG_CARDPUTERO_V0_5),y)
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay cardputerzero-v5-overlay
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay bq27220_v5
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay bmi270_bmm150_overlay
else
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay cardputerzero-v3-overlay
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay bq27220_v3
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay spk-gpio24-high-overlay
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay bmi270_overlay
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay camera-gpio16-high-overlay
endif
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay gpio-ir,gpio_pin=13,gpio_pull=up
	python3 /opt/rpi-config.py --no-backup section-add all dtoverlay gpio-ir-tx,gpio_pin=12


.PHONY: all clean install install_dts install_ko install_led_permissions config_setup
