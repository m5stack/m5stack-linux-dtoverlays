# M5STACK CM4STACK Board Support Guide

> Please note that this document describes the board-level modifications and support for the CM4STACK product produced by M5STACK. Please read and use accordingly.

[CM5STACK](https://docs.m5stack.com/en/core/CM4Stack):
![](https://static-cdn.m5stack.com/resource/docs/products/core/CM4Stack/img-f1f00c1e-789c-4be2-b78f-4570c8732739.webp)

## Product Overview:
The CM4STACK is a small, highly integrated, desktop-level, high-performance computing platform developed based on the Raspberry Pi CM4. It provides powerful performance with the four-core 1.5GHz Cortex-A72 core processor BCM2711, and the metal base and strong fan suppression eliminates worries about device heat dissipation. The ST7789V2 high-definition display sub-screen eliminates worries about black box issues. The powerful AW88298 audio amplifier provides the ultimate 4D surround sound experience. With high performance and small size, you can handle daily computing needs and industrial 7 * 24 operation without any pressure.

## Board Support

### System Support
The company currently provides 32-bit and 64-bit Raspberry Pi OS (Raspbian) systems for users to download and install. If you need support for other systems, method one is to refer to the following methods to support yourself. Method two is to contact the company for technical support.

### Device Tree Support
```
├── overlays
│ └── cm4stack
│ ├── bin
│ │ ├── aw88xx.dtbo # Device tree support for screen, touch, rtc
│ │ └── m5stack-cm4.dtbo # Device support for AW88298 amplifier
```
File location: /boot/overlays
> Device tree files are universal and can be used by any system.

### Kernel Module Support
```
├── modules
│ └── aw882xx
│ ├── bin
│ │ ├── aw882xx_acf.bin # Binary file recommended to be placed in /lib/firmware
│ │ └── aw882xx_drv.ko # Kernel driver recommended to be placed in /usr/local/modules
```
> Note that the binary configuration file is platform-independent, but the kernel driver file needs to distinguish between 32-bit and 64-bit systems.  
> Warning: Do not use the `modprobe` command to load the `aw882xx_drv.ko` file, otherwise irreversible errors will occur. Currently, only re-burning the image can solve this error.  

### Boot File Support
```
├── overlays
│ └── cm4stack
│ ├── cmdline.txt
│ ├── config.txt
```
This file is for boot reference, and users can directly replace or refer to modify.
