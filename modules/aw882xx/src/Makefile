CONFIG_AW882XX_DRV:=m

aw882xx_drv-y:= \
	aw882xx.o \
	aw882xx_monitor.o \
	aw882xx_init.o \
	aw882xx_dsp.o \
	aw882xx_device.o \
	aw882xx_bin_parse.o \
	aw882xx_spin.o
# aw882xx_calib.o is intentionally left out: it needs the private VFS namespace

#ccflags-y += -DDEBUG


obj-$(CONFIG_AW882XX_DRV) += aw882xx_drv.o


# Point KDIR at the kernel you are building for, e.g.
#   make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
