// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
.text
.globl kexec_jump
.type kexec_jump, @function

/* kexec_jump(entry, boot_params_phys, identity_pml4_phys)
 *   rdi = entry point (physical address)
 *   rsi = boot_params physical address
 *   rdx = identity-mapped PML4 physical address
 *
 * Does not return.
 */
kexec_jump:
    cli

    /* Save entry and boot_params before CR3 switch */
    mov %rdi, %rbx         /* entry -> rbx */
    mov %rsi, %rbp         /* boot_params -> rbp */

    /* Switch to identity-mapped page tables */
    mov %rdx, %cr3

    /* Linux 64-bit entry: RSI = boot_params physical address */
    mov %rbp, %rsi

    /* Clear all other registers */
    xor %eax, %eax
    xor %ecx, %ecx
    xor %edx, %edx
    xor %edi, %edi
    xor %r8d, %r8d
    xor %r9d, %r9d
    xor %r10d, %r10d
    xor %r11d, %r11d
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d
    xor %ebp, %ebp

    /* Jump to Linux entry point */
    jmp *%rbx

.section .note.GNU-stack,"",@progbits
