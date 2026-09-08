#!/usr/bin/env bash
set -euo pipefail

BUS=1
ADDR=0x55

if (( EUID != 0 )); then
	echo "Error: run this script as root." >&2
	exit 1
fi

if ! command -v i2ctransfer >/dev/null 2>&1; then
	echo "Error: i2ctransfer not found; install the i2c-tools package." >&2
	exit 1
fi

if [[ ! -e "/dev/i2c-${BUS}" ]]; then
	echo "Error: /dev/i2c-${BUS} does not exist." >&2
	exit 1
fi

write_control_word()
{
	local value=$(( $1 ))
	local lo
	local hi

	lo=$(printf '0x%02x' $((value & 0xff)))
	hi=$(printf '0x%02x' $(((value >> 8) & 0xff)))
	i2ctransfer -y "$BUS" "w3@${ADDR}" 0x00 "$lo" "$hi"
}

read_dm_block()
{
	local label="$1"
	local addr=$(( $2 ))
	local lo
	local hi

	lo=$(printf '0x%02x' $((addr & 0xff)))
	hi=$(printf '0x%02x' $(((addr >> 8) & 0xff)))

	printf '\n%s  DM address=0x%04x\n' "$label" "$addr"
	i2ctransfer -y "$BUS" "w3@${ADDR}" 0x3e "$lo" "$hi"
	sleep 0.02
	i2ctransfer -y "$BUS" "w1@${ADDR}" 0x3e r36
}

echo "Unsealing BQ27220 on i2c-${BUS} address ${ADDR}..."
write_control_word 0x0414
sleep 0.05
write_control_word 0x3672
sleep 0.05

echo "Entering Full Access..."
write_control_word 0xffff
sleep 0.01
write_control_word 0xffff
sleep 0.02

echo "OperationStatus (SEC should be 0x02):"
i2ctransfer -y "$BUS" "w1@${ADDR}" 0x3a r2

read_dm_block "PROFILE_1_BLOCK_A" 0x48e7
read_dm_block "PROFILE_1_BLOCK_B" 0x4907

read_dm_block "PROFILE_2_BLOCK_A" 0x491f
read_dm_block "PROFILE_2_BLOCK_B" 0x493f

read_dm_block "PROFILE_3_BLOCK_A" 0x4957
read_dm_block "PROFILE_3_BLOCK_B" 0x4977

echo
echo "BQ27220 ROM profiles read complete."
