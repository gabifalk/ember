#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# run_test.sh -- run a pre-built test ESP image in QEMU, check serial output
# Usage: run_test.sh <esp_img> [--ext2 <ext2_img>] [--machine <q35|pc>]
# Env: EXTRA_QEMU_ARGS (optional)
set -eu

ESP_IMG="$1"; shift

EXT2_IMG=""
MACHINE="q35"
SMP="1"

while [ $# -gt 0 ]; do
    case "$1" in
        --ext2)    EXT2_IMG="$2"; shift 2 ;;
        --machine) MACHINE="$2"; shift 2 ;;
        --smp)     SMP="$2"; shift 2 ;;
        *)         echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

EXTRA_QEMU_ARGS=${EXTRA_QEMU_ARGS:-}

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "OVMF firmware not found. Set OVMF_CODE and OVMF_VARS." >&2
    exit 77  # meson skip code
fi

WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

vars_copy="$WORK_DIR/OVMF_VARS.fd"
cp "$OVMF_VARS" "$vars_copy"

EXT2_ARGS=""
if [ -n "$EXT2_IMG" ]; then
    cp "$EXT2_IMG" "$WORK_DIR/ext2.img"
    if [ "$MACHINE" = "pc" ]; then
        EXT2_ARGS="-drive file=$WORK_DIR/ext2.img,format=raw,if=ide -device isa-debug-exit,iobase=0xf4,iosize=0x04"
    else
        EXT2_ARGS="-drive file=$WORK_DIR/ext2.img,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.2"
    fi
fi

# --- Run QEMU with timeout, capture serial output ---
SERIAL_LOG="$WORK_DIR/serial.log"

qemu-system-x86_64 \
    -m 512M \
    -machine "$MACHINE" \
    -cpu qemu64 \
    -smp "$SMP" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$vars_copy" \
    -drive if=none,format=raw,id=usbdisk,file="$ESP_IMG" \
    -device usb-ehci \
    -device usb-storage,drive=usbdisk \
    $EXT2_ARGS \
    $EXTRA_QEMU_ARGS \
    -nographic \
    -no-reboot \
    > "$SERIAL_LOG" 2>&1 || true

# --- Check output ---
echo "--- serial output ---"
cat "$SERIAL_LOG"
echo "--- end serial output ---"

if grep -q "FAIL" "$SERIAL_LOG"; then
    echo "TEST FAILED: found FAIL in output"
    exit 1
fi

if ! grep -q "passed" "$SERIAL_LOG"; then
    echo "TEST FAILED: no pass summary found"
    exit 1
fi

echo "TEST PASSED"
exit 0
