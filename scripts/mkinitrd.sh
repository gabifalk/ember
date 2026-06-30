#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# mkinitrd.sh -- build a cpio newc initrd and wrap it as an ELF .o
# Usage: mkinitrd.sh <output.o> <ld> <init_binary> [src:name ...]
#   Arg 1: output initrd.o path
#   Arg 2: ld path
#   Arg 3: init binary -> /init in initrd
#   Args 4+: extra files as path:name -> /name in initrd
set -eu

OUTPUT="$1"; LD="$2"; INIT="$3"
shift 3

TMPDIR=$(mktemp -d); trap 'rm -rf "$TMPDIR"' EXIT
cp "$INIT" "$TMPDIR/init"

# Copy extra files
for arg in "$@"; do
    src="${arg%%:*}"
    name="${arg#*:}"
    cp "$src" "$TMPDIR/$name"
done

(cd "$TMPDIR" && find . -print | cpio -o -H newc > initrd.cpio 2>/dev/null)

# ld -r -b binary derives symbol names from the input filename.
# We must cd so the input is just "initrd.cpio", giving us
# _binary_initrd_cpio_start / _binary_initrd_cpio_end.
OUTPUT_ABS="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
(cd "$TMPDIR" && "$LD" -r -b binary -o "$OUTPUT_ABS" initrd.cpio)
