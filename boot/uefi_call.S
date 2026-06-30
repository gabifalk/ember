// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text
    .globl uefi_call2
    .globl uefi_call3
    .globl uefi_call4
    .globl uefi_call5

/* SysV ABI entry: rdi=fn, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5 */
uefi_call2:
    push %rsp
    push (%rsp)
    and $-16, %rsp
    sub $32, %rsp
    mov %rsi, %rcx
    /* rdx already a2 */
    call *%rdi
    mov 40(%rsp), %rsp
    ret

uefi_call3:
    push %rsp
    push (%rsp)
    and $-16, %rsp
    sub $32, %rsp
    mov %rcx, %r8
    mov %rsi, %rcx
    /* rdx already a2 */
    call *%rdi
    mov 40(%rsp), %rsp
    ret

uefi_call4:
    push %rsp
    push (%rsp)
    and $-16, %rsp
    sub $32, %rsp
    mov %r8, %r9
    mov %rcx, %r8
    mov %rsi, %rcx
    /* rdx already a2 */
    call *%rdi
    mov 40(%rsp), %rsp
    ret

uefi_call5:
    push %rsp
    push (%rsp)
    and $-16, %rsp
    sub $32, %rsp
    mov %r9, 32(%rsp)
    mov %r8, %r9
    mov %rcx, %r8
    mov %rsi, %rcx
    /* rdx already a2 */
    call *%rdi
    mov 40(%rsp), %rsp
    ret

.section .note.GNU-stack,"",@progbits
