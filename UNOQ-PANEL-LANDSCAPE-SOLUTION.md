# 解决方案：JD9852 面板 170×320 竖屏 → 320×170 横屏

> 交接文档 2/2。项目背景见 `UNOQ-PROJECT-BACKGROUND.md`。
> 目标读者：负责实现与测试的工程师。**请按第 5 节验收标准逐条留证据**，不要把未验证项写成通过。

---

## 1. 问题描述

`modules/jd9852a/panel-elida-jd9852a.c` 当前声明的是**竖屏** `170×320`（高 320、宽 170）。
需求是改为**横屏** `320×170`（宽 320、高 170）。

**关键认知：显示方向由三层共同决定，只改 DRM mode 是无效的。**

| 层 | 位置 | 作用 |
| --- | --- | --- |
| 面板 MADCTL（`0x36`） | 驱动 init 序列 | 决定帧存到玻璃的扫描映射（MV 位做行列交换） |
| 地址窗口 CASET/PASET（`0x2a`/`0x2b`） | 驱动 init 序列 | 决定面板**物理激活矩形** |
| DRM mode | `default_mode` | 决定 DSI 主机发送的光栅（每行像素数、每帧行数、porch） |

---

## 2. 原理：为什么是"交换"而不是"改数字"

### 2.1 面板的物理结构（推断依据）

原竖屏配置是 `MADCTL = 0x00`（MV=0），此时 `0x2a`(CASET) 寻址 **source/列**轴、
`0x2b`(PASET) 寻址 **gate/行**轴：

- source 轴（列）：**35..204**，共 170 列
- gate 轴（行）：**0..319**，共 320 行

`35` 是 240 列 GRAM 里 170 列激活区的**居中偏移**：`(240-170)/2 = 35`，
且 `35 + 170 + 35 = 240` 完全对称。

⇒ 面板物理激活区 = **320(gate) × 170(source)**，横竖屏用的是**同一块区域，只是把两个轴对调**。
这正是 MADCTL 的 `MV` 位做的事。

### 2.2 三层各自怎么变

| | 竖屏（现状） | 横屏（目标） |
| --- | --- | --- |
| MADCTL `0x36` | `0x00` | `0xa0`（`MV\|MY`） |
| `0x2a` CASET | `00 23 00 cc` = 35..204 | `00 00 01 3f` = **0..319** |
| `0x2b` PASET | `00 00 01 3f` = 0..319 | `00 23 00 cc` = **35..204** |
| hdisplay | 170 | 320 |
| vdisplay | 320 | 170 |
| width_mm / height_mm | 22 / 42 | 42 / 22 |

**两个窗口就是原值互换**，这是最容易记也最容易做错的地方。

### 2.3 porch 必须"整组对调"，不能只改分辨率

porch 是**按物理轴**的常量，MV 交换轴之后 H/V 的 porch 也要跟着交换。

| | 竖屏 | 横屏 |
| --- | --- | --- |
| 行 | `170+187+28+28` = **413** | `320+8+2+6` = **336** |
| 帧 | `320+8+2+6` = **336** | `170+187+28+28` = **413** |
| `htotal × vtotal` | **138,768** | **138,768** |
| 刷新率 @ `clock=8334` | 60.06 Hz | **60.06 Hz** |

对调后总像素数完全不变 ⇒ `clock`、`dsi->hs_rate=200MHz`、`lanes=1`、RGB666
**全部不用改**，DSI 链路时序与竖屏完全一致，不需要重调 PHY。这是这个改法最大的好处。

轴容量校验（MV=1 后）：`htotal` 要覆盖 gate 轴（320+16=336 ✓），
`vtotal` 要覆盖 source 轴（413 ≥ 240 ✓）。

> ❌ **反例**：只把 hdisplay/vdisplay 换成 320/170 而保留原 porch（htotal 563 / vtotal 186），
> 刷新率会飙到 ~79.6 Hz，且轴容量关系错乱，大概率花屏或撕裂。

---

## 3. ⚠️ 当前代码里的一个已确认 bug

上一次改动**大部分是对的，但 PASET 写错了**，必须先修：

```c
/* 当前（错）：0x0046..0x00ef = 70..239 */
mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2b, 0x00, 0x46, 0x00, 0xef);

/* 应为：0x0023..0x00cc = 35..204 */
mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2b, 0x00, 0x23, 0x00, 0xcc);
```

