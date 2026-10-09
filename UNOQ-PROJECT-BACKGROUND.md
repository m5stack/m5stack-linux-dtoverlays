# UNO Q / CardputerZero 项目背景

> 交接文档 1/2。配套的实施方案见 `UNOQ-PANEL-LANDSCAPE-SOLUTION.md`。
> 编写时间：面板横屏改动评审之后。文中所有"已验证/未验证"均按实际证据标注，不要把未验证项写成通过。

---

## 1. 项目目标

在 **Arduino UNO Q（Qualcomm QRB2210 / qcm2290 家族，机型 `arduino,imola`）** 上，把 M5Stack
CardputerZero 的外设栈跑通：

1. **SE0 SPI**：把 `geniqup@4ac0000` 的 `4a80000` 从官方的 **I2C 模式** 切换为 **SPI 模式**，
   让 `geni_spi` 驱动绑定，得到 `spi0`。
2. **M5IO-HUB**：SE0 SPI 上的从设备（外部 MCU），对外提供 17 路 GPIO、两路 I2C 桥。
3. **TCA8418 键盘**：挂在 M5IO-HUB 的 I2C1 上（地址 `0x34`）。
4. **JD9852 面板**：MIPI-DSI 显示屏，需要从竖屏 170×320 改为横屏 320×170。
5. **产出**：可被 `arduino-flasher-cli` 烧录的固件包。

---

## 2. 硬件平台

### 2.1 QUP / SE0 端点

| 项目 | 值 |
| --- | --- |
| QUP wrapper | `geniqup@4ac0000` |
| SE0 端点地址 | `0x04a80000` |
| SE0 的三个互斥子节点 | `i2c@4a80000` / `serial@4a80000` / `spi@4a80000` |
| 协议读回寄存器 | `GENI_FW_REVISION_RO` @ `0x04a80068`，协议 = `(value >> 8) & 0xff` |
| 协议编号 | `1`=SPI、`2`=UART、`3`=I2C、`255`=未加载 |
| 另一个 SPI | SE5 = `4a94000.spi`（原厂 `spi0` 实际指向它） |

**SoC 引脚**（注意是 SoC GPIO 编号，**不是** Arduino 排针编号）：

| SoC 引脚 | SE0 SPI 功能 |
| --- | --- |
| GPIO0 | MISO (T27) |
| GPIO1 | MOSI (T28) |
| GPIO2 | SCLK (U27) |
| GPIO3 | CS0_N (V27)，低有效 |

**M5IO-HUB 中断**：HOST_READY 走 SoC TLMM **GPIO28**，低电平触发。

### 2.2 显示与键盘

| 外设 | 连接 |
| --- | --- |
| JD9852 面板 | `mdss_dsi0`（`dsi@5e94000`），1 lane，RGB666，`hs_rate=200MHz`，VIDEO_BURST |
| TCA8418 键盘 | M5IO-HUB **I2C1**，地址 `0x34`，中断走 SoC TLMM **GPIO86**，低电平触发 |
| M5IO-HUB I2C1 已知板载地址 | RX8130 `0x32`、TCA8418 `0x34`、BQ27220 `0x55`、BMI270 `0x68`、M5PM1 `0x6e` |

---

## 3. 软件栈与仓库结构

仓库根：`/home/nihao/work/CardputerZero/m5stack-linux-dtoverlays.cpQ`

```
modules/CardputerZeroQ/          # M5IO-HUB 的 Linux 驱动
  m5io-hub-core.c                1936 行，SPI 上的 RPC/TOKEN/DATA 协议、TID 分配、IRQ 线程
  m5io-hub-gpio.c                17 路 GPIO + 中断控制器（含内核 7.0 前后 .set 回调 void/int 兼容）
  m5io-hub-i2c.c                 两个 i2c_adapter（m5io-hub-i2c1/2），CONFIG 100/400kHz
  m5hub-gpio-leds.c              LED 子驱动（会编，但未作为最终运行模块安装）
  m5io-hub-uart.c / spi-m5io-hub.c   已写但 Makefile 注释掉，未编译
  m5io-hub.h                     共享接口定义（M5IO_HUB_NGPIO=17 等）
  spi0-m5io-hub-runtime.dts      把 spidev@0 原位改成 m5stack,m5io-hub，GPIO28 IRQ
  spi0-overlay.dts               更全的版本（含 LED/UART/SPI/SD mmc-slot）
  tca8418-m5io-overlay.dts       键盘挂 I2C1 0x34 + 三套 v5 keymap（各 46 项）
  tests/                         KUnit 18/18 + gpio-selftest.c
  TEST-REPORT.md / I2C-REPORT.md / TCA8418-REPORT.md   三份阶段报告

modules/jd9852a/                 # JD9852 MIPI-DSI 面板
  panel-elida-jd9852a.c          DRM panel 驱动，compatible = "elida,jd9852"
  jd9852a.dts                    面板 overlay（加 panel@0、两个固定 regulator）
  drm-color-test.c               libdrm 彩条测试程序
  now-decompiled.dts             反编译的 7.0 原厂 DTB（参考用）
  Makefile
```

