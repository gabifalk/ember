#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# compile_m2.sh -- compile a C source to static ELF64 using the M2-Planet toolchain
# Usage: compile_m2.sh <m2_prefix> <source.c> <output>
#   m2_prefix must contain bin/M2-Planet, bin/M1, bin/hex2, and M2libc/
set -eu

PREFIX="$1"; SRC="$2"; OUTPUT="$3"

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# 1. M2-Planet: C -> M1 assembly
"$PREFIX/bin/M2-Planet" --architecture amd64 --expand-includes \
    -I "$PREFIX/M2libc" \
    -f "$SRC" \
    -f "$PREFIX/M2libc/amd64/linux/bootstrap.c" \
    -o "$TMP/out.M1"

# 2. M1: macro assembly -> hex2 object
"$PREFIX/bin/M1" \
    -f "$PREFIX/M2libc/amd64/amd64_defs.M1" \
    -f "$PREFIX/M2libc/amd64/libc-full.M1" \
    -f "$TMP/out.M1" \
    --little-endian --architecture amd64 \
    -o "$TMP/out.hex2"

# 3. hex2: link -> static ELF64
"$PREFIX/bin/hex2" \
    -f "$PREFIX/M2libc/amd64/ELF-amd64.hex2" \
    -f "$TMP/out.hex2" \
    --little-endian --architecture amd64 \
    --base-address 0x00600000 \
    -o "$OUTPUT"

chmod +x "$OUTPUT"
