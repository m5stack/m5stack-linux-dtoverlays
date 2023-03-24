# CM4Stack_LvglDemo

Lvgl simple clock demo for CM4STACK

![](https://github.com/m5stack/m5stack-linux-dtoverlays/blob/main/examples/Lvgl/SimpleClock/pic.png?raw=true)

#### File tree

```
.
├── CMakeLists.txt
├── CopyLib.sh
├── README.md
├── UI
├── lv_conf.h
├── lv_drv_conf.h
├── lv_porting
├── lvgl
├── main.cpp
├── pic.png
├── tick.c
└── tick.h
```

#### Update submodules

```shell
git submodule init
git submodule update
```

#### Install SDL2

```
sudo apt update
sudo apt install libsdl2-dev
```

#### Copy the Lvgl related library from example/Basic

```shell
# Run copy script
./CopyLib.sh

# Or Manually
cp -r ../Basic/lv_porting .
cp -r ../Basic/lvgl .
```

#### Build

```shell
mkdir build && cd build
cmake .. && make
```

#### Run

```shell
./cm4LvglDemo
```
