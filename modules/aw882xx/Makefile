install_bin:
	sudo cp ./bin/aw882xx_acf.bin /lib/firmware
	sudo cp ./bin/aw882xx_drv.ko  /lib/modules/$(uname -r)
	sudo depmod -a && sudo modprobe aw882xx_drv