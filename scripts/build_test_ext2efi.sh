#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# build_test_ext2efi.sh -- build a test ESP image with ember.ext2 loaded by EFI
# Usage: build_test_ext2efi.sh <cc> <ld> <srcdir> <boot.a> <kernel.a> <elf2efi> <output_esp> <init>
#   Places an ext2 image at /boot/ember.ext2 on the ESP.
#   The EFI loader loads it before ExitBootServices.
set -eu

CC="$1"; LD="$2"; SRCDIR="$3"; BOOT_A="$4"; KERNEL_A="$5"
ELF2EFI="$6"; OUTPUT_ESP="$7"; INIT="$8"
shift 8

# Resolve relative paths to absolute
case "$LD"            in /*) ;; *) LD="$(pwd)/$LD" ;; esac
case "$BOOT_A"        in /*) ;; *) BOOT_A="$(pwd)/$BOOT_A" ;; esac
case "$KERNEL_A"      in /*) ;; *) KERNEL_A="$(pwd)/$KERNEL_A" ;; esac
case "$ELF2EFI"       in /*) ;; *) ELF2EFI="$(pwd)/$ELF2EFI" ;; esac
case "$OUTPUT_ESP"    in /*) ;; *) OUTPUT_ESP="$(pwd)/$OUTPUT_ESP" ;; esac
case "$INIT"          in /*) ;; *) INIT="$(pwd)/$INIT" ;; esac

WORK_DIR=$(mktemp -d); trap 'rm -rf "$WORK_DIR"' EXIT

# 1. Link bootloader (no initrd)
"$SRCDIR/scripts/compile_kernel.sh" link "$LD" "$SRCDIR" \
    --boot-elf "$WORK_DIR/boot.elf" --boot-a "$BOOT_A"

# 2. Convert to EFI
"$ELF2EFI" "$WORK_DIR/boot.elf" "$WORK_DIR/BOOTX64.EFI"

# 3. Link kernel
"$SRCDIR/scripts/compile_kernel.sh" link "$LD" "$SRCDIR" \
    --kernel-elf "$WORK_DIR/kernel.elf" --kernel-a "$KERNEL_A"

# 4. Build ext2 image with init + hello.txt
EXT2_IMG="$WORK_DIR/ember.ext2"
dd if=/dev/zero of="$EXT2_IMG" bs=1M count=4 2>/dev/null
mkfs.ext2 -b 1024 -q "$EXT2_IMG"

tmpfile=$(mktemp)
printf "hello from ext2\n" > "$tmpfile"
debugfs -w "$EXT2_IMG" -R "write $tmpfile hello.txt" 2>/dev/null
debugfs -w "$EXT2_IMG" -R "write $INIT init" 2>/dev/null
debugfs -w "$EXT2_IMG" -R "modify_inode init mode 0x81ed" 2>/dev/null
rm -f "$tmpfile"

# 5. Build ESP disk image with BOOTX64.EFI, kernel.elf, and /boot/ember.ext2
EFI_BIN="$WORK_DIR/BOOTX64.EFI"
KERN_ELF="$WORK_DIR/kernel.elf"
EFI_SIZE=$(stat -c%s "$EFI_BIN")
KERN_SIZE=$(stat -c%s "$KERN_ELF")
EXT2_SIZE=$(stat -c%s "$EXT2_IMG")
NEED_BYTES=$((1048576 + 65536 + EFI_SIZE + KERN_SIZE + EXT2_SIZE + 131072))
IMG_MB=$(( (NEED_BYTES + 1048575) / 1048576 ))
if [ "$IMG_MB" -lt 34 ]; then IMG_MB=34; fi
IMG_SIZE=$((IMG_MB * 1048576))
PART_START=2048
TOTAL_SECTORS=$((IMG_SIZE / 512))
PART_SECTORS=$((TOTAL_SECTORS - PART_START))
FAT_SIZE=$((PART_SECTORS * 512))

FAT_TMP=$(mktemp)
truncate -s "$FAT_SIZE" "$FAT_TMP"
mformat -i "$FAT_TMP" -F :: 2>/dev/null
mmd -i "$FAT_TMP" ::/EFI ::/EFI/BOOT ::/boot
mcopy -i "$FAT_TMP" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$FAT_TMP" "$KERN_ELF" ::/boot/kernel.elf
mcopy -i "$FAT_TMP" "$EXT2_IMG" ::/boot/ember.ext2

python3 -c "
import struct
img_size = $IMG_SIZE
part_start = $PART_START
part_sectors = $PART_SECTORS
with open('$OUTPUT_ESP', 'wb') as f:
    f.write(b'\x00' * img_size)
    mbr = bytearray(512)
    entry = bytearray(16)
    entry[0] = 0x80
    entry[4] = 0xEF
    entry[1], entry[2], entry[3] = 0, 1, 0
    entry[5], entry[6], entry[7] = 0xFF, 0xFF, 0xFF
    struct.pack_into('<I', entry, 8, part_start)
    struct.pack_into('<I', entry, 12, part_sectors)
    mbr[0x1BE:0x1CE] = entry
    mbr[0x1FE] = 0x55
    mbr[0x1FF] = 0xAA
    f.seek(0)
    f.write(bytes(mbr))
    with open('$FAT_TMP', 'rb') as fp:
        fat_data = fp.read()
    f.seek(part_start * 512)
    f.write(fat_data)
"
rm -f "$FAT_TMP"
