# TCA8418 M5IO I2C1 适配报告

日期：2026-09-17，连接复核：2026-09-18。目标设备：Arduino UNO Q / QRB2210，目标内核 `6.16.7-g0dd6551ae96b`（ARM64）。

## 当前交接摘要（2026-09-20）

本次工作按交付项统计已完成 8 个子任务：SE0 → SPI0 的 `devcfg_a` 配置、SPI0 设备树与 20 MHz 参数、M5IO-HUB SPI/GPIO/I2C 驱动适配、Linux 7.0 模块兼容性构建、TCA8418 I2C1 设备树、SoC GPIO86 中断与键盘矩阵、LED 功能屏蔽及自动加载配置、设备部署与报告整理。最终的“键盘产生输入事件”尚未验收通过。

当前实机基线为 UNO Q，ADB 序列号 `3023797681`，内核 `7.0.0-g122c2c22d838`。`devcfg_a` 已写入并校验，SHA-256 为 `646fbb15e476fc33ee107c64ccf4eff9f12dc1d3d2c4aabcdd2e5ababdc94d87`；`devcfg_b` 未修改。实际启动 DTB 为 `qrb2210-arduino-imola-jd9852.dtb`，当前 DTB SHA-256 为 `3e9c54ac74102ad789cccd8464522ca471167486ae1465062eef6b725ef76cf8`。已确认 `spi0 -> 4a80000.spi`、SPI 20 MHz、`m5io-hub-core` 和 `m5io-hub-gpio` 加载，并注册 17 路 GPIO。

TCA8418 目标配置已经落地：M5IO I2C1、地址 `0x34`、SoC GPIO86 低电平中断、5×10 键盘矩阵；目标设备不需要 LED，设备树已删除三个 LED GPIO 和三个 LED mode 属性。I2C 使用节点绑定，不依赖动态总线号 `i2c-3`。生成物和 7.0 版本模块、设备树、安装脚本位于 `.artifacts/m5io/tca8418-20260918-7.0/`，部署前备份位于 `.artifacts/deploy-20260918-144908/`。

当前阻塞点是外部 M5IO-HUB 没有响应，而不是已经证明 TCA8418 驱动绑定失败：HOST_READY IRQ 计数为 `0`，I2C1 配置返回 `-ETIMEDOUT`，SPI 原始收发在 1 MHz 和 20 MHz 下均读回全 `0xff`。因此没有注册 `m5io-hub-i2c1`，TCA8418 没有 I2C 节点，系统也没有 CardputerZero 键盘输入设备；GPIO0 断电 3 秒、上电 10 秒后现象不变。该记录对应旧版本核心，当前版本的单次 I2C 超时只影响当前请求，后续请求会继续提交。

继续工作前优先核对：M5IO-HUB 电源与地、SE0 GPIO0/1/2/3、CS0、HOST_READY 对应 SoC GPIO28、SPI 电平和接线方向，以及 TCA8418 中断 GPIO86。硬件恢复后依次检查 `m5io-hub-i2c1`、I2C 地址 `0x34`、`tca8418_keypad` 和 `/dev/input/event*`，再用 `evtest` 验证普通键及 Sym/Fn/Shift 组合键。动态 I2C 总线号不要写死；若 SPI 仍为全 `0xff`，先处理供电、CS、HOST_READY 和接线，继续改键盘驱动没有意义。

## 2026-09-18 7.0 实机部署记录

在新设备 `3023797681`（内核 `7.0.0-g122c2c22d838`）上完成了部署准备和引导写入。写入前的完整备份保存在 `.artifacts/deploy-20260918-144908/`。`devcfg_a` 从 `660b63eddd8a3ac386471d907d0608119b5f4bc93934bcc75587d8e1a93a2a4d` 更新为候选镜像 `646fbb15e476fc33ee107c64ccf4eff9f12dc1d3d2c4aabcdd2e5ababdc94d87`；`devcfg_b` 未修改。实际启动项使用 `qrb2210-arduino-imola-jd9852.dtb`，已替换为 `.artifacts/m5io/tca8418-20260918-7.0/qrb2210-arduino-imola-se0-m5io-tca8418.dtb`，当前哈希为 `3e9c54ac74102ad789cccd8464522ca471167486ae1465062eef6b725ef76cf8`。

启动后确认 `spi0 -> 4a80000.spi`，SPI 速率为 20 MHz，`m5io-hub-core` 和 `m5io-hub-gpio` 自动加载并注册 17 个 GPIO。当前目标板的 M5IO-HUB 没有返回任何 SPI 数据：HOST_READY IRQ 计数为 0，I2C1 CONFIG 返回 `-ETIMEDOUT`，低速 1 MHz 和 20 MHz 的原始 `spidev` 读回均为全 `0xff`。因此系统没有注册 `m5io-hub-i2c1`，TCA8418 没有绑定，`/proc/bus/input/devices` 中没有 CardputerZero 键盘。GPIO0 断电 3 秒、上电 10 秒的整板电源循环后结果不变。

