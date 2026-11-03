# m5pmic

`m5pmic.ko` is an I2C driver for the M5Stack M5PM1 MCU at 7-bit address
`0x6e`.

It verifies `Device_ID=0x50` and `Device_Model=0x20`, registers five
`gpiochip` lines, supports the M5PM1 pull and drive settings through GPIO
pinconf, and exposes labelled `VBAT`, `VIN`, `5VOUT` and the internal die
temperature through hwmon. Voltage inputs are reported in mV, and the
temperature input in millidegrees Celsius. If the device tree supplies a
parent interrupt, the driver creates a GPIO IRQ domain and dispatches the
M5PM1 GPIO status bits to child IRQs.

The driver does not change power, watchdog, timer, system-command or LED
registers during probe. A node can be added below the M5IO-HUB I2C1 bus:

```dts
m5pm1: pmic@6e {
	compatible = "m5stack,m5pm1";
	reg = <0x6e>;
	interrupt-parent = <&tlmm>;
	interrupts = <18 IRQ_TYPE_LEVEL_LOW>;
	status = "okay";
};
```

Build against the board kernel with:

```sh
make -C modules/m5pmic KERNELDIR=/home/nihao/w2T/arduino/linux \
	ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
```