**注意**：原厂 DTB 里**没有** panel 节点（已核实，`grep -c panel` = 0）。
面板完全由 `jd9852a.dtbo` 叠加引入，驱动是外部模块 `panel-elida-jd9852a.ko`。

---

## 4. 设备与操作环境

### 4.1 设备

| 项目 | 值 |
| --- | --- |
| 当前目标设备 | 序列号 `3023797681`，内核 `7.0.0-g122c2c22d838` |
| 早期验证设备 | 序列号 `3387062622`，内核 `6.16.7-g0dd6551ae96b`（SE0 SPI 已跑通） |
| 登录 | 用户名 `arduino`，密码 `arduino` |
| 串口 | `/dev/ttyACM1` = `usb-Arduino_UNO_Q_-_uno-q_3023797681-if01`，115200 8N1 |
| 外部调试串口 | `/dev/ttyACM0` = `usb-1a86_USB_Single_Serial_58FE005996-if00` |

串口编号会随 USB 重枚举变化，**操作时优先用 `/dev/serial/by-id/` 下的稳定路径**。
ADB 工具在 `/tmp/se0-tools/platform-tools/adb`。

### 4.2 电源 / BOOT 控制（ESPHome，`http://192.168.28.32`）

- 设备电源 = `ESP-01S GPIO0`
- BOOT 引脚 = `ESP-01S GPIO2`

```sh
# 开电源
curl -X POST "http://192.168.28.32/switch/ESP-01S%20GPIO0/turn_on"
# 关电源
curl -X POST "http://192.168.28.32/switch/ESP-01S%20GPIO0/turn_off"
# 开 BOOT
curl -X POST "http://192.168.28.32/switch/ESP-01S%20GPIO2/turn_on"
# 关 BOOT
curl -X POST "http://192.168.28.32/switch/ESP-01S%20GPIO2/turn_off"
```

**重启**：关 GPIO0 → 开 GPIO0。

**进入烧录模式**：
1. 关 GPIO0
2. 延时 3s
3. 开 GPIO2
4. 延时 3s
5. 开 GPIO0
6. 延时 10s

**烧录**：
```sh
./arduino-flasher-cli flash <固件.tar.zst>
```

**退出烧录模式**：
1. 关 GPIO0
2. 延时 3s
3. 关 GPIO2
4. 延时 3s
5. 开 GPIO0
6. 延时 10s

### 4.3 关键路径

| 内容 | 路径 |
| --- | --- |
| 烧录器 | `/home/nihao/文档/cpQ/work/arduino-flasher-cli-0.5.1-linux-amd64/arduino-flasher-cli` |
| 官方镜像 | 同目录 `arduino-unoq-debian-image-20260528-558.tar.zst`（2.2 GB） |
| 固件工作区 | `/home/nihao/文档/cpQ/work/firmware-work2`（产出 `arduino-unoq-m5stack.tar.zst`） |
| SE0 SPI 前期工作 | `/home/nihao/文档/cpQ/work/se0-spi/` |
| SE0 SPI 集成报告 | `/home/nihao/文档/cpQ/work/se0-spi-integration-report.md` |
| 原始操作记录 | `/home/nihao/文档/cpQ/work/operations-report.md` |
| 部署前备份 | 仓库内 `.artifacts/deploy-20260918-144908/` |

---

## 5. 构建环境

| 项目 | 值 |
| --- | --- |
| 目标内核树（7.0） | `/home/nihao/w2T/arduino/linux`（= `/home/nihao/文档/cpQ/work/linux-qcom` 的实体） |
| 6.16 内核树 | `/home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16` |
| 内核构建输出 | `/home/nihao/文档/cpQ/work/se0-spi/out/kernel-6.16-build` |
| 交叉编译器 | `aarch64-linux-gnu-gcc`（本机 13.3.0） |
| **必须用的 wrapper** | `.artifacts/aarch64-gcc13-kbuild-wrapper` |