为本次目标，TCA8418 专用 overlay 仅启用 M5IO I2C1，I2C2 设为 `disabled`；通用 runtime overlay 仍保留两路 I2C 定义。驱动对声明但不可用的总线做容错，并在没有任何成功总线时返回 `-ENODEV`。当前失败证据指向外部 M5IO-HUB 未供电、接线/CS/引脚不通或从设备未运行，不能作为键盘驱动实机通过。确认 M5IO-HUB 能在 SPI 上返回数据后，重新加载 `m5io-hub-i2c` 或重启即可继续 TCA8418 和按键事件验收。

## 目标与实现

将现有 `modules/tca8418-1.0` 键盘驱动挂到 M5IO-HUB 的 I2C1（Linux 当前动态编号为 `i2c-3`，适配器名 `m5io-hub-i2c1`），TCA8418 地址为 `0x34`，中断使用 SoC `SOC_GPIO_86_SE0`。设备树中使用 Qualcomm TLMM GPIO86、上拉、输入、低电平触发：

```dts
interrupt-parent = <&tlmm>;
interrupts = <86 IRQ_TYPE_LEVEL_LOW>;
irq-gpios = <&tlmm 86 GPIO_ACTIVE_LOW>;
```

GPIO86 不是 M5IO GPIO 扩展器线路；`gpio0..3` 仍由 SE0 SPI 使用。I2C 设备节点放在 `&m5io_i2c1` 下，不硬编码 `i2c-3`，因此适配器重编号也不会改变绑定关系。SPI 保持 `spi-max-frequency = <20000000>`、CS setup `10 us`、CS inactive `100 us`。

三套 `linux,keymap` 从 `modules/CardputerZero/cardputerzero-v5-overlay.dts` 原样复用，每套 46 项，行列为 5×10，Sym/Fn/Shift 扫描码分别为 `0x43/0x42/0x41`。

原驱动无条件要求 `m5ioe1` 的三个 LED mode regmap phandle；当前 M5IO-HUB 设备没有该 I2C provider。`tca8418_parse_led_mode()` 现将这三个属性视为可选，属性存在时仍按原逻辑解析，缺少时保留空 regmap，`tca8418_led_set()` 安全跳过 LED mode 写入，键盘本体仍可探测。

当前目标设备也没有 TCA8418 LED 指示灯。设备树节点显式使用 `/delete-property/` 屏蔽 `tables-sel-led-gpio`、`capslock-led-gpio`、`fn-led-gpio` 及对应的三个 `*-led-mode` 属性，避免与旧 overlay 合并时继承 LED 配置；生成的节点只保留键盘扫描和中断功能。

## 生成物

相对仓库根 `/home/nihao/work/CardputerZero/m5stack-linux-dtoverlays.cpQ`：

| 生成物 | 本机位置 | 目标位置/用途 |
| --- | --- | --- |
| 驱动源码 | `modules/tca8418-1.0/tca8418_keypad_m5stack.c` | 交叉编译为 ARM64 模块 |
| 驱动模块 | `.artifacts/m5io/tca8418-20260917/tca8418_keypad_m5stack.ko` | 板上 `/lib/modules/6.16.7-g0dd6551ae96b/extra/m5io/tca8418_keypad_m5stack.ko` |
| Qualcomm 设备树源码 | `modules/CardputerZeroQ/tca8418-m5io-overlay.dts` | 与现有 M5IO runtime DT overlay 一起合入完整 DTB |
| 编译 overlay | `.artifacts/m5io/tca8418-20260917/tca8418-m5io-overlay.dtbo` | 离线合成中间文件，不直接写分区 |
| 完整 DTB | `.artifacts/m5io/tca8418-20260917/qrb2210-m5io-tca8418.dtb` | 板上 `/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb`，按文件替换 |
| udev 规则 | `modules/CardputerZeroQ/99-cardputerzeroq-keyboard.rules` | 板上 `/etc/udev/rules.d/99-cardputerzeroq-keyboard.rules` |
| 安装脚本 | `.artifacts/m5io/tca8418-20260917/install-on-device.sh` | root 下执行，先备份再安装，重启生效 |

本轮不写入 `devcfg_a`、boot、qupfw，也不修改或烧写 MCU 固件。安装脚本预期现有 DTB SHA-256 为 `93d97de06645cc8dd384f615864dfe88b916a8f4d9f80b99a6786d0adf91e66a`，防止覆盖未知基线；备份目录为板上 `/home/arduino/tca8418-backup-20260917/`。

