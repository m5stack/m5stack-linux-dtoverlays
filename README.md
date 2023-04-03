# m5stack-linux-dtoverlays

On embedded systems, the [Device Tree](https://elinux.org/Device_Tree_What_It_Is) helps the kernel understand various peripherals that are connected to the board and how to initialize them. These hardware might be things like LDO regulators, various controllers, GPIO, etc which are generic, but yet needs certain configuration that should not be hard-coded into the kernel. To understand more about device trees I recommend you start with the Raspberry Pi [documentation on this topic](https://www.raspberrypi.org/documentation/configuration/device-tree.md). There are more links at the end of this article.




Overlays:
------------

Step 1: Clone this repo:
```sh
git clone https://github.com/m5stack/m5stack-linux-dtoverlays
cd m5stack-linux-dtoverlays
```
Step 2: Install *.dtbo:
```sh
#Select platform by replacing word <platform> to
#one of {cm4stack}.
make all_<platform>
#On cm4stack
sudo make install_cm4stack
```
more:
```sh
@echo "Targets:"
@echo "  all_<PLATFORM>:            Build all device tree binaries for <PLATFORM>"
@echo "  clean_<PLATFORM>:          Clean all generated files for <PLATFORM>"
@echo "  install_<PLATFORM>:        Install all generated files for <PLATFORM> (sudo)"
@echo ""
@echo "  overlays/<PLATFORM>/<DTS>.dtbo   Build a single device tree binary"
@echo ""
@echo "PLATFORMES: jetsonnano bb stm32mp1 rpi imx6ull"

```

## Further Reading
- Device Tree for Dummies: https://elinux.org/images/f/f9/Petazzoni-device-tree-dummies_0.pdf
- Raspberry Pi and the Device Tree: https://www.raspberrypi.org/documentation/configuration/device-tree.md
- Device Tree overlay support in the Linux Kernel: https://www.kernel.org/doc/Documentation/devicetree/overlay-notes.txt
- FDT overlays in U-Boot: https://github.com/u-boot/u-boot/blob/master/doc/README.fdt-overlays

## Modules:
------------
Mainline does not have a kernel module, or there is controversy about a kernel module that does work well. We will also collect them together and put them here.
The kernel modules will have the corresponding documentation and detailed instructions。

## examples:  
-----------
