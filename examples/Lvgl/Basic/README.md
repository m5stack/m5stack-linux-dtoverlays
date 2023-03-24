# CM4Stack_LvglDemo

Lvgl Demo for CM4STACK

![](https://github.com/m5stack/m5stack-linux-dtoverlays/blob/main/examples/Lvgl/Basic/pic.png?raw=true)

#### File tree

```
.
├── CMakeLists.txt
├── README.md
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

#### Build

```
mkdir build && cd build
cmake .. && make
```

#### Run

```
./cm4LvglDemo
```
