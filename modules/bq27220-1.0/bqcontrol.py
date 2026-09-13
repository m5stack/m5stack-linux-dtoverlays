#!/usr/bin/env python3
"""BQ27220 control/readout utility.

Requires Linux i2c-dev and either smbus2 or python-smbus installed.
Default bus/address can be provided by env vars:
  BQ27220_BUS / BQ_BUS / I2C_BUS
  BQ27220_ADDR / BQ_ADDR / I2C_ADDR

The init command requires an explicit profile, for example:
  python3 bqcontrol.py init --profile v5
"""

from __future__ import annotations

import argparse
import fcntl
import os
import sys
import time
from dataclasses import dataclass
from typing import Callable, Iterable, Optional

try:
    from smbus2 import SMBus, i2c_msg  # type: ignore
except ImportError:  # pragma: no cover - depends on target board
    try:
        from smbus import SMBus  # type: ignore
        i2c_msg = None  # type: ignore
    except ImportError:  # pragma: no cover
        SMBus = None  # type: ignore
        i2c_msg = None  # type: ignore


DEFAULT_ADDR = 0x55
DEFAULT_BUS = 1

# Standard commands, all little-endian word unless noted.
CMD_CONTROL = 0x00
CMD_TEMPERATURE = 0x06
CMD_VOLTAGE = 0x08
CMD_BATTERY_STATUS = 0x0A
CMD_CURRENT = 0x0C
CMD_REMAINING_CAPACITY = 0x10
CMD_FULL_CHARGE_CAPACITY = 0x12
CMD_AVERAGE_CURRENT = 0x14
CMD_TIME_TO_EMPTY = 0x16
CMD_TIME_TO_FULL = 0x18
CMD_STANDBY_CURRENT = 0x1A
CMD_RAW_COULOMB_COUNT = 0x22
CMD_AVERAGE_POWER = 0x24
CMD_INTERNAL_TEMPERATURE = 0x28
CMD_CYCLE_COUNT = 0x2A
CMD_STATE_OF_CHARGE = 0x2C
CMD_STATE_OF_HEALTH = 0x2E
CMD_CHARGING_VOLTAGE = 0x30
CMD_CHARGING_CURRENT = 0x32
CMD_OPERATION_STATUS = 0x3A
CMD_DESIGN_CAPACITY = 0x3C
CMD_MANUFACTURER_ACCESS = 0x3E
CMD_MAC_DATA = 0x40
CMD_MAC_DATA_SUM = 0x60
CMD_MAC_DATA_LEN = 0x61
MAC_DATA_BLOCK_LEN = 32
MAC_DATA_LEN_OVERHEAD = 4  # address + checksum/length
CMD_RAW_CURRENT = 0x7A
CMD_RAW_VOLTAGE = 0x7C
CMD_RAW_INT_TEMP = 0x7E

I2C_SLAVE = 0x0703
I2C_SLAVE_FORCE = 0x0706

# MAC subcommands.
MAC_CONTROL_STATUS = 0x0000
MAC_DEVICE_NUMBER = 0x0001
MAC_FW_VERSION = 0x0002
MAC_BAT_INSERT = 0x000D
MAC_ENTER_CFG_UPDATE = 0x0090
MAC_EXIT_CFG_UPDATE_REINIT = 0x0091
MAC_EXIT_CFG_UPDATE = 0x0092
MAC_SET_PROFILE_1 = 0x0015
MAC_FULL_ACCESS = 0xFFFF
TRM_UNSEAL_KEY = 0x8000
SEC_FULL = 0x1
SEC_UNSEALED = 0x2
SEC_SEALED = 0x3
SEC_SHIFT = 1
SEC_MASK = 0x3 << SEC_SHIFT

