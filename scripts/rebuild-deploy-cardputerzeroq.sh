#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
KERNELDIR="${KERNELDIR:-/home/nihao/w2T/arduino/linux}"
KERNEL_OUT="${KERNEL_OUT:-}"
ARCH="${ARCH:-arm64}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
CC="${CC:-$ROOT_DIR/.artifacts/aarch64-gcc13-kbuild-wrapper}"
ADB_SERIAL="${ADB_SERIAL:-3023797681}"
REBOOT=0

CARDPUTER_DIR="$ROOT_DIR/modules/CardputerZeroQ"
JD9852_DIR="$ROOT_DIR/modules/jd9852a"
TCA8418_DIR="$ROOT_DIR/modules/tca8418-1.0"
STAMP="$(date +%Y%m%d-%H%M%S)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/.artifacts/cardputerzeroq-deploy-$STAMP}"

BASE_DTB_NAME=qrb2210-arduino-imola-base.dtb
VIDEO_DTBO_NAME=qrb2210-arduino-imola-video_sound-usbc.dtbo
FINAL_DTB_NAME=qrb2210-arduino-imola-cardputerzeroq.dtb

usage() {
	cat <<EOF
Usage: scripts/rebuild-deploy-cardputerzeroq.sh [options]

Build the CardputerZeroQ, JD9852 and TCA8418 modules, build a complete DTB,
upload the artifacts to an Arduino Uno Q, and refresh its initramfs.

Options:
  --serial SERIAL  ADB serial (default: $ADB_SERIAL)
  --reboot         Reboot the device after installation
  --help           Show this help

The device sudo password is read interactively. Set DEVICE_PASSWORD to avoid
the prompt in an already protected shell.
EOF
}

die() {
	echo "error: $*" >&2
	exit 1
}

while (($#)); do
	case "$1" in
		--serial)
			(($# >= 2)) || die "--serial needs a value"
			ADB_SERIAL="$2"
			shift 2
			;;
		--reboot)
			REBOOT=1
			shift
			;;
		--help|-h)
			usage
			exit 0
			;;
		*)
			die "unknown option: $1"
			;;
	esac
done

for command in adb make dtc fdtoverlay; do
	command -v "$command" >/dev/null || die "missing host command: $command"
done
[[ -f "$KERNELDIR/Makefile" ]] || die "kernel source not found: $KERNELDIR"
[[ -x "$CC" ]] || die "compiler wrapper not found or not executable: $CC"
[[ -f "$CARDPUTER_DIR/cardputerzeroq-overlay.dts" ]] || die "CardputerZeroQ DTS not found"

if [[ -z "${DEVICE_PASSWORD:-}" ]]; then
	read -r -s -p "Device sudo password: " DEVICE_PASSWORD
	printf '\n' >&2
fi

mkdir -p "$BUILD_DIR"

KERNEL_ARGS=(-C "$KERNELDIR")
if [[ -n "$KERNEL_OUT" ]]; then
	KERNEL_ARGS+=(O="$KERNEL_OUT")
	KERNEL_TREE="$KERNEL_OUT"
else
	KERNEL_TREE="$KERNELDIR"
fi