**错因**：`70` 正好是 `240-170` 的余量，看起来是把"余量"直接当成了"偏移"，少除了一次 2。
后果是整个 170 行的窗口沿短轴平移 +35 行，画面会错位并出现空白带。

**推导**：MV=1 后 `0x2b` 寻址 source 轴，而物理激活窗口不会因为寻址轴对调而移动，
所以仍是 `35..204`。

---

## 4. 完整改动清单

文件：`modules/jd9852a/panel-elida-jd9852a.c`

### 4.1 init 序列（约 104–117 行）

```c
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x20); /* NINV */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x35); /* TE ON */
	/* Rotate the panel scan direction to landscape (MV | MY). */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x36, 0xa0);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3a, 0x66); /* RGB666 */

	/* Column address: X = 0..319 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2a,
					0x00, 0x00, 0x01, 0x3f);

	/* Page address: Y = 35..204 (the active 170-line window). */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2b,
					0x00, 0x23, 0x00, 0xcc);
```

### 4.2 `default_mode`（约 191–204 行）

```c
static const struct drm_display_mode default_mode = {
	.clock = 8334,
	.hdisplay = 320,
	.hsync_start = 320 + 8,
	.hsync_end = 320 + 8 + 2,
	.htotal = 320 + 8 + 2 + 6,
	.vdisplay = 170,
	.vsync_start = 170 + 187,
	.vsync_end = 170 + 187 + 28,
	.vtotal = 170 + 187 + 28 + 28,
	.width_mm = 42,
	.height_mm = 22,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};
```

### 4.3 日志字符串（约 296 行）

```c
	dev_info(dev,
		 "DSI config: 320x170 lanes=%u format=%u hs_rate=%lu flags=0x%lx\n",
		 dsi->lanes, dsi->format, dsi->hs_rate, dsi->mode_flags);
```

### 4.4 不需要改的

- `dsi->lanes = 1`、`dsi->format`、`dsi->hs_rate`、`dsi->mode_flags`
- `jd9852a.dts`（DT 里没有方向相关配置）
- `&mdss_dsi0_out { data-lanes = <0>; }` —— 单 lane 用 lane0 就是写 `<0>`，**不是 bug**

---

## 5. 构建

```sh
cd /home/nihao/work/CardputerZero/m5stack-linux-dtoverlays.cpQ/modules/jd9852a

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CC=$PWD/../../.artifacts/aarch64-gcc13-kbuild-wrapper \
  KERNELDIR=/home/nihao/w2T/arduino/linux
```

**为什么必须带这些变量**（不带会失败）：
- `ARCH=arm64`：本机 `uname -m` = `x86_64`，Makefile 的 `ARCH ?= $(shell uname -m)` 会选错。
- `CC=…kbuild-wrapper`：7.0 内核树用 gcc 15.2.0 编的，带 `-fmin-function-alignment=4`，
  本机 gcc 13.3.0 不认，会报 `unrecognized command-line option`。wrapper 只过滤这个选项。

**构建产物与校验**：

```sh
ls -l panel-elida-jd9852a.ko
strings panel-elida-jd9852a.ko | grep '^vermagic'
# 期望：vermagic=7.0.0-g122c2c22d838 SMP preempt mod_unload aarch64
```

构建时会出现 `compiler differs` / `pahole version differs` 的 warning，属正常。
若 dtbo 需要重建：`make jd9852a.dtbo`（会打印几条 `graph_port/graph_endpoint` warning，
来自未改动的 `jd9852a.dts`，与本改动无关，可忽略）。

---

## 6. 部署

面板 overlay 在设备树里引入 panel 节点，**原厂 DTB 里没有 panel**，必须叠加。

### 6.1 生成合入面板的完整 DTB（在主机上）

以设备**当前正在使用**的 DTB 为基底（它已经包含 SE0 SPI + M5IO + TCA8418 的改动）：

```sh
ADB=/tmp/se0-tools/platform-tools/adb
DEV=3023797681
cd /home/nihao/work/CardputerZero/m5stack-linux-dtoverlays.cpQ

# 1) 取回当前基底 DTB
$ADB -s $DEV pull /boot/efi/dtb/qcom/qrb2210-arduino-imola-jd9852.dtb /tmp/base.dtb

# 2) 叠加面板 overlay
fdtoverlay -i /tmp/base.dtb -o /tmp/jd9852-landscape.dtb modules/jd9852a/jd9852a.dtbo

# 3) 本地校验（必须全部符合）
fdtget -t s /tmp/jd9852-landscape.dtb \
  /soc@0/display-subsystem@5e00000/display-controller@5e01000 status   # 期望 okay
fdtget -t s /tmp/jd9852-landscape.dtb \
  /soc@0/display-subsystem@5e00000/dsi@5e94000 status                  # 期望 okay
dtc -I dtb -O dts /tmp/jd9852-landscape.dtb | grep -A6 'panel@0'       # 期望 compatible = "elida,jd9852"
```