# Data memory fields used by this script. These are the active RAM data-memory
# addresses from TRM SLUUBD4A Table 3-2, not the Present OTP mirror addresses.
DATA_FIELDS = {
    "Chg Inhibit Temp Low": (0x91F5, "i2", "0.1C", 0),
    "Chg Inhibit Temp High": (0x91F7, "i2", "0.1C", 0),
    "Chg Inhibit Temp Hys": (0x91F9, "i2", "0.1C", 0),
    "Charging Current": (0x91FB, "i2", "mA", 0),
    "Charging Voltage": (0x91FD, "i2", "mV", 0),
    "Taper Current": (0x9201, "i2", "mA", 0),
    "Operation Config A": (0x9206, "h2", "hex", 0),
    "Operation Config B": (0x9208, "h2", "hex", 0),
    "Sleep Current": (0x9217, "i2", "mA", 0),
    # BQ27220 has no HIBERNATE mode; TRM 4.6 recommends clearing this field.
    "Hibernate I": (0x9221, "u1", "mA", 0),
    "Battery Low %": (0x9251, "u2", "0.01%", 0),
    "Near Full": (0x926B, "i2", "mAh", 0),
    "CEDV Profile 1 Gauging Configuration": (0x929B, "h2", "hex", 0),
    "Full Charge Capacity": (0x929D, "i2", "mAh", 0),
    "Design Capacity": (0x929F, "i2", "mAh", 0),
    "Design Voltage": (0x92A3, "i2", "mV", 0),
    "Charge Termination Voltage": (0x92A5, "i2", "mV offset", 0),
    "EMF": (0x92A7, "u2", "raw", 0),
    "C0": (0x92A9, "u2", "raw", 0),
    "R0": (0x92AB, "u2", "raw", 0),
    "T0": (0x92AD, "u2", "raw", 0),
    "R1": (0x92AF, "u2", "raw", 0),
    "TC": (0x92B1, "u1", "raw", 0),
    "C1": (0x92B2, "u1", "raw", 0),
    "Fixed EDV 0": (0x92B4, "i2", "mV", 0),
    "Fixed EDV 1": (0x92B7, "i2", "mV", 0),
    "Fixed EDV 2": (0x92BA, "i2", "mV", 0),
}

INIT_VALUES = {
    "Full Charge Capacity": 1200,
    "Design Capacity": 1200,
    "Design Voltage": 3700,  # mV; 4.2 V is the usual single-cell charge limit.
    "Chg Inhibit Temp Low": 0,      # 0.0C
    # This is the battery temperature threshold, configured here for 55 °C.
    "Chg Inhibit Temp High": 550,   # 55.0C
    "Chg Inhibit Temp Hys": 50,     # 5.0C
    "Hibernate I": 0,
    "Charging Current": 200,
    "Charging Voltage": 4200,
    "Taper Current": 100,
    # Both CardputerZero battery revisions connect BAT_NTC to BIN and do not
    # provide a board-side BIN pullup.
    "Operation Config A": 0x84A4,
    "Fixed EDV 0": 3300,
    "Fixed EDV 1": 3400,
    "Fixed EDV 2": 3500,
    "Battery Low %": 700,
    "Near Full": 120,
}


class BQError(RuntimeError):
    pass


def parse_int_auto(value: str) -> int:
    return int(str(value), 0)


def env_int(names: Iterable[str], default: int) -> int:
    for name in names:
        value = os.getenv(name)
        if value not in (None, ""):
            return parse_int_auto(value)
    return default


def le_u16(data: bytes) -> int:
    return data[0] | (data[1] << 8)


def le_i16(data: bytes) -> int:
    value = le_u16(data)
    return value - 0x10000 if value & 0x8000 else value


def encode_value(kind: str, value: int) -> bytes:
    value = int(value)
    if kind == "u1":
        if not 0 <= value <= 0xFF:
            raise ValueError(f"u1 value out of range: {value}")
        return value.to_bytes(1, "big")
    if kind == "i1":
        if not -0x80 <= value <= 0x7F:
            raise ValueError(f"i1 value out of range: {value}")
        return value.to_bytes(1, "big", signed=True)
    if kind in ("u2", "h2"):
        if not 0 <= value <= 0xFFFF:
            raise ValueError(f"u2 value out of range: {value}")
        return value.to_bytes(2, "big")
    if kind == "i2":
        if not -0x8000 <= value <= 0x7FFF:
            raise ValueError(f"i2 value out of range: {value}")
        return value.to_bytes(2, "big", signed=True)
    raise ValueError(f"unsupported type: {kind}")


def decode_value(kind: str, data: bytes) -> int:
    if kind in ("u1",):
        return data[0]
    if kind == "i1":
        return int.from_bytes(data[:1], "big", signed=True)
    if kind in ("u2", "h2"):
        return int.from_bytes(data[:2], "big", signed=False)
    if kind == "i2":
        return int.from_bytes(data[:2], "big", signed=True)
    raise ValueError(f"unsupported type: {kind}")


