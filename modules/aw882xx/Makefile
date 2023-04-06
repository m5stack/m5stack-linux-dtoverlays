install_bin:
	sudo cp ./bin/aw882xx_acf.bin /lib/firmware
	sudo mkdir -p /usr/local/modules
	sudo cp ./bin/aw882xx_drv.ko  /usr/local/modules

start_aw882xx_drv_ko: install_bin
	sudo insmod /usr/local/modules/aw882xx_drv.ko