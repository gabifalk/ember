// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text
    .globl enter_user

/* enter_user(rip, rsp) */
enter_user:
    /* Save kernel stack to per-CPU cpu_local.resume_rsp (gs:32)
     * so ISR .Lkilled path can recover on user fault.
     * Must be done BEFORE we overwrite GS with user segment. */
    mov %rsp, %gs:32

    mov %rdi, %rax /* rip */
    mov %rsi, %rbx /* rsp */

    /* set user data segments.
     * Do NOT load GS — GS base is managed by MSRs (IA32_GS_BASE /
     * IA32_KERNEL_GS_BASE) and swapgs.  Loading a segment selector
     * into GS via mov zeroes IA32_GS_BASE on AMD64, corrupting the
     * kernel's per-CPU cpu_local pointer. */
    mov $0x1b, %cx
    mov %cx, %ds
    mov %cx, %es
    mov %cx, %fs
    /* %gs: managed by swapgs, do not touch */

    pushq $0x1b        /* SS */
    pushq %rbx        /* RSP */
    pushq $0x202      /* RFLAGS (IF=1) */
    pushq $0x23        /* CS */
    pushq %rax        /* RIP */

    /* Swap kernel GS → user GS before entering user mode.
     * cli prevents nested interrupt between swapgs and iretq.
     * Verified: models/gs_segment_load.pml */
    cli
    .byte 0x0f, 0x01, 0xf8 /* swapgs */
    .byte 0x48, 0xCF   /* iretq */

.section .note.GNU-stack,"",@progbits