def fmt_hex(value: int, width: int = 4) -> str:
    return f"0x{value:0{width}X}"


@dataclass
class Register:
    name: str
    cmd: int
    unit: str
    signed: bool = False
    transform: Optional[Callable[[int], object]] = None


REGISTERS = [
    Register("Temperature", CMD_TEMPERATURE, "degC", False, lambda x: round(x / 10.0 - 273.15, 2)),
    Register("Voltage", CMD_VOLTAGE, "mV"),
    Register("BatteryStatus", CMD_BATTERY_STATUS, "hex", False, lambda x: fmt_hex(x)),
    Register("Current", CMD_CURRENT, "mA", True),
    Register("RemainingCapacity", CMD_REMAINING_CAPACITY, "mAh"),
    Register("FullChargeCapacity", CMD_FULL_CHARGE_CAPACITY, "mAh"),
    Register("AverageCurrent", CMD_AVERAGE_CURRENT, "mA", True),
    Register("TimeToEmpty", CMD_TIME_TO_EMPTY, "min"),
    Register("TimeToFull", CMD_TIME_TO_FULL, "min"),
    Register("StandbyCurrent", CMD_STANDBY_CURRENT, "mA", True),
    Register("RawCoulombCount", CMD_RAW_COULOMB_COUNT, "raw"),
    Register("AveragePower", CMD_AVERAGE_POWER, "mW", True),
    Register("InternalTemperature", CMD_INTERNAL_TEMPERATURE, "degC", False, lambda x: round(x / 10.0 - 273.15, 2)),
    Register("CycleCount", CMD_CYCLE_COUNT, "cycles"),
    Register("StateOfCharge", CMD_STATE_OF_CHARGE, "%"),
    Register("StateOfHealth", CMD_STATE_OF_HEALTH, "%"),
    Register("ChargingVoltage", CMD_CHARGING_VOLTAGE, "mV"),
    Register("ChargingCurrent", CMD_CHARGING_CURRENT, "mA"),
    Register("OperationStatus", CMD_OPERATION_STATUS, "hex", False, lambda x: fmt_hex(x)),
    Register("DesignCapacity", CMD_DESIGN_CAPACITY, "mAh"),
]


