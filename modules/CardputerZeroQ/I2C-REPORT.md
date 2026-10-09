# M5IO-HUB I2C 适配报告

日期：2026-09-17。目标内核：`6.16.7-g0dd6551ae96b`，ARM64。

## 当前状态

**已使用现有 MCU 固件完成 Linux 驱动适配及已知器件实机验收，无需修改或烧写 MCU 固件。** Linux `m5io-hub-core`、`m5io-hub-gpio`、`m5io-hub-i2c` 及新 DTB 已安装到 UNO Q `3387062622`，重启后两路 I2C 自动注册。实际 M5PM1 RTC RAM 短/长包读写、BMI270 ID 读取及模块卸载/重载通过，UML/KUnit 测试扩展至 18/18 通过。现有固件的扫描误报及实机测试覆盖边界见下文。

主机调试串口为 `/dev/ttyACM0`，对应 `usb-1a86_USB_Single_Serial_58FE005996-if00`，115200、8N1；UNO Q 自身 ADB/CDC 接口当前为 `/dev/ttyACM1`，对应序列号 `3387062622`。此前串口无输出和 ADB 为空时遗漏了电源检查；本轮确认 ESPHome GPIO0=OFF 后按原授权上电，BOOT GPIO2 保持 OFF，随后取得完整 Linux 启动日志并连接 ADB。不能将此前设备未上电表述为 USB/串口故障。设备现在保持上电。串口编号可能随 USB 重枚举变化，实际操作应优先使用 `/dev/serial/by-id/`。

SPI 保持 mode 0、8 bit、20 MHz，CS setup 10 us、inactive 100 us。I2C 时钟是另一层参数，默认 100 kHz，支持通过每路设备树 `clock-frequency` 选择 100000 或 400000。

此前 GPIO 实机测试和旧产物继续保存在 [TEST-REPORT.md](TEST-REPORT.md) 及 `.artifacts/m5io/`，不能作为本轮 I2C 的实机通过证据。

## 协议与实现

权威 MCU 源码基线：仓库 `.slave/cardputerq`，提交 `d6a20f2`。核对了 `FUNC2_I2C/func2_i2c.c`、`USER_I2C/`、`PROTOCOL/` 和 `HANDLE/handle.c`。Linux 驱动按现有 FUNC=2 TOKEN/DATA 协议适配，所有本轮实机测试均使用设备原有 MCU 固件。可选 MCU 源码修改已撤回，MCU 仓库无需合并改动。没有读回并比对运行 MCU 的完整固件，因此不声称运行二进制与源码提交完全相同。

核心驱动新增 `m5io_hub_exec()`，按 TID/FUNC 匹配 TOKEN，返回最终 payload、ARG 和对应 Linux errno。GPIO 同步命令也复用此接口；完成回调在状态锁内执行，避免超时清理与槽位复用之间发生旧回调误完成新请求的竞态。

两路 I2C 在 MCU 中共用一个 FUNC=2 状态机，Linux 核心以同一个 mutex 串行化两路完整 RPC。长写 TOKEN+DATA 原子入队，SPI worker 每个 NSS 仍只发送一帧。DATA 的 PID 固定为 0，CH=bus。长读 DATA 在接收解析路径直接保存，再处理完成 TOKEN，不经过异步回调工作队列，从而保留 MCU 的响应顺序。

**当前固件只支持 FUNC=0..2，没有 FUNC=11 DATA ACK。** I2C 使用最终 FUNC=2 TOKEN 作为事务完成标志，既不分配异步 DATA ACK 窗口，也不向 MCU 发送 FUNC=11。旧异步 DATA 发送及注册接口对 I2C 通道返回 `-EOPNOTSUPP`。

