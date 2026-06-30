// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text
    .globl _start

_start:
    int3

    mov $1, %rax          /* SYS_WRITE */
    mov $1, %rdi
    lea msg(%rip), %rsi
    mov $msg_len, %rdx
    syscall

    mov $60, %rax         /* SYS_EXIT */
    xor %rdi, %rdi
    syscall

    .section .rodata
msg:
    .ascii "int3 ok\n"
msg_len = . - msg
