# UNO Q M5IO-HUB GPIO 适配与实测报告

日期：2026-09-16。设备：UNO Q，ADB 序列号 `3387062622`，内核 `6.16.7-g0dd6551ae96b`，ARM64。

本文保留 GPIO 阶段的实机结果和产物哈希。2026-09-17 的 I2C 版驱动和 DTB 已在现有 MCU 固件上完成适配，通过已知器件的实际读写及重载测试，无需修改或烧写 MCU 固件。扫描误报保留为已知限制。当前生成物、测试和验收边界见 [I2C 适配报告](I2C-REPORT.md)；下文源码构建命令和哈希对应的是此前 GPIO 版本。

## 结果与范围

Linux 核心驱动和 GPIO 驱动已在 SE0 `spi0.0` 加载，注册 `m5io-hub-gpio`，共 17 条 GPIO。调试时为 `/dev/gpiochip3`；最终安装重启后自动加载，当前为 `/dev/gpiochip2`，普通 `arduino` 用户自测再次通过。应用应按 chip label 查找，不能依赖固定编号。

最终配置为 SPI mode 0、8 bit、20 MHz、片选建立时间 10 us、片选无效时间 100 us。2026-09-16 按用户要求从 1 MHz 提升至 20 MHz，重新生成并安装完整 DTB，重启后运行设备树的 `spi-max-frequency` 为 `20000000`。单轮 600 次及随后 10 轮共 6,000 次 LED set/read 对全部通过，覆盖三个 LED 的顺序和三线程并发操作。此前 1 MHz 配置还通过了 10 轮模块卸载/重载加自测。每轮恢复三个 LED 为高电平（熄灭）。读取通过 MCU 的 GPIO IDR 寄存器返回，非 Linux 缓存或 MCU 输出锁存值。

GPIO1/2 外部短接尚未得到确认，因此未做外部输入回环或 GPIO 边沿事件验收。HOST_READY 传输中断已实际工作，不能将其等同于外部 GPIO 边沿测试。电源使能、MIC_SW 和复位引脚只读了当前电平，未切换输出。UART/I2C/SPI bridge 子功能未启用、未验收。

## 协议依据

MCU 源码：仓库根目录下 `.slave/cardputerq`，提交 `d6a20f2`（优化SPI从机测试）；本次未修改或烧写 MCU 固件。Linux 驱动源码仍在 `modules/CardputerZeroQ`。

核对文件：`code/CardputerQ-APP/Core/User/PROTOCOL/`、`GPIO_CONTROL/`、`HANDLE/handle.c`、`SPI_SlAVE_DRIVER/spi_slave_driver.c`，以及该 MCU 仓库的 `控制协议/控制协议通讯规则.md`。早先 `/home/nihao/文档/cpQ/new/控制协议通讯规则.md` 与当前固件存在差异。

| 项目 | 当前固件约定 |
| --- | --- |
| CRC | CRC16/CCITT-FALSE，初值 FFFF，在线字节序低字节在前；HELLO 为 `02 10 5c 69` |
| 下行帧 | 一次 NSS 仅一帧，长度必须准确，不得拼接命令或在命令后补齐 dummy |
| 上行帧 | 3 或 4 个 FF 前导，允许跨 SPI 读取事务拼接，多个响应连续排队 |
| GPIO 模式 | OUTPUT=0，INPUT=1，INPUT_PULLUP=2，INPUT_PULLDOWN=3 |
| GPIO 响应 | ARG 为状态；成功 digitalRead 为 ARG=0、LEN=1、电平 0/1 |
| GPIO 事件 | FUNC=1、ARG=7、LEN=1，payload 是 GPIO 编号 |

编号：0/1/2 为 GPIO1/2/3（PB7/PB6/PB5）；3/4/5 为 LED1/2/3（低电平亮）；6 为 SD_DET（只输入）；7..16 依次为 MIC_EN、CAMERA_EN、GROVE1_EN、SD_EN、AMP_EN、QWIIC_EN、HAT_EN、GROVE2_EN、MIC_SW、LCD_KEY_RST。

## Linux 改动

1. 使用本目录的 `m5io-hub.h`，修复外部模块构建缺少内核内置头文件的问题；GPIO `.set` 回调改为匹配当前 Linux 6.16 的 void 签名，通信错误输出限速日志。
2. GPIO 数量从 16 改为 17，补齐名称；修正 INPUT/OUTPUT 模式值。输出方向先发 pinMode，再发 digitalWrite：固件拒绝对输入写值，且 LED pinMode 会将其置为熄灭状态。
3. TX FIFO 每次只取一个完整协议帧；纯读取传输才发 FF dummy。保留上行流式组帧；digitalRead 严格检查一个字节且值为 0/1。
4. 有设备树时用 `devm_of_platform_populate()` 注册实际启用的子节点，无设备树时保留 MFD cells 路径。本机 6.16 MFD 路径通过 `device_set_node()` 设置借用的节点指针，随后释放迭代引用，而 platform release 还会 put，重载曾触发引用计数告警。OF 注册路径管理节点引用，并避免创建设备树中不存在的子功能。修正后重启及 10 轮重载无此告警。
5. 新增 `spi0-m5io-hub-runtime.dts`。在已经启用 SE0 的完整 DTB 上，将现有 `spidev@0` 原位改为 `m5stack,m5io-hub`，使用 TLMM GPIO28 低电平 HOST_READY IRQ，只启用 GPIO 子节点。虽然文件名含 runtime，当前内核未启用 CONFIG_OF_OVERLAY，实际使用 `fdtoverlay` 离线合成完整 DTB 后重启加载。
6. 新增 `tests/gpio-selftest.c`，通过 GPIO v2 ioctl 测试，按 label 找芯片，自动读回并恢复 LED 电平。测试结果还覆盖 LED 输入模式拒绝、SD_DET 输出模式拒绝和 SD_DET 输入读取。