| Linux 操作 | MCU 操作 / 边界 |
| --- | --- |
| 单条写消息，1..12 字节 | WRITE TOKEN 内携带数据 |
| 单条写消息，13..255 字节 | WRITE TOKEN 后跟 PID=0 DATA，等待真实 I2C 完成 TOKEN |
| 单条读消息，1..15 字节 | READ，数据随成功 TOKEN 返回 |
| 单条读消息，16..255 字节 | READ，DATA 后跟完成 TOKEN；两者都有效才发布数据 |
| 同地址写 1/2 字节后读 | MEM_READ，保留 repeated START；16 位内部地址在线上高字节在前，RPC 字段低字节在前 |
| 单条零长度写 | 单地址 DETECT；MCU 报告未应答时返回 `-ENXIO`，扫描存在下述误报限制 |
| 零长度读、超过 255 字节、超过两条消息、其他组合 | `-EOPNOTSUPP`，发送前检查整个消息数组 |
| 10 位地址、NOSTART、IGNORE_NAK、RECV_LEN、STOP 等标志 | `-EOPNOTSUPP`；仅接受 RD 和 DMA_SAFE |
| 保留地址或越界地址 | 仅接受 7 位 `0x08..0x77`，其余 `-EINVAL` |

普通写消息按其原始字节序列发送，包含内部地址的写消息也适用。通用核心接口还可承载原生 MEM_WRITE、CONFIG 和总线扫描，测试覆盖这些 MCU 命令。

SMBus 能力声明包含 byte、byte-data、word-data、write-block-data 和 I2C-block，由 Linux I2C 核心通过上述消息组合模拟。未声明 QUICK、PEC、process-call、可变长度 block-read 或 10 位地址能力；不能假定任意 `I2C_RDWR` 消息组合都能由该固件表示。

状态 2..7 分别映射为 `-EOPNOTSUPP`、`-EINVAL`、`-EBUSY`、`-EALREADY`、`-EBADMSG`、`-EIO`；状态 1 继续等待。当前 MCU 普通读写把 NACK、总线错误及 I2C 超时合并为状态 7，因此 Linux 无法将其细分为不同 errno。

若 RPC 超时，或长读缺失 DATA、DATA 的 PID/通道/长度不匹配、出现重复 DATA，驱动不会发布接收缓冲区，本次请求返回对应错误。事务状态会在请求结束时清理，后续 Linux I2C 请求继续提交；驱动不会自动重发可能已经执行过的写操作。

## 现有固件的扫描限制

实机连续扫描时，I2C1 的五个板载地址稳定存在，但偶尔出现额外地址；I2C2 返回的地址在不同扫描间变化，例如 `0x35/0x50`、`0x4a/0x6f/0x75`、`0x1f`、`0x67`。这些变化地址不能作为真实外设存在的证据，日志见 `probe-i2c.json`、`lifecycle.log` 和 `probe-repeat.log`。

源码中的 `USER_I2C_WAIT_FLAG` 先读取错误标志，再读取完成标志。若 NACK 在两次读取之间与 STOP 一起到达，DETECT 可能把未应答当作成功。此前围绕这一窗口准备的 MCU 修改属于额外修复，已撤回源码及测试改动，并将相关实验产物移出本次交付目录；运行 MCU 固件始终未改写。

本次验收以已知器件的实际数据读写为依据。现有固件已支持所需 I2C 操作，修复扫描误报不作为 Linux 驱动适配或代码合并的前提，也不要求连接 SWD 下载器。扫描限制仍然存在，不能将扫描结果等同于设备身份或数据正确性验证。

2026-09-17 复核：通过 `/tmp/se0-tools/platform-tools/adb` 连接设备 `3387062622`，确认内核 `6.16.7-g0dd6551ae96b`、三个 M5IO 模块、`spi0 -> 4a80000.spi`、`m5io-hub-i2c1 -> i2c-3` 和 `m5io-hub-i2c2 -> i2c-2` 均正常；运行设备树 `spi-max-frequency` 原始值为 `01 31 2d 00`（20 MHz）。当前源码重新强制编译模块成功，独立 KUnit 运行目录为 `.artifacts/m5io/i2c-20260917/kunit-current-1789626122/`，18 项全部通过。

## 总线与设备树

一个 `m5stack,m5io-hub-i2c` platform child 注册两个 `i2c_adapter`，名称分别为 `m5io-hub-i2c1` 和 `m5io-hub-i2c2`。Linux 总线编号动态分配，使用 `i2cdetect -l` 或 `/sys/class/i2c-adapter/i2c-*/name` 确认，不能写死编号。

