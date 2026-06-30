#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# build_test.sh -- build a test ESP image from pre-compiled kernel archives
# Usage: build_test.sh <cc> <ld> <srcdir> <boot.a> <kernel.a> <elf2efi> <output_esp> <init> [extra:name ...]
#   Steps: mkinitrd (cpio) -> link boot -> elf2efi -> link kernel -> mk_esp (with initrd.cpio)
set -eu

CC="$1"; LD="$2"; SRCDIR="$3"; BOOT_A="$4"; KERNEL_A="$5"
ELF2EFI="$6"; OUTPUT_ESP="$7"; INIT="$8"
shift 8
# Remaining args: extra files as path:name for initrd

# Resolve relative paths to absolute (meson runs from build dir)
case "$LD"            in /*) ;; *) LD="$(pwd)/$LD" ;; esac
case "$BOOT_A"        in /*) ;; *) BOOT_A="$(pwd)/$BOOT_A" ;; esac
case "$KERNEL_A"      in /*) ;; *) KERNEL_A="$(pwd)/$KERNEL_A" ;; esac
case "$ELF2EFI"       in /*) ;; *) ELF2EFI="$(pwd)/$ELF2EFI" ;; esac
case "$OUTPUT_ESP"    in /*) ;; *) OUTPUT_ESP="$(pwd)/$OUTPUT_ESP" ;; esac
case "$INIT"          in /*) ;; *) INIT="$(pwd)/$INIT" ;; esac

WORK_DIR=$(mktemp -d); trap 'rm -rf "$WORK_DIR"' EXIT

# 1. Build initrd cpio (raw, not wrapped as .o)
INITRD_DIR="$WORK_DIR/initrd"
mkdir -p "$INITRD_DIR"
cp "$INIT" "$INITRD_DIR/init"
for arg in "$@"; do
    src="${arg%%:*}"
    name="${arg#*:}"
    cp "$src" "$INITRD_DIR/$name"
done
(cd "$INITRD_DIR" && find . -print | cpio -o -H newc > "$WORK_DIR/initrd.cpio" 2>/dev/null)

# 2. Link bootloader
"$SRCDIR/scripts/compile_kernel.sh" link "$LD" "$SRCDIR" \
    --boot-elf "$WORK_DIR/boot.elf" --boot-a "$BOOT_A"

# 3. Convert bootloader to EFI
"$ELF2EFI" "$WORK_DIR/boot.elf" "$WORK_DIR/BOOTX64.EFI"

# 4. Link kernel
"$SRCDIR/scripts/compile_kernel.sh" link "$LD" "$SRCDIR" \
    --kernel-elf "$WORK_DIR/kernel.elf" --kernel-a "$KERNEL_A"

# 5. Build ESP with bootloader, kernel, and initrd
"$SRCDIR/scripts/mk_esp.sh" "$WORK_DIR/BOOTX64.EFI" "$OUTPUT_ESP" \
    "/boot/kernel.elf:$WORK_DIR/kernel.elf" \
    "/boot/initrd.cpio:$WORK_DIR/initrd.cpio"