## 时序问题与证据

仅修正协议后，20 MHz 无显式片选延时时仍出现间歇超时及错误电平读回；1 MHz 无显式延时的顺序测试通过，但并发测试失败。增加上述片选时序后，先在 1 MHz 完成顺序、并发及重载验证，再提升至 20 MHz 完成顺序及并发压力测试，全部通过。MCU 在 NSS 中断中记录帧边界，在 NSS 空闲时整理和填充 DMA 环形缓存，需要留出处理时间；本次验证了可用参数，未用逻辑分析仪测定最小延时、实际 SCK 波形或证明失效点的唯一电气根因。20 MHz 为本次配置的通信速率，保留 10/100 us 片选时序。

证据位于仓库根目录 `.artifacts/m5io/`（被 git 忽略）：

| 文件 | 内容 |
| --- | --- |
| `gpio-selftest.log`、`gpio-selftest-debug2.log` | 20 MHz 无显式片选延时的历史失败记录 |
| `dmesg-transfer-debug.txt` | 调试版逐帧 TX/RX 记录 |
| `gpio-selftest-1mhz.log` | 未加片选延时的 1 MHz 并发失败 |
| `gpio-selftest-timing.log` | 1 MHz 加片选延时，单轮 600 次通过 |
| `gpio-reload-stress.log` | 1 MHz，10 轮卸载/重载及 6,000 次通过，HOST_READY IRQ 计数 4510 |
| `dmesg-before-lifecycle-fix.txt` | 修正前 OF 引用计数告警 |
| `dmesg-after-reload-stress.txt` | 修正后重载日志；tainted=4096，仅外部模块 O 标志，无 W 告警标志 |
| `build.log` | 模块构建记录 |
| `final-boot-selftest.log`、`dmesg-final-boot.txt` | 1 MHz 安装后重启，自动加载为 gpiochip2，普通用户自测 600 次通过，无 WARN；HOST_READY 分配 IRQ125 |
| `gpio-selftest-20mhz-timing.log` | 20 MHz 安装后重启，运行 DT 属性为 `01 31 2d 00`，普通用户自测 600 次通过 |
| `gpio-stress-20mhz-timing.log` | 20 MHz 加片选延时，10 轮共 6,000 次通过，结束时 IRQ125 计数 4221 |
| `dmesg-20mhz-timing.txt` | 20 MHz 测试后日志，无 WARN；tainted=4096 仅外部模块标记 |

## 生成物与安装位置

本机仓库根：`/home/nihao/work/CardputerZero/m5stack-linux-dtoverlays.cpQ`。

| 生成物 | 本机位置（相对仓库根） | UNO Q 安装位置 / 用途 |
| --- | --- | --- |
| `m5io-hub-core.ko` | `.artifacts/m5io/m5io-hub-core.ko`，构建原件在 `modules/CardputerZeroQ/` | `/lib/modules/6.16.7-g0dd6551ae96b/extra/m5io/m5io-hub-core.ko`，rootfs 文件 |
| `m5io-hub-gpio.ko` | `.artifacts/m5io/m5io-hub-gpio.ko`，构建原件同上 | 同目录 `m5io-hub-gpio.ko`，rootfs 文件 |
| `spi0-m5io-hub-runtime.dtbo` | `.artifacts/m5io/spi0-m5io-hub-runtime.dtbo`，构建原件同上 | 主机离线合成用，不写成独立分区 |
| `qrb2210-m5io-hub.dtb` | `.artifacts/m5io/qrb2210-m5io-hub.dtb` | EFI 分区内 `/dtb/qcom/qrb2210-arduino-imola.dtb`，板上路径 `/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb`，按文件替换 |
| `gpio-selftest` | `.artifacts/m5io/gpio-selftest` | `/home/arduino/m5io-test/gpio-selftest`，用户态测试程序 |
| `qrb2210-pre-m5io.dtb` | `.artifacts/m5io/qrb2210-pre-m5io.dtb` | M5IO 之前已启用 spidev 的 DTB 备份；板上同目录 `qrb2210-arduino-imola.dtb.m5io-pretest-20260916` |
| `qrb2210-m5io-hub-1mhz.dtb` | `.artifacts/m5io/qrb2210-m5io-hub-1mhz.dtb` | 1 MHz M5IO 备份；板上同目录 `qrb2210-arduino-imola.dtb.m5io-1mhz-20260916` |