## 构建与校验

```sh
make -C /home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16 \
  O=/home/nihao/文档/cpQ/work/se0-spi/out/kernel-6.16-build \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  LOCALVERSION=-g0dd6551ae96b M="$PWD/modules/tca8418-1.0" \
  W=1 KBUILD_MODPOST_WARN=1 modules

make -C modules/CardputerZeroQ \
  DTS_INCLUDE=/home/nihao/文档/cpQ/work/se0-spi/out/linux-6.16/include dtbs

fdtoverlay -i .artifacts/m5io/i2c-20260917/qrb2210-m5io-hub-i2c.dtb \
  -o .artifacts/m5io/tca8418-20260917/qrb2210-m5io-tca8418.dtb \
  modules/CardputerZeroQ/tca8418-m5io-overlay.dtbo
```

验证结果：ARM64 vermagic 为 `6.16.7-g0dd6551ae96b SMP preempt mod_unload aarch64`；模块 37 个导入符号在保存的目标内核导出表中全部存在；编译 DTB 中 `reg=0x34`、中断 86/低电平、GPIO86 pinctrl、5×10 参数、SPI 20 MHz 均通过；三套键位表各 46 项与 v5 参考完全一致。结果记录在 `.artifacts/m5io/tca8418-20260917/validation.log`，哈希在同目录 `SHA256SUMS`。

目标内核原始 `Module.symvers` 缺失，构建中仍有 modpost 未解析符号警告。已将全部导入符号与此前保存的目标内核导出表核对，但模块加载兼容性仍需实机验证。`irq-gpios` 沿用参考节点的描述方式，当前驱动实际通过 `client->irq` 使用 `interrupts` 属性。

## 实机状态与回退

2026-09-18 用户重新接线后复核：最初电源 GPIO0=OFF、BOOT GPIO2=OFF，已执行 `turn_on`。串口重新枚举后起初无访问权限；权限恢复后打开 `/dev/serial/by-id/usb-1a86_USB_Single_Serial_58FE005996-if00`（当前 `/dev/ttyACM0`，115200 8N1），断电 3 秒再上电并捕获 55 秒，仅收到 `00 00 00`，没有可解析的启动日志。主机未枚举 UNO Q，ADB 仍为空。日志为产物目录中的 `serial-power-20260918.log` 和 `connection-20260918.log`；产物哈希复核通过，但仍未安装到板上。电源保持 ON、BOOT OFF，待现场确认板端供电和 USB 数据连接。

2026-09-17 首次尝试：确认 ESPHome 电源 GPIO0 为 ON、BOOT GPIO2 为 OFF，并多次断电等待后重新上电。当时主机只枚举外部 QinHeng 调试串口 `/dev/ttyACM0`，未枚举 UNO Q 的 USB CDC/ADB（`adb devices -l` 为空）；串口捕获未收到数据。该轮未把模块/DTB 安装到设备，也没有重启后的驱动绑定、IRQ 计数或真实按键事件证据。不能将离线校验写成实机通过。

USB 恢复后，按以下顺序部署：

```sh
ADB=/tmp/se0-tools/platform-tools/adb
"$ADB" -s 3387062622 push .artifacts/m5io/tca8418-20260917 /home/arduino/
"$ADB" -s 3387062622 shell -t \
  'sudo sh /home/arduino/tca8418-20260917/install-on-device.sh'
"$ADB" -s 3387062622 shell -t 'sudo reboot'
```

重启后检查：

```sh
"$ADB" -s 3387062622 shell '
for adapter in /sys/class/i2c-adapter/i2c-*; do
    test "$(cat "$adapter/name")" = m5io-hub-i2c1 || continue
    bus=${adapter##*/i2c-}
    readlink -f "/sys/bus/i2c/devices/$bus-0034/driver"
done
grep -i tca8418 /proc/interrupts
cat /proc/bus/input/devices
ls -l /dev/input/cardputer-zero-internal
'
```

`sudo` 提示时输入设备账户密码。按键验收需在设备执行 `sudo evtest /dev/input/cardputer-zero-internal`，检查普通键及 Sym/Fn/Shift 组合键的按下、释放事件，以及 GPIO86 IRQ 计数变化。

回退时先恢复备份目录中的 `qrb2210-arduino-imola.dtb` 到 `/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb`，再恢复旧模块和旧规则；安装前不存在的模块/规则则删除本轮新增文件。执行 `sudo depmod -a 6.16.7-g0dd6551ae96b`、`sudo udevadm control --reload-rules`、`sync` 后重启。不要用更早的 SPI/DevCfg 备份回退本轮键盘改动。
