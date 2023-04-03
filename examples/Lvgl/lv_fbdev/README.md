# lv_fbdev
lvgl linux framebuffer project template

add r329 support. demo for meter.
``` bash
./demo
```
![](./demo.jpg)

add cm4stack support. demo for meter.
``` bash
./demo /dev/fb$(cat /proc/fb | grep fb_st7789v | awk '{print $1}')  /dev/input/event$(cat /proc/bus/input/devices | grep fe205000.i2c | tail -c 2)
```