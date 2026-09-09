#!/usr/bin/env python3
"""BQ27220 完整参数清单 + 可直接执行的写入器。

地址全部取自 TRM SLUUBD4A **Table 3-2 的 RAM 段（0x91xx / 0x92xx）**。
TRM 的两处写入示例（§6.1 p65 写 Design Capacity、§4.6 p51 写 Hibernate I）
用的都是 RAM 段地址，因此本文件一律使用 RAM 段。

注意：bqcontrol.py 里原有的 0x40xx 地址属于 "Present OTP" 镜像段
（例如 Chg Inhibit Temp Low = 0x4041），本文件对应项改用 RAM 段 0x91F5。

用法（在 modules/bq27220-1.0/ 目录下执行）：

    python3 bq27220_profile.py                    # 打印 v5 的推荐值（默认，不写器件）
    python3 bq27220_profile.py --profile v3       # 打印 v3 的值
    python3 bq27220_profile.py --read             # 读回器件当前值
    python3 bq27220_profile.py --write            # 写入（unseal -> CFGUPDATE -> 写 -> 退出 -> seal）
    python3 bq27220_profile.py --write --seed-fcc # 新器件/首次配置：同时把 FCC 种成设计容量
    python3 bq27220_profile.py --write --edv fixed  # 保留驱动原方案（EDV_CMP=0 + 固定 EDV）

不要写 0x92A1（"Design Energy"）：该参数在 SLUUBD4A 中不存在，详见审计报告。
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bqcontrol as bc  # noqa: E402

# ---------------------------------------------------------------------------
# 1. 地址表：name -> (address, kind, unit, offset)
#    kind: u1/i1/u2/i2/h2（h2 与 u2 同为无符号大端 2 字节；H1 用 u1）
# ---------------------------------------------------------------------------
DATA_FIELDS: dict[str, tuple[int, str, str, int]] = {
    # ---- Gas Gauging / CEDV Configuration（Table 3-2, p29） ----
    "Battery Low %":                  (0x9251, "u2", "0.01%", 0),
    "Learning Low Temp":              (0x925B, "u1", "0.1C", 0),
    "Overload Current":               (0x9264, "i2", "mA", 0),
    "Self Discharge Rate":            (0x9268, "u1", "0.0025%/day", 0),
    "Electronics Load":               (0x9269, "i2", "3uA", 0),
    "Near Full":                      (0x926B, "i2", "mAh", 0),
    "Reserve Capacity":               (0x926D, "i2", "mAh", 0),
    "Chg Eff":                        (0x926F, "u1", "%", 0),
    "Dsg Eff":                        (0x9270, "u1", "%", 0),
    # ---- CEDV Smoothing Config ----
    "Smoothing Config":               (0x9271, "u1", "hex", 0),
    "Smoothing Start Voltage":        (0x9272, "i2", "mV", 0),
    "Smoothing Delta Voltage":        (0x9274, "i2", "mV", 0),
    "Max Smoothing Current":          (0x9276, "i2", "raw", 0),
    "EOC Smooth Current":             (0x927B, "u1", "0.1%", 0),
    "EOC Smooth Current Time":        (0x927C, "u1", "s", 0),
    # ---- Cycle / SOC flags / Battery ID ----
    "Cycle Count %":                  (0x927D, "u1", "%", 0),
    "Flag Config A":                  (0x927F, "h2", "hex", 0),
    "Flag Config B":                  (0x9281, "u1", "hex", 0),
    "Battery ID":                     (0x929A, "u1", "hex", 0),
    # ---- CEDV Profile 1 ----
    "Gauging Configuration":          (0x929B, "h2", "hex", 0),
    "Full Charge Capacity":           (0x929D, "i2", "mAh", 0),
    "Design Capacity":                (0x929F, "i2", "mAh", 0),
    "Design Voltage":                 (0x92A3, "i2", "mV", 0),
    "Charge Termination Voltage":     (0x92A5, "i2", "mV", 0),
    "EMF":                            (0x92A7, "u2", "raw", 0),
    "C0":                             (0x92A9, "u2", "raw", 0),
    "R0":                             (0x92AB, "u2", "raw", 0),
    "T0":                             (0x92AD, "u2", "raw", 0),
    "R1":                             (0x92AF, "u2", "raw", 0),
    "TC":                             (0x92B1, "u1", "raw", 0),
    "C1":                             (0x92B2, "u1", "raw", 0),
    "Age Factor":                     (0x92B3, "u1", "raw", 0),
    "Fixed EDV 0":                    (0x92B4, "i2", "mV", 0),
    "EDV 0 Hold Time":                (0x92B6, "u1", "s", 0),
    "Fixed EDV 1":                    (0x92B7, "i2", "mV", 0),
    "EDV 1 Hold Time":                (0x92B9, "u1", "s", 0),
    "Fixed EDV 2":                    (0x92BA, "i2", "mV", 0),
    "EDV 2 Hold Time":                (0x92BC, "u1", "s", 0),
    "Voltage 0% DOD":                 (0x92BD, "i2", "mV", 0),
    "Voltage 10% DOD":                (0x92BF, "i2", "mV", 0),
    "Voltage 20% DOD":                (0x92C1, "i2", "mV", 0),
    "Voltage 30% DOD":                (0x92C3, "i2", "mV", 0),
    "Voltage 40% DOD":                (0x92C5, "i2", "mV", 0),
    "Voltage 50% DOD":                (0x92C7, "i2", "mV", 0),
    "Voltage 60% DOD":                (0x92C9, "i2", "mV", 0),
    "Voltage 70% DOD":                (0x92CB, "i2", "mV", 0),
    "Voltage 80% DOD":                (0x92CD, "i2", "mV", 0),
    "Voltage 90% DOD":                (0x92CF, "i2", "mV", 0),
    "Voltage 100% DOD":               (0x92D1, "i2", "mV", 0),
    # ---- Configuration / Registers ----
    "Operation Config A":             (0x9206, "h2", "hex", 0),
    "Operation Config B":             (0x9208, "h2", "hex", 0),
    "SOC Delta":                      (0x920B, "u1", "%", 0),
    "Clk Ctl Reg":                    (0x920C, "u1", "hex", 0),
    "Sleep Current":                  (0x9217, "i2", "mA", 0),
    "Bus Low Time":                   (0x9219, "u1", "s", 0),
    "Offset Cal Inhibit Temp Low":    (0x921A, "i2", "0.1C", 0),
    "Offset Cal Inhibit Temp High":   (0x921C, "i2", "0.1C", 0),
    "Sleep Voltage Time":             (0x921E, "u1", "s", 0),
    "Sleep Current Time":             (0x921F, "u1", "s", 0),
    # ---- Current thresholds ----
    "Discharge Detection Threshold":  (0x9228, "i2", "mA", 0),
    "Charge Detection Threshold":     (0x922A, "i2", "mA", 0),
    "Quit Current":                   (0x922C, "i2", "mA", 0),
    "Discharge Relax Time":           (0x922E, "u2", "s", 0),
    "Charge Relax Time":              (0x9230, "u1", "s", 0),
    "Quit Relax Time":                (0x9231, "u1", "s", 0),
    "Initial Standby":                (0x923C, "i1", "mA", 0),
    # ---- System down ----
    "SysDown Set Volt Threshold":     (0x9240, "i2", "mV", 0),
    "SysDown Set Volt Time":          (0x9242, "u1", "s", 0),
    "SysDown Clear Volt Threshold":   (0x9243, "i2", "mV", 0),
    # ---- Charge inhibit / charge（RAM 段，非 0x40xx） ----
    "Chg Inhibit Temp Low":           (0x91F5, "i2", "0.1C", 0),
    "Chg Inhibit Temp High":          (0x91F7, "i2", "0.1C", 0),
    "Temp Hys":                       (0x91F9, "i2", "0.1C", 0),
    "Charging Current":               (0x91FB, "i2", "mA", 0),
    "Charging Voltage":               (0x91FD, "i2", "mV", 0),
    "Taper Current":                  (0x9201, "i2", "mA", 0),
    # ---- Safety ----
    "OT Chg":                         (0x9232, "i2", "0.1C", 0),
    "OT Chg Time":                    (0x9234, "u1", "s", 0),
    "OT Chg Recovery":                (0x9235, "i2", "0.1C", 0),
    "OT Dsg":                         (0x9237, "i2", "0.1C", 0),
    "OT Dsg Time":                    (0x9239, "u1", "s", 0),
    "OT Dsg Recovery":                (0x923A, "i2", "0.1C", 0),
    # ---- Calibration（写入值需实测，默认 0） ----
    "CC Offset":                      (0x9180, "i2", "counts", 0),
    "Board Offset":                   (0x91B4, "i1", "counts", 0),
    "Int Temp Offset":                (0x91B5, "i1", "0.1C", 0),
    "Ext Temp Offset":                (0x91B6, "i1", "0.1C", 0),
    "Pack V Offset":                  (0x91B7, "i1", "mV", 0),
    "Filter":                         (0x91DD, "u1", "raw", 0),
    "Deadband":                       (0x91DE, "u1", "mA", 0),
    "CC Deadband":                    (0x91DF, "u1", "294nV", 0),
}

# ---------------------------------------------------------------------------
# 2. GPCCEDV 报告输出（rev=61 那份 cedv_input-report）
#    EMF/C0/R0/T0/R1/TC/C1 + VOC25/VOC50/VOC75 + OCV11
#    这一组是"电压模型"，与电芯容量无关，v3/v5 共用。
# ---------------------------------------------------------------------------
CEDV: dict[str, int] = {
    "EMF": 3526,
    "C0": 36,
    "C1": 0,
    "R0": 1224,
    "R1": 2420,
    "T0": 2624,
    "TC": 11,
    "Age Factor": 0,
}

# OCV11.txt：SOC 100% -> 0%（即 DOD 0% -> 100%）
DOD_TABLE: dict[str, int] = {
    "Voltage 0% DOD": 4120,
    "Voltage 10% DOD": 3951,
    "Voltage 20% DOD": 3841,
    "Voltage 30% DOD": 3741,
    "Voltage 40% DOD": 3653,
    "Voltage 50% DOD": 3594,
    "Voltage 60% DOD": 3554,
    "Voltage 70% DOD": 3516,
    "Voltage 80% DOD": 3466,
    "Voltage 90% DOD": 3391,
    "Voltage 100% DOD": 3010,
}

# 公共部分：与容量无关的配置（取 TRM 默认值，除非另有说明）
COMMON: dict[str, int] = {
    "Battery Low %": 700,                 # 7.00%
    "Learning Low Temp": 119,             # 11.9 °C
    "Overload Current": 1500,             # mA
    "Self Discharge Rate": 20,            # 0.05%/day
    "Electronics Load": 0,
    "Reserve Capacity": 0,
    "Chg Eff": 100,
    "Dsg Eff": 100,
    "Smoothing Config": 0x08,             # SMEN=1，平滑到 EDV2
    "Smoothing Start Voltage": 3700,
    "Smoothing Delta Voltage": 100,
    "Max Smoothing Current": 8000,
    "EOC Smooth Current": 2,
    "EOC Smooth Current Time": 60,
    "Cycle Count %": 90,
    "Flag Config A": 0x0C8C,
    "Flag Config B": 0x8C,
    "Battery ID": 0x00,                   # 不烧 OTP 就保持 0
    "Design Voltage": 3700,
    "Charge Termination Voltage": 100,
    "EDV 0 Hold Time": 1,
    "EDV 1 Hold Time": 1,
    "EDV 2 Hold Time": 1,
    "Operation Config B": 0x1000,
    "SOC Delta": 1,
    "Clk Ctl Reg": 0x09,
    "Sleep Current": 10,                  # mA
    "Bus Low Time": 5,
    "Offset Cal Inhibit Temp Low": 50,
    "Offset Cal Inhibit Temp High": 450,
    "Sleep Voltage Time": 20,
    "Sleep Current Time": 20,
    "Discharge Detection Threshold": 60,
    "Charge Detection Threshold": 75,
    "Quit Current": 40,
    "Discharge Relax Time": 60,
    "Charge Relax Time": 60,
    "Quit Relax Time": 1,
    "Initial Standby": -10,
    "SysDown Set Volt Threshold": 3150,
    "SysDown Set Volt Time": 2,
    "SysDown Clear Volt Threshold": 3250,
    "Chg Inhibit Temp Low": 0,            # 0.0 °C
    "Chg Inhibit Temp High": 550,         # 55.0 °C（默认 45 °C 在整机发热时会误禁充）
    "Temp Hys": 50,
    "Charging Current": 200,
    "Charging Voltage": 4200,
    "Taper Current": 100,
    "OT Chg": 550,
    "OT Chg Time": 2,
    "OT Chg Recovery": 500,
    "OT Dsg": 600,
    "OT Dsg Time": 2,
    "OT Dsg Recovery": 550,
    "CC Offset": 0,
    "Board Offset": 0,
    "Int Temp Offset": 0,
    "Ext Temp Offset": 0,
    "Pack V Offset": 0,
    "Filter": 239,
    "Deadband": 5,
    "CC Deadband": 17,
}

# ---------------------------------------------------------------------------
# 3. 按硬件版本区分（来自 bq27220_v3.dts / bq27220_v5.dts）
# ---------------------------------------------------------------------------
PROFILES: dict[str, dict[str, int]] = {
    "v5": {
        "Design Capacity": 2000,          # mAh
        "Near Full": 200,                 # 0.1 × DC
        "Operation Config A": 0x8484,     # 0x0484 | TEMPS(bit15)，v5 用外部 NTC
    },
    "v3": {
        "Design Capacity": 1200,          # mAh
        "Near Full": 120,                 # 0.1 × DC
        "Operation Config A": 0x0484,     # 用内部温度传感器
    },
}

# EDV 门限的两种方案
EDV_MODEL = {  # 推荐：打开 CEDV 补偿，让器件用 EMF/C0/R0/T0/R1/TC/C1 自己算 EDV1/EDV2
    "Gauging Configuration": 0x102A,      # TRM 默认：EDV_CMP=1, FIXED_EDV0=1, CSYNC=1, SME0=1
    "Fixed EDV 0": 3000,                  # 等于 GPCCEDV 的 CellTermV
    "Fixed EDV 1": 3385,                  # EDV_CMP=1 时忽略
    "Fixed EDV 2": 3501,                  # EDV_CMP=1 时忽略
}
EDV_FIXED = {  # 保守：保留驱动的 EDV_CMP=0，但把固定门限改成真实低电量电压
    "Gauging Configuration": 0x0D31,      # 驱动原值：EDV_CMP=0（关闭补偿）
    "Fixed EDV 0": 3010,                  # OCV11 @ 0% SOC
    "Fixed EDV 1": 3120,                  # OCV11 @ 3% SOC
    "Fixed EDV 2": 3280,                  # OCV11 @ 7% SOC（= Battery Low %）
}


# 校准偏移量由器件的 MAC 命令实测（BOARD_OFFSET 0x0009 / CC_OFFSET 0x000A /
# CC_OFFSET_SAVE 0x000B），默认不覆盖，避免把已经标定好的值清成 0。
CALIB_OFFSETS = {
    "CC Offset", "Board Offset", "Int Temp Offset", "Ext Temp Offset", "Pack V Offset",
}


def build_values(profile: str, edv: str = "model", seed_fcc: bool = False,
                 with_calib: bool = False) -> dict[str, int]:
    """合成一份完整的写入清单（顺序即写入顺序）。"""
    if profile not in PROFILES:
        raise SystemExit(f"unknown profile {profile!r}, choose from {sorted(PROFILES)}")
    if edv not in ("model", "fixed"):
        raise SystemExit("--edv must be 'model' or 'fixed'")

    values: dict[str, int] = {}
    values.update(EDV_MODEL if edv == "model" else EDV_FIXED)
    values.update(CEDV)
    values.update(DOD_TABLE)
    values.update(COMMON)
    values.update(PROFILES[profile])
    if seed_fcc:
        values["Full Charge Capacity"] = values["Design Capacity"]
    if not with_calib:
        for name in CALIB_OFFSETS:
            values.pop(name, None)
    return values


def dump(values: dict[str, int]) -> None:
    print(f"{'name':<32} {'addr':>6} {'value':>8}  unit")
    print("-" * 62)
    for name, value in values.items():
        addr, kind, unit, _ = DATA_FIELDS[name]
        shown = f"0x{value:04X}" if (kind == "h2" or unit == "hex") else str(value)
        print(f"{name:<32} 0x{addr:04X} {shown:>8}  {unit}")


def program(dev: bc.BQ27220, values: dict[str, int], seal: bool = True,
            bat_insert: bool = True) -> None:
    """unseal -> CFGUPDATE -> 写全部 -> 退出 -> (BAT_INSERT) -> (SEALED)。"""
    dev.unseal_default()
    dev.enter_cfg_update()
    try:
        dev.control(bc.MAC_SET_PROFILE_1, wait=0.05)
        for name, value in values.items():
            dev.write_field(name, value)
            print(f"  wrote {name} = {value}")
    finally:
        dev.exit_cfg_update(reinit=True)
    if bat_insert:
        try:
            dev.control(bc.MAC_BAT_INSERT, wait=0.05)
        except Exception as exc:  # noqa: BLE001
            print(f"  warn: BAT_INSERT failed: {exc}", file=sys.stderr)
    if seal:
        dev.control(0x0030, wait=0.05)  # SEALED
    time.sleep(0.2)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", choices=sorted(PROFILES), default="v5")
    ap.add_argument("--edv", choices=["model", "fixed"], default="model",
                    help="model=CEDV 补偿开启(推荐)；fixed=保留驱动 EDV_CMP=0 并修正门限")
    ap.add_argument("--seed-fcc", action="store_true",
                    help="把 Learned FCC 写成设计容量（新器件/首次配置时用一次）")
    ap.add_argument("--with-calib", action="store_true",
                    help="同时写入校准偏移量（默认跳过，避免清掉已标定值）")
    ap.add_argument("--write", action="store_true", help="真正写入器件")
    ap.add_argument("--read", action="store_true", help="读回器件当前值")
    ap.add_argument("--no-seal", action="store_true", help="写完不执行 SEALED")
    ap.add_argument("--bus", type=bc.parse_int_auto, default=bc.DEFAULT_BUS)
    ap.add_argument("--addr", type=bc.parse_int_auto, default=bc.DEFAULT_ADDR)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args(argv)

    values = build_values(args.profile, args.edv, args.seed_fcc, args.with_calib)

    if not (args.write or args.read):
        print(f"# profile={args.profile} edv={args.edv} seed_fcc={args.seed_fcc}")
        dump(values)
        print("\n# 加 --write 才会写入器件；加 --read 可读回当前值")
        return 0

    bc.DATA_FIELDS = dict(DATA_FIELDS)   # 用本文件的 RAM 段地址表覆盖 bqcontrol
    bc.INIT_VALUES = dict(values)

    try:
        dev = bc.BQ27220(args.bus, args.addr, force=args.force)
    except Exception as exc:  # noqa: BLE001
        print(f"error: cannot open /dev/i2c-{args.bus} addr 0x{args.addr:02X}: {exc}",
              file=sys.stderr)
        return 2

    try:
        if args.read:
            bc.show_registers(dev)
            bc.show_config(dev)
        if args.write:
            program(dev, values, seal=not args.no_seal)
            print("\n[read back]")
            bc.show_config(dev)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:  # noqa: BLE001
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
