#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# run_e1000_icmp.sh <esp_img> [--smp N]
# Boot the kernel with an e1000 on a QEMU socket netdev and let the in-kernel
# net stack answer ARP + ICMP. This script owns QEMU's lifecycle: launch in the
# background, run the ICMP check helper, then kill QEMU. PASS requires the
# helper returning 0 and the serial log showing the echo reply was sent.
set -eu

ESP="$1"; shift
SMP="1"
while [ $# -gt 0 ]; do
    case "$1" in
        --smp) SMP="$2"; shift 2 ;;
        *)     echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
done

export PATH="/usr/sbin:/sbin:$PATH"

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "OVMF firmware not found. Set OVMF_CODE and OVMF_VARS." >&2
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found; skipping" >&2
    exit 77
fi

PORT=$(( (`date +%s` % 20000) + 20000 ))
WORK_DIR=$(mktemp -d)
QEMU_PID=""

cleanup() {
    if [ -n "$QEMU_PID" ]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

vars_copy="$WORK_DIR/OVMF_VARS.fd"
cp "$OVMF_VARS" "$vars_copy"
SERIAL_LOG="$WORK_DIR/serial.log"

qemu-system-x86_64 \
    -m 512M \
    -machine q35 \
    -cpu qemu64 \
    -smp "$SMP" \
    -nic none \
    -device e1000,netdev=n0 \
    -netdev "socket,id=n0,listen=127.0.0.1:$PORT" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$vars_copy" \
    -drive if=none,format=raw,readonly=on,id=usbdisk,file="$ESP" \
    -device usb-ehci \
    -device usb-storage,drive=usbdisk \
    -nographic \
    -no-reboot \
    > "$SERIAL_LOG" 2>&1 &
QEMU_PID=$!

RC=0
python3 "$(dirname "$0")/e1000_icmp_check.py" "$PORT" || RC=$?

echo "--- serial output ---"
cat "$SERIAL_LOG" 2>/dev/null || true
echo "--- end serial output ---"

if [ "$RC" -ne 0 ]; then
    echo "FAIL: ICMP check helper returned $RC"
    exit 1
fi
if ! grep -q "icmp: echo reply sent" "$SERIAL_LOG"; then
    echo "FAIL: kernel did not log 'icmp: echo reply sent'"
    exit 1
fi

echo "e1000 ICMP verified"
exit 0
