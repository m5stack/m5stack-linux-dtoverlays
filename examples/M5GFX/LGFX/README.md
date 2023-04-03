# M5GFX / LovyanGFX for CM4Stack

Thanks to [lovyan03](https://github.com/lovyan03) and [IAMLIUBO](https://github.com/imliubo), we can use LovyanGFX on Raspberry Pi even x86 Linux devices coding like Arduino.

And Thanks to [Forairaaaaa](https://github.com/Forairaaaaa) help me testing these examples.

## Linux FrameBuffer

The Linux framebuffer (fbdev) is a linux subsystem used to show graphics on a computer monitor, typically on the system console.

It was designed as a hardware-independent API to give user space software access to the framebuffer (the part of a computer's video memory containing a current video frame) using only the Linux kernel's own basic facilities and its device file system interface, avoiding the need for libraries like SVGAlib which effectively implemented video drivers in user space.

In most applications, fbdev has been superseded by the linux Direct Rendering Manager subsystem, but as of 2022, several drivers provide both DRM and fbdev APIs for backwards compatibility with software that has not been updated to use the DRM system, and there are still fbdev drivers for older (mostly embedded) hardware that does not have a DRM driver.

Firstly, we should install all the dependencies:

```shell
sudo apt update && sudo apt upgrade
sudo apt install build-essential cmake git 
```

Then, clone the repository into any folder you like:

```shell
git clone --recurse-submodules https://github.com/m5stack/m5stack-linux-dtoverlays.git
cd m5stack-linux-dtoverlays/examples/M5GFX/LGFX/CMake_FrameBuffer/
```

It's ready for compiling:

```shell
mkdir build && cd build
cmake ..
make -j4
```

*In Raspberry Pi, especially CM4Stack, when we plug the HDMI, /dev/fb0 will be the HDMI output; SPI display has to be the /dev/fb1 device. So in the demo code when we plug the HDMI, /dev/fb0 could cause some error such as HDMI output failure. Please refer [About the Linux Framebuffer on Raspberry Pi #383](https://github.com/lovyan03/LovyanGFX/issues/383) for futher information.*

## SDL2





