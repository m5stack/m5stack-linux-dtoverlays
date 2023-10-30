# Raspberry kernel 编译

raspberry 使用的内核是在官方的基础上添加了相关默认配置和设备树。



## 本地编译

1、安装软件包依赖

```bash
sudo apt install git bc bison flex libssl-dev make device-tree-compiler
```

2、下载内核源码
```bash
git clone https://github.com/raspberrypi/linux.git
```

3、内核配置
```bash
# 进入源码目录
cd linux

# 查看分支信息
git branch -a

# Raspberry Pi 4的默认配置
make bcm2711_defconfig  

# 如果是Raspberry Pi 2, Pi 3, Pi 3+系列，默认配置：
# make bcm2709_defconfig

# Raspberry Pi 1、Pi Zero、Pi Zero W系列的默认采用配置
# make bcmrpi_defconfig

# 自定义配置
make menuconfig

```

4、内核编译
```bash
# 多线程编译
make zImage modules dtbs -j

# 内核安装,会将压缩过的内核和设备树文件复制到默认路径 /boot 。
make zinstall
# 内核模块安装,默认路径是 / ，内核模块会被复制到 /lib/modules 。
make modules_install
```







## 交叉编译
```bash
sudo apt install git bc bison flex libssl-dev make device-tree-compiler
```

同时需要准备交叉编译工具链，可以下载安装 linaro 版本的工具链，也可以直接使用 apt 下载交叉编译工具链。
```bash
# 32 位 arm 交叉编译工具链安装
sudo apt install gcc-arm-linux-gnueabihf

# 64 位 arm 交叉编译工具链安装
sudo apt install gcc-aarch64-linux-gnu

```

2、下载内核源码
```bash
git clone https://github.com/raspberrypi/linux.git
```

3、内核配置
```bash
# 进入源码目录
cd linux

# 查看分支信息
git branch -a

# Raspberry Pi 4的默认配置
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- bcm2711_defconfig  # 64 位内核
# make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-  bcm2711_defconfig  # 32 位内核

# 如果是Raspberry Pi 2, Pi 3, Pi 3+系列，默认配置：
# make bcm2709_defconfig

# Raspberry Pi 1、Pi Zero、Pi Zero W系列的默认采用配置
# make bcmrpi_defconfig

# 自定义配置
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig # 64 位内核
# make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- menuconfig # 32 位内核
```

4、内核编译
```bash
# 多线程编译
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- zImage modules dtbs -j # 64 位内核
# make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- zImage modules dtbs -j # 32 位内核

# 内核安装,会将压缩过的内核和设备树文件复制到默认路径 /boot 。
make zinstall INSTALL_PATH=../boot
# 内核模块安装,默认路径是 / ，内核模块会被复制到 /lib/modules 。
make modules_install INSTALL_MOD_PATH=../modules
```


## boot 启动配置
在 raspberry 启动 SD 卡中的 boot 目录存在 config.txt 文件。该文件用于控制内核的启动，修改 config.txt 的配置文件种的 kernel 字段,使其指向需要启动内核名字：
```
kernel=kernel-new.img
```















