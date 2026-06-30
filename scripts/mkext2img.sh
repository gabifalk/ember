#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
set -eu

IMG="$1"

dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
mkfs.ext2 -b 1024 -q "$IMG"

# debugfs "write" requires a real file, not stdin
tmpfile=$(mktemp)
printf "hello from ext2\n" > "$tmpfile"
debugfs -w "$IMG" -R "write $tmpfile hello.txt" 2>/dev/null
rm -f "$tmpfile"

echo "Created ext2 image: $IMG"