class BQ27220:
    def __init__(self, bus_num: int, address: int, debug: bool = False, force: bool = False):
        if SMBus is None:
            raise BQError("missing SMBus module; install python3-smbus or smbus2")
        self.bus_num = bus_num
        self.address = address
        self.debug = debug
        self.force = force
        self.bus = SMBus(bus_num)

    def close(self) -> None:
        close = getattr(self.bus, "close", None)
        if close:
            close()

    def _dbg(self, msg: str) -> None:
        if self.debug:
            print(f"[debug] {msg}", file=sys.stderr)

    def _select_address(self) -> None:
        fd = getattr(self.bus, "fd", None)
        if fd is not None:
            fcntl.ioctl(fd, I2C_SLAVE_FORCE if self.force else I2C_SLAVE, self.address)

    def read_bytes(self, reg: int, length: int) -> bytes:
        self._select_address()
        fd = getattr(self.bus, "fd", None)
        if self.force and fd is not None:
            written = os.write(fd, bytes([reg]))
            if written != 1:
                raise BQError(f"short I2C register write: {written}/1 bytes")
            time.sleep(0.001)
            data = os.read(fd, length)
            if len(data) != length:
                raise BQError(f"short I2C read: {len(data)}/{length} bytes")
            return data
        if i2c_msg is not None:
            write = i2c_msg.write(self.address, [reg])
            read = i2c_msg.read(self.address, length)
            self.bus.i2c_rdwr(write, read)
            data = bytes(read)
            if len(data) != length:
                raise BQError(f"short I2C read: {len(data)}/{length} bytes")
            return data
        data = bytes(self.bus.read_i2c_block_data(self.address, reg, length))
        if len(data) != length:
            raise BQError(f"short I2C block read: {len(data)}/{length} bytes")
        return data

    def write_bytes(self, reg: int, data: bytes | list[int]) -> None:
        self._select_address()
        payload = list(data)
        self._dbg(f"write reg=0x{reg:02X} data={' '.join(f'{x:02X}' for x in payload)}")
        fd = getattr(self.bus, "fd", None)
        if self.force and fd is not None:
            expected = 1 + len(payload)
            written = os.write(fd, bytes([reg] + payload))
            if written != expected:
                raise BQError(f"short I2C write: {written}/{expected} bytes")
        elif i2c_msg is not None:
            # BQ27220 expects an incremental I2C write (register followed by
            # payload).  SMBus block writes add a count byte, which would be
            # interpreted as data by the gauge.
            msg = i2c_msg.write(self.address, [reg] + payload)
            self.bus.i2c_rdwr(msg)
        elif len(payload) == 2 and hasattr(self.bus, "write_word_data"):
            # MACDataSum() and MACDataLen() must be committed as one word.
            # SMBus word writes transmit the low byte first, so preserve the
            # caller's byte order explicitly.
            value = payload[0] | (payload[1] << 8)
            self.bus.write_word_data(self.address, reg, value)
        elif len(payload) == 2:
            # Splitting this write would make MACDataSum()/MACDataLen() arrive
            # as separate transactions, which the gauge does not execute as a
            # data-memory commit.  Fail clearly instead of silently corrupting
            # the update protocol.
            raise BQError(
                "two-byte writes require smbus2.i2c_msg or SMBus.write_word_data"
            )
        elif len(payload) == 1:
            self.bus.write_byte_data(self.address, reg, payload[0])
        else:
            # python-smbus versions without i2c_msg have no raw incremental
            # write API.  Individual byte writes avoid the SMBus block-count
            # byte.  Keep a small bus-free interval for fast-mode adapters.
            for offset, value in enumerate(payload):
                self.bus.write_byte_data(self.address, reg + offset, value)
                if offset + 1 < len(payload):
                    time.sleep(0.0001)
        time.sleep(0.002)

    def read_word(self, reg: int, signed: bool = False) -> int:
        data = self.read_bytes(reg, 2)
        return le_i16(data) if signed else le_u16(data)

    def write_word(self, reg: int, value: int) -> None:
        self.write_bytes(reg, bytes([value & 0xFF, (value >> 8) & 0xFF]))

    def control(self, subcmd: int, wait: float = 0.05) -> None:
        self.write_word(CMD_CONTROL, subcmd)
        time.sleep(wait)

    def security_state(self) -> int:
        """Return SEC as 1=FULL, 2=UNSEALED, 3=SEALED."""
        status = self.read_word(CMD_OPERATION_STATUS)
        return (status & SEC_MASK) >> SEC_SHIFT

    def mac_command(self, subcmd: int, wait: float = 0.05) -> bytes:
        self.write_word(CMD_MANUFACTURER_ACCESS, subcmd)
        time.sleep(wait)
        # MACData() starts at 0x40. Reading from 0x3E also includes the
        # two-byte subcommand echo and truncates the actual response.
        return self.read_bytes(CMD_MAC_DATA, MAC_DATA_BLOCK_LEN)

    def unseal_default(self) -> None:
        status = self.read_word(CMD_OPERATION_STATUS)
        sec = (status & SEC_MASK) >> SEC_SHIFT
        if sec in (SEC_FULL, SEC_UNSEALED):
            return
        if sec != SEC_SEALED:
            raise BQError(f"unsupported SEC state before unseal (OperationStatus=0x{status:04X})")

        # Older examples use 0x0414/0x3672, while SLUUBD4A documents
        # 0x8000/0x8000 as the ROM default. Try the former first for parts
        # already configured from those examples, then the documented default.
        for key1, key2 in ((0x0414, 0x3672),
                           (TRM_UNSEAL_KEY, TRM_UNSEAL_KEY)):
            self.control(key1, wait=0.05)
            self.control(key2, wait=0.05)
            status = self.read_word(CMD_OPERATION_STATUS)
            if ((status & SEC_MASK) >> SEC_SHIFT) in (SEC_FULL, SEC_UNSEALED):
                return
        raise BQError(f"unseal key rejected (OperationStatus=0x{status:04X})")

    def seal(self) -> None:
        """Enter SEALED mode and verify the transition."""
        if self.security_state() == SEC_SEALED:
            return
        self.control(0x0030, wait=0.05)
        sec = self.security_state()
        if sec != SEC_SEALED:
            raise BQError(f"failed to enter SEALED mode (SEC={sec})")

    def full_access(self) -> None:
        # Full access is required before BQ27220 data-memory writes.
        status = self.read_word(CMD_OPERATION_STATUS)
        sec = (status & SEC_MASK) >> SEC_SHIFT
        if sec == SEC_FULL:
            return
        if sec != SEC_UNSEALED:
            raise BQError(f"cannot enter full access from SEC={sec} (OperationStatus=0x{status:04X})")
        self.control(MAC_FULL_ACCESS, wait=0.05)
        self.control(MAC_FULL_ACCESS, wait=0.05)
        status = self.read_word(CMD_OPERATION_STATUS)
        if ((status & SEC_MASK) >> SEC_SHIFT) != SEC_FULL:
            raise BQError(f"full access rejected (OperationStatus=0x{status:04X})")

    def is_cfg_update_active(self) -> bool:
        """Return the current CFGUPDATE state from OperationStatus()."""
        status = self.read_word(CMD_OPERATION_STATUS)
        return bool(status & (1 << 10))

    def _select_data_memory(self, address: int) -> int:
        if not 0 <= address <= 0xFFFF:
            raise ValueError(f"data-memory address out of range: 0x{address:X}")
        self.write_bytes(CMD_MANUFACTURER_ACCESS, bytes([address & 0xFF, (address >> 8) & 0xFF]))
        time.sleep(0.01)

        data_len = self.read_bytes(CMD_MAC_DATA_LEN, 1)
        if len(data_len) != 1:
            raise BQError("short read from MACDataLen")
        block_len = data_len[0] - MAC_DATA_LEN_OVERHEAD
        if not 0 <= block_len <= MAC_DATA_BLOCK_LEN:
            raise BQError(f"invalid MACDataLen 0x{data_len[0]:02X}")
        return block_len

    def read_data_memory(self, address: int, length: int) -> bytes:
        if not 1 <= length <= MAC_DATA_BLOCK_LEN:
            raise ValueError(f"data-memory read length must be 1..32: {length}")
        block_len = self._select_data_memory(address)
        if length > block_len:
            raise BQError(
                f"data-memory read crosses block boundary: length={length}, block={block_len}"
            )

        # read_bytes() guarantees an exact-length result.  Reading from 0x40
        # is the documented MACData() interface; a short-read fallback here
        # was unreachable and could issue a second, unrelated MAC read.
        return self.read_bytes(CMD_MAC_DATA, length)

    def write_data_memory(self, address: int, payload: bytes, offset: int = 0) -> None:
        if not 0 <= address <= 0xFFFF:
            raise ValueError(f"data-memory address out of range: 0x{address:X}")
        if not 1 <= len(payload) <= MAC_DATA_BLOCK_LEN:
            raise BQError("data-memory payload must be 1..32 bytes")
        if offset < 0:
            raise ValueError(f"data-memory field offset must be non-negative: {offset}")

        # BQ27220 commits the complete selected MACData block.  The selected
        # address is presented at 0x3E/0x3F and maps the first byte of the
        # block to that address; preserve all neighbouring parameters with a
        # read-modify-write operation.  MACDataLen varies by subclass (for
        # example 0x05 for a one-byte field and 0x24 for a 32-byte block).
        block_len = self._select_data_memory(address)
        if offset + len(payload) > block_len:
            raise BQError(
                f"data-memory write crosses block boundary: offset={offset}, "
                f"payload={len(payload)}, block={block_len}"
            )
        block = bytearray(self.read_bytes(CMD_MAC_DATA, block_len))
        if len(block) != block_len:
            raise BQError("short read from MACData")
        block[offset:offset + len(payload)] = payload
        # MACDataSum is the complement of the selected address bytes
        # (ManufacturerAccessControl) plus the complete MACData block.
        checksum = (~((address & 0xFF) + (address >> 8) + sum(block))) & 0xFF

        self.write_bytes(CMD_MAC_DATA, block)
        # MACDataLen is the selected block length, not the number of bytes
        # changed in this transaction.
        self.write_bytes(CMD_MAC_DATA_SUM,
                         bytes([checksum, block_len + MAC_DATA_LEN_OVERHEAD]))
        time.sleep(0.02)

    def read_field(self, name: str,
                   fields: Optional[dict[str, tuple[int, str, str, int]]] = None) -> int:
        fields = DATA_FIELDS if fields is None else fields
        address, kind, _unit, offset = fields[name]
        size = 1 if kind.endswith("1") else 2
        return decode_value(kind, self.read_data_memory(address, offset + size)[offset:offset + size])

    def write_field(self, name: str, value: int,
                    fields: Optional[dict[str, tuple[int, str, str, int]]] = None) -> None:
        fields = DATA_FIELDS if fields is None else fields
        address, kind, _unit, offset = fields[name]
        self.write_data_memory(address, encode_value(kind, value), offset)

    def enter_cfg_update(self) -> None:
        # The TRM requires at least 2 s before OperationStatus()[CFGUPDATE]
        # may be read after ENTER_CFG_UPDATE.
        self.control(MAC_ENTER_CFG_UPDATE, wait=2.0)
        self._wait_cfg_update(True)

    def exit_cfg_update(self, reinit: bool = True) -> None:
        self.control(MAC_EXIT_CFG_UPDATE_REINIT if reinit else MAC_EXIT_CFG_UPDATE,
                     wait=0.05)
        self._wait_cfg_update(False)

    def _wait_cfg_update(self, active: bool, timeout: float = 2.0) -> None:
        deadline = time.monotonic() + timeout
        status = 0
        while True:
            status = self.read_word(CMD_OPERATION_STATUS)
            if bool(status & (1 << 10)) == active:
                return
            if time.monotonic() >= deadline:
                state = "enter" if active else "exit"
                raise BQError(
                    f"CFGUPDATE {state} timeout (OperationStatus=0x{status:04X})"
                )
            time.sleep(0.1)


