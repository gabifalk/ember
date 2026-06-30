#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# mk_esp.sh -- create MBR-partitioned FAT ESP disk image with BOOTX64.EFI
# Usage: mk_esp.sh <efi_binary> <output_img> [/path/on/esp:local_file ...]
set -eu

EFI_BIN="$1"
OUTPUT="$2"
shift 2

# Determine image size: EFI binary + extra files + overhead, minimum 34MB for valid FAT32
EFI_SIZE=$(stat -c%s "$EFI_BIN")
EXTRA_SIZE=0
for arg in "$@"; do
    LOCAL_FILE="${arg#*:}"
    FILE_SIZE=$(stat -c%s "$LOCAL_FILE")
    EXTRA_SIZE=$((EXTRA_SIZE + FILE_SIZE))
done

NEED_BYTES=$((1048576 + 65536 + EFI_SIZE + EXTRA_SIZE + 131072))
IMG_MB=$(( (NEED_BYTES + 1048575) / 1048576 ))
if [ "$IMG_MB" -lt 34 ]; then IMG_MB=34; fi
IMG_SIZE=$((IMG_MB * 1048576))
PART_START=2048  # LBA (1MB offset)
TOTAL_SECTORS=$((IMG_SIZE / 512))
PART_SECTORS=$((TOTAL_SECTORS - PART_START))
FAT_SIZE=$((PART_SECTORS * 512))

# Create FAT partition image
FAT_TMP=$(mktemp)
trap 'rm -f "$FAT_TMP"' EXIT
truncate -s "$FAT_SIZE" "$FAT_TMP"
mformat -i "$FAT_TMP" -F :: 2>/dev/null
mmd -i "$FAT_TMP" ::/EFI ::/EFI/BOOT
mcopy -i "$FAT_TMP" "$EFI_BIN" ::/EFI/BOOT/BOOTX64.EFI

# Copy additional files
CREATED_DIRS=""
for arg in "$@"; do
    ESP_PATH="${arg%%:*}"
    LOCAL_FILE="${arg#*:}"
    ESP_DIR=$(dirname "$ESP_PATH")
    if [ "$ESP_DIR" != "." ] && [ "$ESP_DIR" != "/" ]; then
        case " $CREATED_DIRS " in
            *" $ESP_DIR "*) ;;
            *) mmd -i "$FAT_TMP" "::${ESP_DIR}"; CREATED_DIRS="$CREATED_DIRS $ESP_DIR" ;;
        esac
    fi
    mcopy -i "$FAT_TMP" "$LOCAL_FILE" "::${ESP_PATH}"
done

# Build disk image with MBR
truncate -s "$IMG_SIZE" "$OUTPUT"
echo ",,ef,*" | sfdisk -q "$OUTPUT"
dd if="$FAT_TMP" of="$OUTPUT" bs=512 seek=$PART_START conv=notrunc status=none
