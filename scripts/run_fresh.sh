#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# run_fresh.sh -- run ember kernel from build/ with a fresh ext2 image
set -eu

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$SRCDIR/build"
EXT2_IMG="${EXT2_IMG:-/tmp/ember.ext2}"

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "OVMF firmware not found." >&2
    echo "Set OVMF_CODE and OVMF_VARS to your OVMF firmware paths." >&2
    exit 2
fi

EFI_BIN="$BUILD_DIR/BOOTX64.EFI"
if [ ! -f "$EFI_BIN" ]; then
    echo "Missing EFI binary: $EFI_BIN" >&2
    echo "Run: meson setup build && ninja -C build" >&2
    exit 2
fi

if [ ! -f "$EXT2_IMG" ]; then
    echo "Missing ext2 image: $EXT2_IMG" >&2
    exit 2
fi

# --- Prepare QEMU runtime directory ---
run_dir=$(mktemp -d)
trap 'rm -rf "$run_dir"' EXIT

vars_copy="$run_dir/OVMF_VARS.fd"
cp "$OVMF_VARS" "$vars_copy"

KERNEL_ELF="$BUILD_DIR/kernel.elf"
if [ ! -f "$KERNEL_ELF" ]; then
    echo "Missing kernel ELF: $KERNEL_ELF" >&2
    exit 2
fi

mkdir -p "$run_dir/EFI/BOOT" "$run_dir/boot"
cp "$EFI_BIN" "$run_dir/EFI/BOOT/BOOTX64.EFI"
cp "$KERNEL_ELF" "$run_dir/boot/kernel.elf"

# --- Run ---
echo "==> Starting QEMU..."
qemu-system-x86_64 \
    -m 4096M \
    -machine q35 \
    -cpu qemu64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$vars_copy" \
    -drive if=none,format=raw,id=usbdisk,file=fat:rw:"$run_dir" \
    -device usb-ehci \
    -device usb-storage,drive=usbdisk \
    -drive file="$EXT2_IMG",format=raw,if=none,id=disk0 \
    -device ide-hd,drive=disk0,bus=ide.2 \
    -nographic \
    -no-reboot \
    #