def battery_status_flags(value: int) -> list[str]:
    flags = []
    mapping = [
        (0, "DSG"), (1, "SYSDWN"), (2, "TDA"), (3, "BATTPRES"),
        (4, "AUTH_GD"), (5, "OCVGD"), (6, "TCA"), (8, "CHGINH"),
        (9, "FC"), (10, "OTD"), (11, "OTC"), (12, "SLEEP"),
        (13, "OCVFAIL"), (14, "OCVCOMP"), (15, "FD"),
    ]
    for bit, name in mapping:
        if value & (1 << bit):
            flags.append(name)
    return flags


def operation_status_flags(value: int) -> list[str]:
    flags = []
    mapping = [
        (0, "CALMD"), (1, "SEC0"), (2, "SEC1"), (3, "EDV2"),
        (4, "VDQ"), (5, "INITCOMP"), (6, "SMTH"), (7, "BTPINT"),
        (10, "CFGUPDATE"),
    ]
    for bit, name in mapping:
        if value & (1 << bit):
            flags.append(name)
    return flags


def print_kv(name: str, value: object, unit: str = "") -> None:
    suffix = f" {unit}" if unit and unit not in ("hex", "raw") else ""
    print(f"{name:34s}: {value}{suffix}")


def show_registers(dev: BQ27220) -> None:
    print(f"BQ27220 on /dev/i2c-{dev.bus_num}, address 0x{dev.address:02X}")
    print("\n[standard registers]")
    for reg in REGISTERS:
        try:
            raw = dev.read_word(reg.cmd, signed=reg.signed)
            value = reg.transform(raw) if reg.transform else raw
            print_kv(reg.name, value, reg.unit)
            if reg.name == "BatteryStatus":
                print_kv("  BatteryStatus flags", ",".join(battery_status_flags(raw)) or "none")
            elif reg.name == "OperationStatus":
                print_kv("  OperationStatus flags", ",".join(operation_status_flags(raw)) or "none")
        except Exception as exc:
            print_kv(reg.name, f"read failed: {exc}")

    print("\n[device info]")
    for name, subcmd in (("Device Number", MAC_DEVICE_NUMBER), ("FW Version raw", MAC_FW_VERSION)):
        try:
            data = dev.mac_command(subcmd, wait=0.05)
            shown = " ".join(f"{b:02X}" for b in data[:16])
            if name == "Device Number" and len(data) >= 2:
                shown = f"0x{le_u16(data[:2]):04X} ({shown})"
            print_kv(name, shown)
        except Exception as exc:
            print_kv(name, f"read failed: {exc}")


