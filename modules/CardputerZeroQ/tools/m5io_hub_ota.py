#!/usr/bin/env python3
"""Upgrade CardputerQ through the m5io-hub-core OTA ioctl.

The kernel driver performs the complete synchronous IAP transaction.  This
script only keeps the image buffer alive, submits the ioctl, and decodes the
result returned by the driver.
"""

from __future__ import annotations

import argparse
import binascii
import ctypes
import errno
import fcntl
import os
import struct
import sys
from pathlib import Path


OTA_START_ADDRESS = 0x08002800
OTA_END_ADDRESS = 0x08020000
OTA_PAGE_SIZE = 0x0800
OTA_IMAGE_SIZE = OTA_END_ADDRESS - OTA_START_ADDRESS
OTA_PAGE_COUNT = OTA_IMAGE_SIZE // OTA_PAGE_SIZE
OTA_APP_REGION_SIZE = 0x0801E000 - OTA_START_ADDRESS
OTA_APP_PAYLOAD_SIZE = OTA_APP_REGION_SIZE - 4
OTA_USER_DATA_SIZE = OTA_END_ADDRESS - 0x0801E000

IOC_MAGIC = ord("M")
IOC_WRITE = 1
IOC_READ = 2
IOC_WRITE_READ = IOC_WRITE | IOC_READ
IOC_NRBITS = 8
IOC_TYPEBITS = 8
IOC_SIZEBITS = 14
IOC_NRSHIFT = 0
IOC_TYPESHIFT = IOC_NRSHIFT + IOC_NRBITS
IOC_SIZESHIFT = IOC_TYPESHIFT + IOC_TYPEBITS
IOC_DIRSHIFT = IOC_SIZESHIFT + IOC_SIZEBITS

# struct m5io_hub_ota_request: __u64 + 5*__u32 + __u16 + 2*__u8 = 32 bytes.
REQUEST_FORMAT = "<QIIIIIHBB"
REQUEST_SIZE = struct.calcsize(REQUEST_FORMAT)
M5IO_HUB_IOCTL_OTA = (
    (IOC_WRITE_READ << IOC_DIRSHIFT)
    | (IOC_MAGIC << IOC_TYPESHIFT)
    | (0x01 << IOC_NRSHIFT)
    | (REQUEST_SIZE << IOC_SIZESHIFT)
)

STATUS_NAMES = {
    0: "OK",
    1: "NOT_ENTERED",
    2: "BAD_COMMAND",
    3: "BAD_LENGTH",
    4: "BAD_ADDRESS",
    5: "ERASE_ERROR",
    6: "PROGRAM_ERROR",
    7: "VERIFY_ERROR",
    8: "APP_INVALID",
    9: "TRANSPORT_ERROR",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Upgrade CardputerQ through /dev/m5io-hub0."
    )
    parser.add_argument(
        "image",
        type=Path,
        help=(
            f"IAP image ({OTA_IMAGE_SIZE} bytes), packed APP image "
            f"({OTA_APP_REGION_SIZE} bytes), or raw APP .bin"
        ),
    )
    parser.add_argument(
        "-d",
        "--device",
        default="/dev/m5io-hub0",
        help="M5IO-HUB OTA device (default: /dev/m5io-hub0)",
    )
    parser.add_argument(
        "-t",
        "--timeout-ms",
        type=int,
        default=2000,
        help="timeout for each MCU IAP command, 100..30000 ms (default: 2000)",
    )
    return parser.parse_args()


def package_image(source: bytes) -> bytearray:
    """Turn a raw/APP image into the fixed-size image expected by the ioctl."""
    if len(source) == OTA_IMAGE_SIZE:
        return bytearray(source)

    if len(source) == OTA_APP_REGION_SIZE:
        return bytearray(source) + b"\xff" * OTA_USER_DATA_SIZE

    if len(source) > OTA_APP_PAYLOAD_SIZE:
        raise ValueError(
            f"image is {len(source)} bytes; expected {OTA_IMAGE_SIZE} bytes "
            f"(complete IAP), {OTA_APP_REGION_SIZE} bytes (packed APP), or "
            f"at most {OTA_APP_PAYLOAD_SIZE} bytes (raw APP)"
        )

    if len(source) < 8:
        raise ValueError("raw APP image is too short for an ARM vector table")
    stack_pointer, reset_vector = struct.unpack_from("<II", source)
    if not (0x20000000 <= stack_pointer <= 0x20009000 and stack_pointer % 4 == 0):
        raise ValueError(f"invalid initial stack pointer: 0x{stack_pointer:08x}")
    if not (reset_vector & 1) or not (
        OTA_START_ADDRESS <= (reset_vector & ~1) < 0x0801DFFC
    ):
        raise ValueError(f"invalid APP reset vector: 0x{reset_vector:08x}")

    payload = bytearray(source)
    payload.extend(b"\xff" * (OTA_APP_PAYLOAD_SIZE - len(payload)))
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    return payload + struct.pack("<I", crc) + b"\xff" * OTA_USER_DATA_SIZE


def make_request(image: bytearray, timeout_ms: int) -> bytearray:
    image_address = ctypes.addressof(ctypes.c_ubyte.from_buffer(image))
    return bytearray(
        struct.pack(
            REQUEST_FORMAT,
            image_address,
            len(image),
            timeout_ms,
            0,
            0xFF,
            0,
            0,
            0,
            0,
        )
    )


def decode_request(request: bytearray) -> dict[str, int]:
    values = struct.unpack(REQUEST_FORMAT, request)
    return {
        "completed_pages": values[3],
        "status": values[4],
        "address": values[5],
        "length": values[6],
        "command": values[7],
    }


def status_name(status: int) -> str:
    return STATUS_NAMES.get(status, f"UNKNOWN({status})")


def main() -> int:
    args = parse_args()

    if sys.platform != "linux":
        print("error: this ioctl script requires Linux", file=sys.stderr)
        return 2
    if not 100 <= args.timeout_ms <= 30000:
        print("error: --timeout-ms must be between 100 and 30000", file=sys.stderr)
        return 2

    try:
        source = args.image.read_bytes()
    except OSError as exc:
        print(f"error: cannot read {args.image}: {exc}", file=sys.stderr)
        return 2

    try:
        image = package_image(source)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    request = make_request(image, args.timeout_ms)
    print(
        f"Starting OTA: {args.image} ({len(image)} bytes, {OTA_PAGE_COUNT} pages)"
    )
    try:
        fd = os.open(args.device, os.O_RDWR | os.O_CLOEXEC)
    except OSError as exc:
        print(f"error: cannot open {args.device}: {exc}", file=sys.stderr)
        return 1

    try:
        try:
            fcntl.ioctl(fd, M5IO_HUB_IOCTL_OTA, request, True)
        except OSError as exc:
            result = decode_request(request)
            detail = (
                f"pages={result['completed_pages']}/{OTA_PAGE_COUNT}, "
                f"MCU status={status_name(result['status'])}, "
                f"command=0x{result['command']:02x}, "
                f"address=0x{result['address']:08x}"
            )
            if exc.errno == errno.EBUSY:
                detail += "; another hub operation or OTA is active"
            elif exc.errno == errno.ETIMEDOUT:
                detail += "; HOST_READY response timed out"
            print(f"OTA failed: {exc} ({detail})", file=sys.stderr)
            return 1
    finally:
        os.close(fd)

    result = decode_request(request)
    print(
        "OTA complete: "
        f"pages={result['completed_pages']}/{OTA_PAGE_COUNT}, "
        f"status={status_name(result['status'])}, "
        f"command=0x{result['command']:02x}, "
        f"address=0x{result['address']:08x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