adb -s "$ADB_SERIAL" get-state >/dev/null || die "ADB device is not ready: $ADB_SERIAL"
REMOTE_KERNEL="$(adb -s "$ADB_SERIAL" shell uname -r | tr -d '\r')"
UTS_RELEASE_FILE="$KERNEL_TREE/include/generated/utsrelease.h"
[[ -f "$UTS_RELEASE_FILE" ]] || die "kernel UTS release header not found: $UTS_RELEASE_FILE"
LOCAL_KERNEL="$(sed -n 's/^#define UTS_RELEASE "\([^"]*\)"/\1/p' "$UTS_RELEASE_FILE")"
[[ "$REMOTE_KERNEL" == "$LOCAL_KERNEL" ]] || die \
	"kernel mismatch: device=$REMOTE_KERNEL build=$LOCAL_KERNEL"

MODULE_DTB_ARGS=(KERNELDIR="$KERNELDIR")
if [[ -n "$KERNEL_OUT" ]]; then
	MODULE_DTB_ARGS+=(KERNEL_OUT="$KERNEL_OUT")
fi

echo "[1/5] Building CardputerZeroQ modules"
make "${KERNEL_ARGS[@]}" M="$CARDPUTER_DIR" \
	ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" CC="$CC" \
	KBUILD_MODPOST_WARN=1 modules
make -C "$CARDPUTER_DIR" "${MODULE_DTB_ARGS[@]}" \
	ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" DTC=dtc CPP=cpp dtbs

echo "[2/5] Building JD9852 and TCA8418 modules"
make "${KERNEL_ARGS[@]}" M="$JD9852_DIR" \
	ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" CC="$CC" \
	KBUILD_MODPOST_WARN=1 modules
make -C "$JD9852_DIR" KERNELDIR="$KERNELDIR" \
	ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" jd9852a.dtbo
make "${KERNEL_ARGS[@]}" M="$TCA8418_DIR" \
	ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" CC="$CC" \
	KBUILD_MODPOST_WARN=1 modules

echo "[3/5] Building the base DTB and merging the CardputerZeroQ overlay"
make "${KERNEL_ARGS[@]}" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	"qcom/$BASE_DTB_NAME" \
	"qcom/$VIDEO_DTBO_NAME"

BASE_DTB="$KERNEL_TREE/arch/$ARCH/boot/dts/qcom/$BASE_DTB_NAME"
VIDEO_DTBO="$KERNEL_TREE/arch/$ARCH/boot/dts/qcom/$VIDEO_DTBO_NAME"
CARDPUTER_DTBO="$CARDPUTER_DIR/cardputerzeroq-overlay.dtbo"
MERGED_VIDEO="$BUILD_DIR/qrb2210-arduino-imola-video.dtb"
FINAL_DTB="$BUILD_DIR/$FINAL_DTB_NAME"

[[ -f "$BASE_DTB" ]] || die "base DTB was not built: $BASE_DTB"
[[ -f "$VIDEO_DTBO" ]] || die "video overlay was not built: $VIDEO_DTBO"
[[ -f "$CARDPUTER_DTBO" ]] || die "CardputerZeroQ overlay was not built: $CARDPUTER_DTBO"

fdtoverlay -i "$BASE_DTB" -o "$MERGED_VIDEO" "$VIDEO_DTBO"
fdtoverlay -i "$MERGED_VIDEO" -o "$FINAL_DTB" "$CARDPUTER_DTBO"

strings "$FINAL_DTB" > "$BUILD_DIR/final-dtb.strings"
if ! grep -Fq 'elida,jd9852' "$BUILD_DIR/final-dtb.strings"; then
	die "merged DTB does not contain the JD9852 panel node"
fi
if ! grep -Fq 'm5stack,m5io-hub' "$BUILD_DIR/final-dtb.strings"; then
	die "merged DTB does not contain the M5IO-HUB node"
fi

cp "$JD9852_DIR/panel-elida-jd9852a.ko" "$BUILD_DIR/"
for module in m5io-hub-core m5io-hub-gpio m5hub-gpio-leds m5io-hub-i2c; do
	cp "$CARDPUTER_DIR/$module.ko" "$BUILD_DIR/"
done
cp "$TCA8418_DIR/tca8418_keypad_m5stack.ko" "$BUILD_DIR/"
cp "$CARDPUTER_DIR/99-cardputerzeroq-keyboard.rules" "$BUILD_DIR/"
cp "$TCA8418_DIR/tca8418_keypad_m5stack_keymap.map" "$BUILD_DIR/"

echo "[4/5] Uploading modules and DTB to ADB device $ADB_SERIAL"

REMOTE_TMP="/tmp/cardputerzeroq-deploy-$$"
adb -s "$ADB_SERIAL" shell "mkdir -p '$REMOTE_TMP'"
for artifact in \
	panel-elida-jd9852a.ko \
	m5io-hub-core.ko \
	m5io-hub-gpio.ko \
	m5hub-gpio-leds.ko \
	m5io-hub-i2c.ko \
	tca8418_keypad_m5stack.ko \
	99-cardputerzeroq-keyboard.rules \
	tca8418_keypad_m5stack_keymap.map; do
	adb -s "$ADB_SERIAL" push "$BUILD_DIR/$artifact" "$REMOTE_TMP/$artifact" >/dev/null
done
adb -s "$ADB_SERIAL" push "$FINAL_DTB" "$REMOTE_TMP/$FINAL_DTB_NAME" >/dev/null

remote_root_script() {
	{
		printf '%s\n' "$DEVICE_PASSWORD"
		cat
	} | adb -s "$ADB_SERIAL" shell 'sudo -S -p "" sh -s'
}

remote_root_script <<REMOTE
set -eu
KVER=\$(uname -r)
MODULE_DIR=/usr/lib/modules/\$KVER/extra
install -d "\$MODULE_DIR" /etc/udev/rules.d /usr/share/keymaps
for module in \
    panel-elida-jd9852a \
    m5io-hub-core \
    m5io-hub-gpio \
    m5hub-gpio-leds \
    m5io-hub-i2c \
    tca8418_keypad_m5stack; do
    install -m 0644 "$REMOTE_TMP/\$module.ko" "\$MODULE_DIR/\$module.ko"
done
install -m 0644 "$REMOTE_TMP/99-cardputerzeroq-keyboard.rules" \
    /etc/udev/rules.d/99-cardputerzeroq-keyboard.rules
install -m 0644 "$REMOTE_TMP/tca8418_keypad_m5stack_keymap.map" \
    /usr/share/keymaps/tca8418_keypad_m5stack_keymap.map

DTB_PATH=/boot/efi/dtb/qcom/qrb2210-arduino-imola.dtb
DTB_ALIAS=/boot/efi/dtb/qcom/qrb2210-arduino-imola-jd9852.dtb
if [ -f "\$DTB_PATH" ]; then
    cp -p "\$DTB_PATH" "\$DTB_PATH.before-cardputerzeroq-\$(date +%Y%m%d-%H%M%S)"
fi
install -m 0644 "$REMOTE_TMP/$FINAL_DTB_NAME" "\$DTB_PATH"
install -m 0644 "$REMOTE_TMP/$FINAL_DTB_NAME" "\$DTB_ALIAS"

depmod -a "\$KVER"
update-initramfs -u -k "\$KVER"
MACHINE_ID=\$(cat /etc/machine-id)
EFI_INITRD=/boot/efi/\$MACHINE_ID/\$KVER/initrd.img-\$KVER
if [ -f /boot/initrd.img-\$KVER ] && [ -d "\$(dirname "\$EFI_INITRD")" ]; then
    install -m 0755 /boot/initrd.img-\$KVER "\$EFI_INITRD"
fi
sync
sha256sum "\$DTB_PATH" "\$MODULE_DIR/panel-elida-jd9852a.ko"
REMOTE

adb -s "$ADB_SERIAL" shell "rm -rf '$REMOTE_TMP'"

echo "Artifacts saved in: $BUILD_DIR"
sha256sum "$FINAL_DTB" "$BUILD_DIR/panel-elida-jd9852a.ko"

if ((REBOOT)); then
	echo "[5/5] Rebooting device"
	printf '%s\n' "$DEVICE_PASSWORD" | \
		adb -s "$ADB_SERIAL" shell 'sudo -S -p "" systemctl reboot' || true
	cat <<'EOF'
Device reboot requested. After ADB returns, verify:
  cat /sys/class/drm/*DSI*/modes
  dmesg | grep -E 'DSI config|jd9852|vblank|FIFO|timeout'
EOF
else
	echo "[5/5] Upload complete; reboot was not requested"
	echo "Run again with --reboot to activate the new DTB and modules."
fi