def show_config(dev: BQ27220,
                fields: Optional[dict[str, tuple[int, str, str, int]]] = None) -> None:
    print("\n[data memory / battery config]")
    fields = DATA_FIELDS if fields is None else fields
    for name, (_address, kind, unit, _offset) in fields.items():
        try:
            value = dev.read_field(name, fields)
            if kind == "h2":
                value = fmt_hex(value)
            print_kv(name, value, unit)
        except Exception as exc:
            print_kv(name, f"read failed: {exc}")


def show_config_with_access(
    dev: BQ27220,
    fields: Optional[dict[str, tuple[int, str, str, int]]] = None,
) -> None:
    """Read Data Memory while preserving the original security state.

    Most Data Memory blocks are inaccessible while SEALED.  The standard
    commands remain readable, so callers can still inspect those separately.
    """
    original = dev.security_state()
    if original not in (SEC_FULL, SEC_UNSEALED, SEC_SEALED):
        raise BQError(f"unsupported SEC state while reading config: {original}")

    try:
        if original == SEC_SEALED:
            dev.unseal_default()
        if original in (SEC_SEALED, SEC_UNSEALED):
            # Data Memory reads require FULL ACCESS, even when the gauge was
            # already UNSEALED before this command.
            dev.full_access()
        show_config(dev, fields)
    finally:
        if original == SEC_SEALED:
            dev.seal()
        elif original == SEC_UNSEALED:
            # FULL ACCESS has no direct downgrade command. Seal and unseal
            # again so a caller that started UNSEALED is left UNSEALED.
            dev.seal()
            dev.unseal_default()


