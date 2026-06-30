#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
set -eu

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "OVMF firmware not found." >&2
    echo "Set OVMF_CODE and OVMF_VARS to your OVMF firmware paths." >&2
    exit 2
fi

BUILD_DIR=${BUILD_DIR:-build}
EFI_BIN=${EFI_BIN:-$BUILD_DIR/BOOTX64.EFI}

if [ ! -f "$EFI_BIN" ]; then
    echo "Missing EFI binary: $EFI_BIN" >&2
    exit 2
fi

run_dir=$(mktemp -d)
trap 'rm -rf "$run_dir"' EXIT

vars_copy="$run_dir/OVMF_VARS.fd"
cp "$OVMF_VARS" "$vars_copy"

KERNEL_ELF=${KERNEL_ELF:-$BUILD_DIR/kernel.elf}

mkdir -p "$run_dir/EFI/BOOT" "$run_dir/boot"
cp "$EFI_BIN" "$run_dir/EFI/BOOT/BOOTX64.EFI"
if [ -f "$KERNEL_ELF" ]; then
    cp "$KERNEL_ELF" "$run_dir/boot/kernel.elf"
fi

EXT2_IMG=${EXT2_IMG:-$BUILD_DIR/ext2.img}
EXT2_ARGS=""
if [ -f "$EXT2_IMG" ]; then
    EXT2_ARGS="-drive file=$EXT2_IMG,format=raw,if=none,id=disk0 -device ide-hd,drive=disk0,bus=ide.2"
fi

SMP=${SMP:-2}

qemu-system-x86_64 \
    -m 512M \
    -machine q35 \
    -cpu qemu64 \
    -smp "$SMP" \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$vars_copy" \
    -drive if=none,format=raw,id=usbdisk,file=fat:rw:"$run_dir" \
    -device usb-ehci \
    -device usb-storage,drive=usbdisk \
    $EXT2_ARGS \
    -nographic \
    -no-reboot \
    #