| MCU 总线 | 引脚 | DT 子节点 | DATA 通道 |
| --- | --- | --- | --- |
| I2C1 | PB8=SCL、PB9=SDA | `i2c/i2c@1`，`reg=<1>` | 1 |
| I2C2 | PB10=SCL、PB11=SDA | `i2c/i2c@2`，`reg=<2>` | 2 |

`spi0-m5io-hub-runtime.dts` 和 `spi0-overlay.dts` 都已启用上述子节点。每个总线节点具有 `#address-cells=<1>`、`#size-cells=<0>`，可在其中描述真实 I2C 外设。改变速率时修改对应节点的 `clock-frequency`，只允许 100000/400000。没有显式总线节点的旧 platform child 默认注册两路 100 kHz adapter。

总线注册前发送 CONFIG 并确认返回成功。adapter 使用 `devm_i2c_add_adapter()` 管理生命周期，DT 总线节点的引用在 adapter 移除后释放。

## 生成物

以下本机路径以仓库根目录 `/home/nihao/work/CardputerZero/m5stack-linux-dtoverlays.cpQ` 为根。新文件统一放在 `.artifacts/m5io/i2c-20260917/`，保留此前 GPIO 的产物。

| 文件 | 目标位置或用途 |
| --- | --- |
| `m5io-hub-core.ko` | 目标 rootfs `/lib/modules/6.16.7-g0dd6551ae96b/extra/m5io/` |
| `m5io-hub-gpio.ko` | 同目录，供 GPIO 回归使用 |
| `m5io-hub-i2c.ko` | 同目录；模块依赖 `m5io-hub-core` |
| `spi0-m5io-hub-runtime.dtbo` | 主机离线合成完整 DTB，当前内核不动态加载 overlay |
| `qrb2210-m5io-hub-i2c.dtb` | EFI 内 `/dtb/qcom/qrb2210-arduino-imola.dtb`；板上 `/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb`，按文件替换，不作为分区镜像烧写 |
| `SHA256SUMS` | 以上生成物及 KUnit 日志的 SHA-256 |
| `build.log`、`kunit.log`、`serial-boot.log`、`serial-i2c-boot.log` | ARM64 构建、18 项测试、上电及安装后重启记录；旧 `serial.log` 为空 |
| `imports.txt`、`available-exports.txt` | 模块符号与此前保存的目标内核导出表核对依据 |
| `backup/` | 本轮安装前的 GPIO DTB、两个模块及哈希；板上备份为 `/home/arduino/m5io-i2c-backup-20260917/` |
| `device-firstboot.log`、`pmic-ram-test.log`、`lifecycle.log`、`lifecycle-dmesg.txt` | 本轮 Linux 驱动实机验收记录 |

已安装完整 DTB 的 SHA-256：`93d97de06645cc8dd384f615864dfe88b916a8f4d9f80b99a6786d0adf91e66a`。本轮没有改动 devcfg、boot 或 qupfw 分区，也未烧写 MCU。本次交付为 Linux 驱动模块、设备树及相应测试记录，不包含 MCU 固件或 MCU 修复补丁。

## 构建与测试

在仓库根运行：

```sh
make -C /home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16 \
  O=/home/nihao/文档/cpQ/work/se0-spi/out/kernel-6.16-build \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  LOCALVERSION=-g0dd6551ae96b M="$PWD/modules/CardputerZeroQ" \
  W=1 KBUILD_MODPOST_WARN=1 modules

make -C modules/CardputerZeroQ \
  DTS_INCLUDE=/home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16/include dtbs

fdtoverlay -i .artifacts/m5io/qrb2210-pre-m5io.dtb \
  -o .artifacts/m5io/i2c-20260917/qrb2210-m5io-hub-i2c.dtb \
  modules/CardputerZeroQ/spi0-m5io-hub-runtime.dtbo

sh modules/CardputerZeroQ/tests/run-kunit.sh \
  /home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16 \
  .artifacts/m5io/kunit-i2c
```

模块编译成功，vermagic 为 `6.16.7-g0dd6551ae96b SMP preempt mod_unload aarch64`。由于缺少目标内核原始 `Module.symvers`，modpost 仍有 unresolved symbol 警告；将新模块全部导入符号与此前保存的目标内核导出表及本轮模块导出表比较，没有缺项。随后目标内核已实际接受三个模块，并通过自动加载和重载验证。