def init_device(dev: BQ27220, assume_unsealed: bool = False, reinit: bool = True,
                seal: bool = True,
                values: Optional[dict[str, int]] = None,
                fields: Optional[dict[str, tuple[int, str, str, int]]] = None) -> None:
    values = INIT_VALUES if values is None else values
    fields = DATA_FIELDS if fields is None else fields
    print("Entering CONFIG UPDATE and writing temporary battery settings...")
    try:
        if not assume_unsealed:
            dev.unseal_default()
        dev.full_access()
        cfgupdate_active = False
        try:
            dev.enter_cfg_update()
            cfgupdate_active = True
            for name, value in values.items():
                dev.write_field(name, value, fields)
                print_kv(f"wrote {name}", value, fields[name][2])
        finally:
            if not cfgupdate_active:
                try:
                    cfgupdate_active = dev.is_cfg_update_active()
                except Exception as exc:  # noqa: BLE001
                    if sys.exc_info()[0] is not None:
                        print(f"warn: cannot determine CFGUPDATE state: {exc}",
                              file=sys.stderr)
                    else:
                        raise
            if cfgupdate_active:
                had_error = sys.exc_info()[0] is not None
                try:
                    dev.exit_cfg_update(reinit=reinit)
                except Exception as exc:  # noqa: BLE001
                    if had_error:
                        print(f"warn: exit CFGUPDATE failed: {exc}", file=sys.stderr)
                    else:
                        raise

        # BAT_INSERT is only required when Operation Config A[BIEnable] is
        # clear.  The profiles used by this project set BIEnable=1, so the
        # gauge detects the battery from BIN and no command is needed.
        op_cfg_a = values.get("Operation Config A")
        if op_cfg_a is not None and not (op_cfg_a & (1 << 7)):
            try:
                dev.control(MAC_BAT_INSERT, wait=0.05)
            except Exception as exc:  # noqa: BLE001
                print(f"warn: BAT_INSERT failed: {exc}", file=sys.stderr)
                raise
    finally:
        if seal:
            had_error = sys.exc_info()[0] is not None
            try:
                dev.seal()
            except Exception as exc:  # noqa: BLE001
                if had_error:
                    print(f"warn: SEALED failed: {exc}", file=sys.stderr)
                else:
                    raise
    print("Done. Note: setting is temporary unless OTP/programming flow is used.")