**两个必踩的坑**：

1. 7.0 内核树是用 **gcc 15.2.0** 编的，带 `-fmin-function-alignment=4`，本机的 gcc 13.3.0
   不认这个选项，会直接报 `unrecognized command-line option`。所以必须用 wrapper
   （它只是把这个选项过滤掉）：
   ```sh
   CC=/home/nihao/work/CardputerZero/m5stack-linux-dtoverlays.cpQ/.artifacts/aarch64-gcc13-kbuild-wrapper
   ```
2. 本机是 x86_64，`uname -m` 会给出 `x86_64`。模块必须显式 `ARCH=arm64`。

**可用的模块构建命令**（已验证，产出 vermagic `7.0.0-g122c2c22d838 SMP preempt mod_unload aarch64`）：

```sh
cd modules/jd9852a        # 或 modules/CardputerZeroQ
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CC=$PWD/../../.artifacts/aarch64-gcc13-kbuild-wrapper \
  KERNELDIR=/home/nihao/w2T/arduino/linux
```

构建会报"compiler differs / pahole version differs"的 warning，属正常，可忽略。

---

## 6. SE0 SPI：问题的本质（重要）

**这是两层独立的问题，只改一层无效。**

| 层 | 决定什么 | 由谁决定 |
| --- | --- | --- |
| 早期固件 | SE0 加载**哪一套协议微码**（SPI/I2C/UART） | DevCfg 分区里的 TZ / HYP 平台配置 |
| Linux 设备树 | 启用**哪个控制器驱动**（`geni_spi` / `geni_i2c`） | `i2c@4a80000` / `spi@4a80000` 的 `status` |

**只把设备树里的 `spi@4a80000` 打开是不够的**——如果早期固件加载的是 I2C 微码，
`geni_spi` 探测时会读到协议 3 并打印 `Invalid proto 3`，直接放弃。

### 6.1 已查明的证据

- `qupv3fw.elf`（`/lib/firmware/qcom/qcm2290/qupv3fw.elf`，sha256 `b2bc9910…b31222`）
  内含各协议微码，已导出为 `firmware-work2/out/qup-protocols.json`：
  protocol 1 = SPI master（442 words，fw `0xb02`，sha256 `e21d7c65…fddcb`）、3 = I2C、2 = UART。
- **TZ 内部也内嵌了逐字节相同的 SPI payload**（TZ 文件偏移 `0x125240`）。
  ⇒ **不需要替换 `tz.mbn` / `hyp.mbn` / `qupv3fw.elf`**。
- 真正触发早期协议选择的是 DevCfg 里的协议字段。

### 6.2 两条可选路线

**路线 A：改 DevCfg（6.16 设备上已验证成功）**

把 128 KiB DevCfg 分区镜像里 TZ、HYP 两份 SE0 记录的协议字段 `3` 改成 `1`：

| 文件偏移 | 含义 | 修改 |
| --- | --- | --- |
| `0x9534` | TZ 段 SE0 协议（32 位小端） | `3` → `1` |
| `0xbd48` | HYP 段 SE0 协议 | `3` → `1` |
| `0x1108` | TZ 段 SHA-384（48 字节） | 对 `0x3000` 起、长度 `0x7270` 重算 |
| `0x1138` | HYP 段 SHA-384（48 字节） | 对 `0xb000` 起、长度 `0xf18` 重算 |

- 生成器：`se0-spi/make-devcfg-spi.py`；候选产物：`se0-spi/out/devcfg-spi-rehashed.img`
  （sha256 `646fbb15e476fc33ee107c64ccf4eff9f12dc1d3d2c4aabcdd2e5ababdc94d87`）
- 只写 **A 槽 `devcfg_a`**，保留 `devcfg_b` 不动作为回退。
- ⚠️ **签名边界**：重算摘要 ≠ 重新签名。旧 ECDSA 签名对新镜像无效，本板
  `Secure Boot: Off` 才接受。启用 Secure Boot 的产品必须走正式签名流程。
- ⚠️ **偏移只适用于对应固件基线**。换固件版本必须重新定位偏移、重新验证摘要。

**路线 B：运行时强制 load 微码（本次提示要求重试）**

- Linux 侧 `spi-geni-qcom.c` 只在 `proto == GENI_SE_INVALID_PROTO(255)` 时才调
  `geni_load_se_firmware(se, GENI_SE_SPI)`；`proto=3` 会走 `Invalid proto 3` 分支。
