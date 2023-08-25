# M5STACK_LINUX_LIBS

`dianjixz_libs` 一个公开的/自由的 linux c/c++ 应用开发库，提供的丰富的库和编程示例，它是一个最小依赖框架。可以在 unix 环境中编译本机和嵌入式平台的 c/c++ 项目。

## 项目目录简介

介绍项目的主要目录以及作用。

``` bash
dianjixz-lib/
├── components #组件库根目录，这是构成项目的重要组成部分，可以选择性的将库编译到项目中去，从而达到最小依赖和最小可执行文件体积。
    ├── component1  # 组件库模板，可以依照该模板快速添加组件库
    ├── hv          # 添加的 libhv 组件库
    ├── lvgl_component  # 添加的 libhv 组件库
    └── DeviceDriver   # 添加的 linux 硬件组件库
        ├── include # 头文件的 include 目录
        ├── party
            ├── framebuffer # linux framebuffer 开发的简单库
            ├── linux_i2c   # linux i2c 硬件使用库
            ├── linux_spi   # linux spi 硬件使用库
            ├── linux_uart  # linux uart 硬件使用库
            ├── ptmx        # linux pts虚拟终端库
    ...
├── doc     # 文档目录
├── examples    # 大量的开发示例库，同时项目的源代码也会在该目录中。
│   ├── demo1   # 项目的模板，可以复制该目录，快速建立一个开发工程。
    ...
├── github_source # github 源码存放位置
│   ├── source-list.sh # github 源码仓库索引，需要手动下载文件到该目录。该目录除了 source-list.sh 文件外，其他的文件不会被纳入仓库的记录范围。
│   ├── libhv   # libhv源码
└── tools   # 脚本和工具的存放目录
    ├── cmake   # cmake 编译脚本
    ├── config_defaults_cross.mk    # 交叉平台的默认配置，主要用于交叉编译的 sdk 设置。
    ├── kconfig # make menuconfig 的主要库目录
    ├── Makefile.mk # 通用 make 操作的实现。
```

想要使用一个框架，首先了解框架的主要目录结构是基础，然后开始编译一个 hello world! 程序作为我们的开始。  
框架依赖 make cmake python3 expect 软件包。请确保安装上述软件包，
``` bash
# 安装依赖(只需要安装一次)
sudo apt update
sudo apt install make cmake python3 expect

# 假定你目前的终端在 dianjixz-lib 目录。进入 examples/demo1
cd examples/demo1

# 开始编译,有时可能会出现编译失败的情况，使用 `make distclean` 可以彻底清理项目编译文件。
make

# 执行完 make 命令后，项目将会完成编译，生成的可执行文件和相关依赖库会被复制到 dist 目录向。可以选择 make 运行或者手动运行
make run    # make 运行测试
# cd dist;./demo1   # 手动运行测试

# 清理编译文件
make clean

# 彻底清理编译文件，包括 make menuconfig 生成的编译项
make distclean
```

## 特性

- 跨平台（Linux, Windows, macOS）
- 高性能软件库,libhv,mongoose,stb,Cimg 等等可以让你迅速使用的库.
- 拥有各种库的使用示例,不用辛苦在网上找库,下载库,安装库,然后寻找库的使用方法.简单的库,代码及文档,复杂的库直接使用示例.
- TCP/UDP服务端/客户端等示例
- 可靠UDP支持: WITH_KCP 示例
- WebSocket服务端/客户端示例
- MQTT客户端示例
- c/c++混合开发
- 快速交叉编译. make set_arm 一个命令切换交叉编译器,享受无缝交叉编译.
- 最小依赖,框架内组件的依赖自动打包,可选动态依赖和静态依赖,告别复杂依赖的烦恼.
- 最小执行程序的设置,开启 release 编译后可生成最小可执行文件尺寸.
- 编译过程全可控.verbose 选项可显示出编译过程的每一个命令. 


## 更多的示例

在项目的 examples 目录下,拥有多线程，多线程同步锁，守护进程，动态库调用，linux 硬件总线开发，lvgl gui， u8g2 驱动 sh1107 等众多示例，可以随时在其他仓库中复制代码添加到自己的项目中.

## 编译本机程序
``` bash
# 进入工作目录
cd examples/demo1
# 编译
make
# 测试运行程序
make run
```

## 交叉编译程序
``` bash
# 进入工作目录
cd examples/demo1
# 设置交叉编译工具链
make set_arm
# 编译
make
# 使用 scp 上传 
make push
# 使用 ssh 命令运行程序
# make push_run
```
## 相关命令
``` bash
# 清理编译文件
make clean
# 彻底清理编译文件
make distclean
# 编译优化版程序（可能出现异常）
make release
# 输出详细的编译过程
make verbose
# 打开或者关闭自带的库
make menuconfig
```

## 更多信息

[]()