KUnit 脚本复制独立的内核源码到指定输出目录，再接入测试；不修改传入的内核源码。测试直接编译生产 `m5io-hub-core.c`、`m5io-hub-i2c.c` 和 MCU `func2_i2c.c`，模拟 SPI 控制器和 MCU 底层 I2C 函数，使用独立的 CRC 实现。结果为 18/18 通过：

- 两路总线的读写长度 1、12、13、15、16、254、255，核对每次 NSS 只有完整一帧、DATA PID=0。
- 8/16 位内部地址、短/长寄存器读，核对地址端序与数据。
- 无设备 DETECT、实际 MCU 状态映射、IN_PROGRESS 后完成；不支持的消息在发送前拒绝。
- 原生短/长 MEM_WRITE、总线扫描及 400 kHz CONFIG。
- DATA 缺失、CRC 损坏、错误 PID、错误通道、重复 DATA、完成 TOKEN 丢失；失败时读缓冲区不变。
- GPIO 核心 API 回归，以及两线程各自对一路 I2C 做 16 轮 255 字节写后读。
- 通过 Linux SMBus 模拟层实际执行 byte、byte-data、word-data、block-write、I2C-block，核对命令/长度字节和数据端序；验证不支持的调用发送前返回错误。
- OF 路径实际创建两路 100/400 kHz adapter 及其 I2C client，核对 SPI 20 MHz 和 CS 延时；分别进行三轮子驱动重绑定、第二路 CONFIG 失败后的回收与重新探测。

日志含 UML 的 `virtio_pcidev_init: No virtio device ID configured for PCI` 启动告警，来自测试内核未配置 PCI 模拟设备，在测试开始前出现。最终 18 项测试日志没有 M5IO 引用计数/OF 泄漏告警。测试同时覆盖无 DT 的 MFD 路径和 OF 路径，HOST_READY 由排队读取模拟；它不替代目标板电气测试，也没有包含 MCU 底层寄存器轮询，不能据此声称扫描误报已消除。

## 实机结果与验收边界

已取得以下目标板证据：

- 安装后重启，SPI0 属于 `4a80000.spi`，运行 DT 速率为 `20000000`，三个模块自动加载；IRQ125 供 HOST_READY 使用。
- `m5io-hub-i2c1` 当前为 `/dev/i2c-3`，`m5io-hub-i2c2` 为 `/dev/i2c-2`，两路 CONFIG 100 kHz 成功。编号仍须动态查询。
- I2C1 已知板载地址：RX8130 `0x32`、TCA8418 `0x34`、BQ27220 `0x55`、BMI270 `0x68`、M5PM1 `0x6e`。BMI270 寄存器 `0x00` 返回芯片 ID `0x24`。
- M5PM1 的文档定义 `0xa0..0xbf` 为 32 字节 RTC RAM。保存其原始值后，对 1、11、12、13、15、16、31、32 字节各做四轮写入、组合读和独立读，全部一致；最后恢复原有 32 个零字节并完整读回确认。覆盖实际 WRITE TOKEN/DATA 与 READ TOKEN/DATA 分界，最大实测有效数据为 32 字节，255 字节边界仅在 KUnit 中验证。
- I2C 子模块五轮卸载/重载、core/GPIO/I2C 五轮整体卸载/重载均通过；每轮检查两个 adapter 清理及重建、BMI270 ID。整体重载附带五轮共 3,000 次 GPIO 写入/读回；首次启动另有 600 次。最终 tainted=4096，未观察到内核 WARN/BUG 或 M5IO 引用计数错误。启动日志中原有的 PSCI/蓝牙信息不属于 M5IO 通过声明。

以下仍未完成实机验证，保留为后续测试项；不要求为本次适配更新 MCU 固件：

1. 外部 I2C2 的实际数据读写。需先确认真实接线和器件型号，不能依赖目前变化的扫描结果认定器件身份。
2. 实机 400 kHz、255 字节及两路并发测试。当前这部分仅有离线集成测试证据。
3. SCK/I2C 电气波形测量。

回退仅需恢复本轮安装前备份的 GPIO DTB/模块并重启；不要误恢复更早的 I2C 原始 DTB，也不需要回退 DevCfg。