- 现成诊断补丁：`se0-spi/force-se0-spi-firmware.patch`
  —— 增加模块参数 `force_se0_spi_firmware=1`，在满足
  `imola` + `res->start == 0x04a80000` + `proto == GENI_SE_I2C` + `hw_version == 0x30000000`
  且 SE 空闲时，强制重载 SPI 微码并读回校验。
- 补丁后源码：`se0-spi/driver/spi-geni-qcom.c`；脚本：`se0-spi/load-on-device.sh`
  （先 `insmod se0-dt-switch.ko`，再 `insmod spi-geni-se0-test.ko force_se0_spi_firmware=1`）。
- U-Boot 侧对应：`drivers/misc/qcom_geni.c: qcom_geni_load_firmware()`。
- ⚠️ **历史风险**：运行时强制加载曾导致设备复位；另有写 `SE_GENI_CLK_CTRL` @ `0x04a82000`
  的 **HYP data abort** 记录。原因是 HYP 的 `QUP_0S0S` 策略在「SE0 byte17==1 且 mode==2」时
  跳过附加区域授权，导致 `0x04a82000` 不可写。
- ⚠️ **重要疑点**：`se0-spi/logs/default-off-and-restore.txt` 显示当时测试模块实际打印的是
  `Invalid proto 3`（**没有走强制分支**），`force-load-root-container-*.txt` 也没有 dmesg 输出。
  **即强制加载路径很可能从未真正执行成功过** —— 这就是要重试的原因，不是"试过失败了"。

---

## 7. 当前状态与已知问题

### 7.1 已完成

- 6.16 设备 `3387062622`：SE0 SPI 协议切换成功（读回 `0x102` → 协议 1），
  `4a80000.spi` 绑定 `geni_spi`，`/dev/spidev0.0` 可用，内部回环 540 条消息 / 199584 字节通过。
- M5IO-HUB 驱动：GPIO 17 路 + I2C 两路 + KUnit 18/18 通过；6.16 设备上 I2C 实机读写通过。
- 面板驱动：竖屏 170×320 能正常显示（当前状态）。
- 面板 overlay 部署链路：**已验证** `fdtoverlay -i <base>.dtb -o <out>.dtb jd9852a.dtbo`
  可干净套用，产出 `panel@0`（`compatible = "elida,jd9852"`）、`mdp`/`dsi0` = okay、
  `data-lanes = <0>`（单 lane）、endpoint 双向连接正确。

### 7.2 未解决 / 阻塞

1. **7.0 设备 `3023797681` 上 M5IO-HUB 完全没有 SPI 返回**
   - `spi0 -> 4a80000.spi` 正常、速率 20 MHz，`m5io-hub-core` / `m5io-hub-gpio` 自动加载、
     注册 17 路 GPIO；
   - 但 HOST_READY **IRQ 计数为 0**，I2C1 CONFIG 返回 `-ETIMEDOUT`，
     1 MHz / 20 MHz 裸 `spidev` 读回**全 `0xff`**；
   - 因此 `m5io-hub-i2c1` 未注册，TCA8418 未绑定，`/proc/bus/input/devices` 里没有键盘；
   - 断电 3s / 上电 10s 整板电源循环后结果不变；
   - **判断**：指向外部 M5IO-HUB 未供电、接线/CS/引脚不通，或从设备未运行。
     这与 SE0 协议是否为 SPI 是**两件独立的事**，必须分开判断。
2. **面板方向**：当前竖屏 170×320，需改为横屏 320×170（见配套方案文档）。
3. **TCA8418 键盘**：从未取得实机按键事件证据（受 7.1 阻塞）。
4. **I2C2 外部总线**：未做实际数据读写（扫描结果不稳定，不能作为器件存在的证据）。
5. **400 kHz / 255 字节 / 双路并发**：仅有离线 KUnit 证据，无实机证据。

### 7.3 诊断建议顺序

遇到"外设不工作"时，按这个顺序排除，不要跳步：

1. 先确认 **SE0 协议读回 = 1**（`0x04a80068`，`(v>>8)&0xff`）。不是 1 就是固件层问题。
2. 再确认 **`spi0 -> 4a80000.spi`** 且速率正确（`/sys/bus/spi/devices/spi0.0`）。
3. 再确认 **模块加载与 gpiochip 注册**（`gpiodetect`、`dmesg | grep m5io`）。
4. 最后才是**电气/接线层**：裸 `spidev` 读回、HOST_READY IRQ 计数、示波器看 SCK/CS。