def poll(dev: BQ27220, interval: float) -> None:
    names = [
        ("Voltage", CMD_VOLTAGE, False, "mV"),
        ("Current", CMD_CURRENT, True, "mA"),
        ("AverageCurrent", CMD_AVERAGE_CURRENT, True, "mA"),
        ("RemainingCapacity", CMD_REMAINING_CAPACITY, False, "mAh"),
        ("FullChargeCapacity", CMD_FULL_CHARGE_CAPACITY, False, "mAh"),
        ("StateOfCharge", CMD_STATE_OF_CHARGE, False, "%"),
        ("StateOfHealth", CMD_STATE_OF_HEALTH, False, "%"),
        ("Temperature", CMD_TEMPERATURE, False, "degC"),
        ("BatteryStatus", CMD_BATTERY_STATUS, False, "hex"),
    ]
    print("time                 voltage current avg_current rem_cap full_cap soc soh temp status")
    while True:
        values: dict[str, object] = {}
        for name, cmd, signed, unit in names:
            try:
                raw = dev.read_word(cmd, signed=signed)
                if name == "Temperature":
                    values[name] = f"{raw / 10.0 - 273.15:.1f}C"
                elif unit == "hex":
                    values[name] = fmt_hex(raw)
                else:
                    values[name] = f"{raw}{unit}"
            except Exception as exc:
                values[name] = f"ERR:{exc}"
        print(
            f"{time.strftime('%Y-%m-%d %H:%M:%S')} "
            f"{values['Voltage']:>8} {values['Current']:>8} {values['AverageCurrent']:>11} "
            f"{values['RemainingCapacity']:>8} {values['FullChargeCapacity']:>8} "
            f"{values['StateOfCharge']:>5} {values['StateOfHealth']:>5} "
            f"{values['Temperature']:>6} {values['BatteryStatus']:>6}",
            flush=True,
        )
        time.sleep(interval)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Read/configure/monitor TI BQ27220 over Linux i2c-dev")
    parser.add_argument("--bus", type=parse_int_auto, default=env_int(("BQ27220_BUS", "BQ_BUS", "I2C_BUS"), DEFAULT_BUS),
                        help="I2C bus number, env BQ27220_BUS/BQ_BUS/I2C_BUS, default 1")
    parser.add_argument("--addr", type=parse_int_auto, default=env_int(("BQ27220_ADDR", "BQ_ADDR", "I2C_ADDR"), DEFAULT_ADDR),
                        help="7-bit I2C address, env BQ27220_ADDR/BQ_ADDR/I2C_ADDR, default 0x55")
    parser.add_argument("--debug", action="store_true", help="print I2C write frames to stderr")
    parser.add_argument("--force", action="store_true", help="force I2C access even if a kernel driver owns the address")
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("show", help="read registers and current data-memory battery config")

    init_p = sub.add_parser(
        "init",
        help="write a complete v3/v5 battery profile to BQ27220 RAM data memory",
    )
    init_p.add_argument("--profile", choices=("v3", "v5"), required=True,
                        help="hardware battery profile (required; prevents an implicit 1200 mAh write)")
    init_p.add_argument("--edv", choices=("model", "fixed"), default="model",
                        help="model=CEDV compensation; fixed=static EDV thresholds")
    init_p.add_argument("--seed-fcc", action="store_true",
                        help="initialize Learned FCC to the profile capacity")
    init_p.add_argument("--with-calib", action="store_true",
                        help="also write calibration offsets (normally skipped)")
    init_p.add_argument("--assume-unsealed", action="store_true", help="skip default unseal sequence")
    init_p.add_argument("--no-reinit", action="store_true", help="exit CONFIG UPDATE without reinitialize")
    init_p.add_argument("--no-seal", action="store_true", help="leave the gauge unsealed (debug only)")

    poll_p = sub.add_parser("poll", help="print live battery data repeatedly")
    poll_p.add_argument("--interval", "-i", type=float, default=1.0, help="poll interval seconds, default 1")

    sub.add_parser("read", help="same as show")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    command = args.command or "show"
    try:
        dev = BQ27220(args.bus, args.addr, debug=args.debug, force=args.force)
    except Exception as exc:
        print(f"error: cannot open BQ27220 on /dev/i2c-{args.bus} addr 0x{args.addr:02X}: {exc}", file=sys.stderr)
        return 2

    try:
        if command in ("show", "read"):
            show_registers(dev)
            show_config_with_access(dev)
        elif command == "init":
            # Keep the standalone utility and the full profile writer on the
            # same address/value table. The import is lazy so read/poll do not
            # pull in the profile module or create a second SMBus dependency.
            try:
                import bq27220_profile as profile
                init_values = profile.build_values(
                    args.profile, args.edv, args.seed_fcc, args.with_calib)
            except (ImportError, ValueError, SystemExit) as exc:
                raise BQError(f"cannot load battery profile: {exc}") from exc
            init_device(dev, assume_unsealed=args.assume_unsealed,
                        reinit=not args.no_reinit, seal=not args.no_seal,
                        values=init_values, fields=profile.DATA_FIELDS)
            show_registers(dev)
            show_config_with_access(dev, profile.DATA_FIELDS)
        elif command == "poll":
            poll(dev, args.interval)
        else:
            raise BQError(f"unknown command {command}")
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
