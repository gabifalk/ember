// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text
    .globl efi_main
    .globl _start

/* UEFI entry uses Microsoft x64 ABI: rcx=ImageHandle, rdx=SystemTable */
_start:
efi_main:
    push %rbp
    mov %rsp, %rbp
    sub $32, %rsp
    mov %rcx, %rdi
    mov %rdx, %rsi
    call efi_main_c
    add $32, %rsp
    pop %rbp
    ret

.section .note.GNU-stack,"",@progbits