本轮没有再次修改 devcfg、boot 或 qupfw 分区。此前 SE0 SPI 所需的 DevCfg 生成物及刷写目标仍见 `/home/nihao/文档/cpQ/work/operations-report.md`。`m5hub-gpio-leds.ko` 虽由现有 Makefile 构建，但未作为最终运行模块安装，LED 本次经 GPIO 接口测试。

SHA-256：

```text
41a7797769dc75def2697325fd49c98900d367633b625e4f55a60f4da005600c  m5io-hub-core.ko
361a0b9ef27f6c5109dd6d560018354ac0559666d481c6c7a6d5ebae4b922783  m5io-hub-gpio.ko
3dd9753f8a65134ee7909396b256b527b650477f26055d026858f9fce14c400e  spi0-m5io-hub-runtime.dtbo
0ff834e98f63f18c093236473804a625b63bf235a93b69cdc87a86d759719a3b  qrb2210-m5io-hub.dtb
bcae0b1a44a5faac0d72b54d425e4b1baaa84a86960f368b1b912ee9a36f007d  qrb2210-m5io-hub-1mhz.dtb
18032dc81201290dff0737dad8a599d32e78f39eae98eafeff3fd1614f8ddfe7  qrb2210-pre-m5io.dtb
3a4b5cf383ab20dd83bf8805c2ab0149076bf4329e380d1a6811f954b2b12988  gpio-selftest
```

## 复现步骤

在仓库根目录构建：

```sh
make -C /home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16 \
  O=/home/nihao/文档/cpQ/work/se0-spi/out/kernel-6.16-build \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  LOCALVERSION=-g0dd6551ae96b M="$PWD/modules/CardputerZeroQ" \
  W=1 KBUILD_MODPOST_WARN=1 modules
make -C modules/CardputerZeroQ \
  DTS_INCLUDE=/home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16/include \
  spi0-m5io-hub-runtime.dtbo
fdtoverlay -i .artifacts/m5io/qrb2210-pre-m5io.dtb \
  -o .artifacts/m5io/qrb2210-m5io-hub.dtb \
  modules/CardputerZeroQ/spi0-m5io-hub-runtime.dtbo
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Werror -pthread \
  -o .artifacts/m5io/gpio-selftest modules/CardputerZeroQ/tests/gpio-selftest.c
```

缺少原内核 `Module.symvers`，modpost 会报导出符号警告；此处是针对已核对内核配置/版本、并实际成功正常加载的设备实验构建，未使用 force。正式发布应提供匹配内核的完整构建输出，去掉 `KBUILD_MODPOST_WARN=1` 后构建。

最终安装后运行 `depmod -a 6.16.7-g0dd6551ae96b`，由设备树 modalias 自动加载；也可在板上手动执行 `sudo modprobe m5io-hub-core` 和 `sudo modprobe m5io-hub-gpio`。

板上测试命令（libgpiod 2.2.1）：

```sh
gpiodetect
chip=$(gpiodetect | awk '$2 == "[m5io-hub-gpio]" {print $1}')
test -n "$chip" || exit 1
gpioinfo -c "$chip"
/home/arduino/m5io-test/gpio-selftest
gpioget -c "$chip" 6
gpioset -c "$chip" -t 0 3=0
gpioset -c "$chip" -t 0 3=1
gpioget -a -c "$chip" 3
```

`3=0` 点亮 LED1，`3=1` 熄灭。LED 为输出专用，读取时用 `gpioget -a` 保持方向；libgpiod 本版本 `-c` 使用 chip 路径/编号，不用 label。`.set` 回调为 void，传输失败可能仅体现于内核日志，因此验收使用真实读回比较。固件无原子“方向+初始值”命令，方向切换不能保证无瞬态。

## 恢复与合并

恢复此前 SE0 spidev：在板上将 `/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb.m5io-pretest-20260916` 复制回 `/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb`，`sync` 后重启。不要误用 `.se0-original-20260916`，那是更早的 I2C 原始设备树。安装的 M5IO 模块没有匹配设备时不会绑定。

当前 SE0 被 M5IO 驱动占用，`/dev/spidev0.0` 不再存在，这是预期结果；原 SE5 `/dev/spidev1.0` 保留。不要把原 `spi0-overlay.dts` 与本次 overlay 叠加：原模板还包含未验收的 SD、LED 及其他子功能，且会新建另一个 CS0 节点。

合并内容为驱动源码修正、Makefile 新 overlay 目标、新 DTS、自测 C 源码和本报告。`.slave` MCU 仓库、`.artifacts` 测试证据以及二进制不纳入本仓库提交；部署包应另行包含上述生成物及校验值。本次未执行 git commit 或合并，保留用户原有 staged 修改。