> 已实测：对原厂 `active-jd9852.dtb` 套用 `jd9852a.dtbo` 可干净通过（exit 0），
> 产出 `panel@0` 挂在 `dsi@5e94000` 下、`mdp`/`dsi0` 均 okay、`data-lanes = <0>`、
> endpoint 双向连接正确。`&mdp`/`&mdss_dsi0`/`&mdss_dsi0_out` 这些 label 在基底 DTB 的
> `__symbols__` 里都有，能解析。
> **若基底 DTB 缺 `__symbols__` 导致 fdtoverlay 报 unresolved**，改用原厂
> `qrb2210-arduino-imola-jd9852.dtb` 作基底重新叠加。

### 6.2 安装到设备（设备上以 root 执行）

```sh
# 0) 备份（必须先做）
sudo cp /boot/efi/dtb/qcom/qrb2210-arduino-imola-jd9852.dtb \
        /home/arduino/jd9852-backup-$(date +%Y%m%d-%H%M%S).dtb
ls -l /lib/modules/$(uname -r)/extra/panel-elida-jd9852a.ko 2>/dev/null \
  && sudo cp /lib/modules/$(uname -r)/extra/panel-elida-jd9852a.ko /home/arduino/panel-backup.ko

# 1) 安装模块
sudo cp /home/arduino/panel-elida-jd9852a.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a $(uname -r)

# 2) 替换 DTB
sudo cp /home/arduino/jd9852-landscape.dtb \
        /boot/efi/dtb/qcom/qrb2210-arduino-imola-jd9852.dtb
sync

# 3) 重启
sudo reboot
```

推送文件：
```sh
$ADB -s $DEV push modules/jd9852a/panel-elida-jd9852a.ko /home/arduino/
$ADB -s $DEV push /tmp/jd9852-landscape.dtb /home/arduino/
```

---

## 7. 测试与验收标准

重启后逐条核对，**每条都要留证据**（命令输出 / 截图 / 照片）。

| # | 检查项 | 命令 | 通过标准 |
| --- | --- | --- | --- |
| 1 | 驱动已绑定 | `dmesg \| grep -iE 'jd9852\|panel'` | 出现 `DSI config: 320x170 …`，无 error |
| 2 | mode 已变 | `modetest -M msm -c`（或 `-M qcom`） | 首选 mode = **320x170**，刷新 ~60 Hz |
| 3 | 彩条方向 | `./drm-color-test /dev/dri/card0` | 上半部 **8 条竖色带**，下半部灰阶渐变 |
| 4 | 色序 | 同上 | 左→右：白、黄、青、绿、品红、红、蓝、黑 |
| 5 | 无偏移 | 同上 | 画面**居中铺满**，无空白带、无错位、无卷绕 |
| 6 | 无异常 | `dmesg \| grep -iE 'dsi\|mdss'` | 无 timeout / overflow / underrun / fifo error |
| 7 | 稳定性 | 连续运行 ≥ 10 分钟 | 无闪屏、无撕裂、无花屏 |

**判断方向错误的方法**：

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 画面**上下颠倒**（差 180°） | MADCTL 旋转方向反了 | `0x36` 从 `0xa0` 改 `0x60` |
| 画面**整体偏移 / 有空白带** | 地址窗口错 | 确认 `0x2b` = `0x0023..0x00cc`（35..204） |
| 画面**左右镜像** | 接线 / 像素序问题 | MADCTL 修不了，需查硬件 |
| **花屏 / 撕裂 / 锁不住** | porch 或面板内部时序不匹配 | 见第 9 节风险 |
| 驱动根本没 probe | overlay 没生效 | 回到第 6.1 步校验 DTB |

**参考**：`0xa0` 的选择与本仓库自身先例一致 ——
`modules/st7789v-1.0/st7789v_m5stack.c:330` 对同族控制器做 90° 旋转用的就是
`MADCTL_MV | MADCTL_MY`（= `0xa0`）。

