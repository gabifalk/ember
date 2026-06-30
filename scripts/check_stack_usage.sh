#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
#
# check_stack_usage.sh -- flag kernel functions with dangerously large stack frames
#
# Usage: check_stack_usage.sh <kernel.a>
#
# Disassembles the archive and looks for sub $imm,%rsp and add $-imm,%rsp
# instructions (TCC sometimes uses add with a negative immediate instead of sub).
# Reports any function allocating more than 8192 bytes of stack and exits 1.

set -eu

THRESHOLD=8192

if [ $# -ne 1 ]; then
    echo "Usage: $0 <kernel.a>" >&2
    exit 1
fi

ARCHIVE="$1"

if [ ! -f "$ARCHIVE" ]; then
    echo "ERROR: $ARCHIVE not found" >&2
    exit 1
fi

objdump -d "$ARCHIVE" | awk -v thresh="$THRESHOLD" '
    function hex2dec(s,    i, v, c) {
        v = 0
        for (i = 1; i <= length(s); i++) {
            c = substr(s, i, 1)
            if      (c >= "0" && c <= "9") v = v * 16 + (c + 0)
            else if (c == "a" || c == "A") v = v * 16 + 10
            else if (c == "b" || c == "B") v = v * 16 + 11
            else if (c == "c" || c == "C") v = v * 16 + 12
            else if (c == "d" || c == "D") v = v * 16 + 13
            else if (c == "e" || c == "E") v = v * 16 + 14
            else if (c == "f" || c == "F") v = v * 16 + 15
        }
        return v
    }
    / <[^>]+>:/ {
        fn = $0
        sub(/.*</, "", fn)
        sub(/>:.*/, "", fn)
    }
    /sub    \$0x[0-9a-fA-F]+,%rsp/ {
        imm = $0
        sub(/.*sub    \$0x/, "", imm)
        sub(/,%rsp.*/, "", imm)
        dec = hex2dec(imm)
        if (dec > thresh) {
            printf "WARNING: %s allocates %d bytes of stack\n", fn, dec
            found++
        }
    }
    /add    \$0xffffffff[0-9a-fA-F]+,%rsp/ {
        imm = $0
        sub(/.*add    \$0xffffffff/, "", imm)
        sub(/,%rsp.*/, "", imm)
        alloc = 4294967296 - hex2dec(imm)
        if (alloc > thresh) {
            printf "WARNING: %s allocates %d bytes of stack\n", fn, alloc
            found++
        }
    }
    /add    \$-[0-9]+,%rsp/ {
        imm = $0
        sub(/.*add    \$-/, "", imm)
        sub(/,%rsp.*/, "", imm)
        if (imm + 0 > thresh) {
            printf "WARNING: %s allocates %d bytes of stack\n", fn, imm + 0
            found++
        }
    }
    END { exit (found > 0) ? 1 : 0 }
'
