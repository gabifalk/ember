// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text
    .globl tss_load

/* tss_load(sel, new_rsp) */
tss_load:
    mov %rdi, %rax
    mov %rsi, %r8
    mov %rsp, %r9
    mov %r8, %rsp
    ltr %ax
    mov %r9, %rsp
    ret

.section .note.GNU-stack,"",@progbits