---

## 8. 回退方案

```sh
# 设备上以 root 执行
sudo cp /home/arduino/jd9852-backup-<时间戳>.dtb \
        /boot/efi/dtb/qcom/qrb2210-arduino-imola-jd9852.dtb
# 若之前存在旧模块则恢复，否则删除本轮新增的模块
sudo cp /home/arduino/panel-backup.ko /lib/modules/$(uname -r)/extra/panel-elida-jd9852a.ko
sudo depmod -a $(uname -r)
sync && sudo reboot
```

> ⚠️ 不要用更早的 SPI/DevCfg 备份来回退本轮面板改动。

---

## 9. 风险与待确认项（诚实清单）

1. **面板内部 porch 寄存器**：驱动从未写过时序寄存器（init 里只有 `0xb2, 0x00, 0x37`
   一条，含义需查规格书）。如果面板 OTP/寄存器里锁了竖屏的行数，
   **光改 DRM mode 可能锁不住**。这是唯一需要实机验证、且可能推翻本方案的点。
2. **`0xa0` vs `0x60`**：两者都是合法纯旋转（差 180°），哪个对取决于面板在整机里的实际安装方向。
   必须看彩条确认，不能靠推断。
3. **`35` 偏移**：若旋转后画面整体偏了几列/几行，微调 `0x2b` 的 35/204。
   `35` 的推导依据是"240 列 GRAM 居中 170 列"，如果实际 GRAM 不是 240 列，
   这个值需要按实测修正（但**窗口本身仍是原竖屏 CASET 的那一对值**）。
4. **DSI burst 占空比**：横屏后每行 320 像素、hblank 只有 16 像素。
   按 `clock=8334`、18bpp、1 lane 估算，320 px 需 ~28.8 µs，行长 ~40.3 µs，
   占空比 ~71%，可行；竖屏时约 31%。已核算无越界，但**未实测**。
5. **Makefile 脆弱点**（非本次改动引入，但会影响复现）：
   - `ARCH ?= $(shell uname -m)` 在 x86_64 主机会选错，必须手传 `ARCH=arm64`；
   - 新版去掉了 `ifneq ($(KERNELRELEASE),)` 保护，且自定义了会递归调用 kbuild 的
     `$(Target_ko).ko:` 规则，目前能跑通但比旧版脆；
   - 旧版的 `CROSS_COMPILE` / `ARCH` 自动探测能力已丢失。
   **建议顺手修**，但不是本次任务的验收项。

---

## 10. 附：SE0 SPI 协议切换工作流（独立任务）

如果本次任务还包含"把 SE0 从 I2C 模式改成 SPI 模式"，请**先读
`UNOQ-PROJECT-BACKGROUND.md` 第 6 节**，那里写清了机理与两条路线。

要点速览：

- **路线 A（改 DevCfg，6.16 设备已验证成功）**：改 `0x9534`(TZ) / `0xbd48`(HYP) 协议字段
  `3→1`，重算 `0x1108` / `0x1138` 的 SHA-384，只写 `devcfg_a`。
  生成器 `se0-spi/make-devcfg-spi.py`。⚠️ 偏移仅适用于对应固件基线，换版本必须重新定位。
- **路线 B（运行时强制 load 微码，本次提示要求重试）**：用
  `se0-spi/force-se0-spi-firmware.patch` 加模块参数 `force_se0_spi_firmware=1`。
  ⚠️ 历史上有复位和 HYP data abort（`0x04a82000`）记录；
  ⚠️ 但**日志显示该路径很可能从未真正执行成功过**，所以值得重试。
  **重试必须全程抓 `/dev/ttyACM1` 串口，并先备好恢复路径。**

**验收（路线 A/B 通用）**：
1. `0x04a80068` 读回，`(v>>8)&0xff == 1`（协议 = SPI）；
2. `4a80000.spi` 绑定 `geni_spi`，master parent 是 SE0（不是 SE5 的 `4a94000.spi`）；
3. SoC GPIO0..3 归 SE0 SPI 所有；
4. 裸 `spidev` 收发或内部回环通过。

**注意**：即使第 1~4 条全过，**M5IO-HUB 仍可能不响应**（7.0 设备当前就是这种情况：
SE0 已是 SPI、驱动已加载，但 HUB 返回全 `0xff`、HOST_READY IRQ = 0）。
那是电气/供电/接线层的独立问题，见背景文档第 7.2 节。
