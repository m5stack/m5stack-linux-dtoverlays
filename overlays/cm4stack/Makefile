install_bin:
	sudo cp ./bin/*.dtbo /boot/overlays
	sudo sed -i '$$a\dtoverlay=m5stack-cm4' /boot/config.txt
	sudo sed -i '$$a\dtoverlay=aw88xx' /boot/config.txt

