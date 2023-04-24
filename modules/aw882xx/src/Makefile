CONFIG_AW882XX_DRV:=m

aw882xx_drv-y:= \
	aw882xx.o \
	aw882xx.o \
	aw882xx_monitor.o \
	aw882xx_init.o \
	aw882xx_dsp.o \
	aw882xx_device.o \
	aw882xx_bin_parse.o \
	aw882xx_spin.o		\
#	aw882xx_calib.o 

#ccflags-y += -DDEBUG


obj-$(CONFIG_AW882XX_DRV) += aw882xx_drv.o


LINUX_DIR=KDIR=/lib/modules/$(shell uname -r)/build

all:
		make -C ${LINUX_DIR} M=$(PWD) modules
clean:
		make -C ${LINUX_DIR} M=$(PWD) clean
