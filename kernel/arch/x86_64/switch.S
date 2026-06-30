// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text

/*
 * context_switch(uint64_t *old_ksp, uint64_t new_ksp)
 *
 * Save callee-saved registers and RSP to *old_ksp,
 * then load new_ksp into RSP and restore callee-saved registers.
 * The return address on the stack determines where execution resumes.
 */
    .globl context_switch
context_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, (%rdi)    /* *old_ksp = rsp */
    mov %rsi, %rsp      /* rsp = new_ksp */

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

/*
 * fork_child_return -- first "return" address for a newly forked child.
 * At this point RSP points at the child's syscall_frame_t (rax=0).
 * Restore regs and sysretq back to user mode.
 */
    .globl fork_child_return
fork_child_return:
    jmp syscall_return

.section .note.GNU-stack,"",@progbits
