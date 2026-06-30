#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
set -eu

IMG="$1"
TCC_BIN="$2"
HELLO_C="$3"

# 8 MiB image to fit tcc (~485 KB) plus room for compiled output
dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null
mkfs.ext2 -b 1024 -q "$IMG"

# Create /bin directory and copy tcc
debugfs -w "$IMG" -R "mkdir bin" 2>/dev/null
debugfs -w "$IMG" -R "write $TCC_BIN bin/tcc" 2>/dev/null

# Copy the test C source
debugfs -w "$IMG" -R "write $HELLO_C hello.c" 2>/dev/null

# hello.txt needed by the generic ext2 tests (tests 1/3/4)
tmpfile=$(mktemp)
printf "hello from ext2\n" > "$tmpfile"
debugfs -w "$IMG" -R "write $tmpfile hello.txt" 2>/dev/null
rm -f "$tmpfile"

echo "Created tcc ext2 image: $IMG"
