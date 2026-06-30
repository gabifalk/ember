// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text
    .globl syscall_entry

/* SYSCALL entry: RCX=RIP, R11=RFLAGS, RSP=user */
syscall_entry:
    .byte 0x0f, 0x01, 0xf8 /* swapgs */
    mov %rsp, %gs:8       /* save user rsp to cpu_local.user_rsp */
    mov %gs:0, %rsp       /* switch to kernel stack */

    /* build an explicit syscall_frame_t on the stack (19 fields * 8 bytes) */
    sub $152, %rsp

    /* r15..rax (all original values preserved) */
    mov %r15, 0(%rsp)
    mov %r14, 8(%rsp)
    mov %r13, 16(%rsp)
    mov %r12, 24(%rsp)
    mov %r11, 32(%rsp)
    mov %r10, 40(%rsp)
    mov %r9, 48(%rsp)
    mov %r8, 56(%rsp)
    mov %rsi, 64(%rsp)
    mov %rdi, 72(%rsp)
    mov %rbp, 80(%rsp)
    mov %rdx, 88(%rsp)
    mov %rcx, 96(%rsp)
    mov %rbx, 104(%rsp)
    mov %rax, 112(%rsp)

    /* user rip (from RCX) and rflags (from R11) */
    mov %rcx, 120(%rsp)
    mov %r11, 128(%rsp)

    /* user rsp from gs:8 (use rcx as temp, already saved) */
    mov %gs:8, %rcx
    mov %rcx, 136(%rsp)

    mov %rsp, %rdi

    /* Acquire BKL before entering kernel */
    push %rdi
    call bkl_acquire_entry
    pop %rdi

    sti
    call syscall_dispatch
    cli

    .globl syscall_return
syscall_return:
    /* Release BKL before returning to user */
    call bkl_release_entry

    /* Check if rt_sigreturn requested iretq return (gs:56). */
    cmpq $0, %gs:56
    jne .Lsyscall_iretq

    /* restore registers */
    mov 0(%rsp), %r15
    mov 8(%rsp), %r14
    mov 16(%rsp), %r13
    mov 24(%rsp), %r12
    mov 32(%rsp), %r11
    mov 40(%rsp), %r10
    mov 48(%rsp), %r9
    mov 56(%rsp), %r8
    mov 64(%rsp), %rsi
    mov 72(%rsp), %rdi
    mov 80(%rsp), %rbp
    mov 88(%rsp), %rdx
    mov 96(%rsp), %rcx
    mov 104(%rsp), %rbx
    mov 112(%rsp), %rax

    /* restore user state */
    mov 120(%rsp), %rcx
    mov 128(%rsp), %r11

    /* ── ASSERTION: sysretq RIP (RCX) must be valid user address ──
     * Catches corruption of the syscall frame's rip field.
     * cmp/test only modify RFLAGS which sysretq overwrites from R11. */
    test %rcx, %rcx
    jz .Lbad_sysret_rip
    test %rcx, %rcx
    js .Lbad_sysret_rip

    mov 136(%rsp), %rsp

    .byte 0x0f, 0x01, 0xf8 /* swapgs */
    .byte 0x48, 0x0f, 0x07 /* sysretq */

/*
 * iretq return path for rt_sigreturn.
 * Restores ALL registers including R11/RCX from the frame,
 * then returns via iretq.  This preserves R11/RCX which sysretq
 * would clobber.
 *
 * Strategy: build the iretq frame [RIP][CS][RFLAGS][RSP][SS] in
 * place OVER the syscall_frame_t (which we no longer need after
 * extracting all values), then restore GPRs from saved copies
 * below the frame.
 */
.Lsyscall_iretq:
    movq $0, %gs:56         /* clear flag */

    /*
     * Step 1: Move RSP down to make room for saved GPRs (15*8=120)
     * below the current frame.  We'll stash GPR values there,
     * then overwrite the frame area with the iretq frame.
     */
    sub $120, %rsp

    /* Step 2: Copy all GPRs from the frame (now at 120(%rsp)) to
     * the save area at 0(%rsp). Also extract rip/rflags/rsp. */
    /* Save rip/rflags/user_rsp into r14/r15/r13 (clobbered below) */
    mov 120+120(%rsp), %r14   /* rip */
    mov 120+128(%rsp), %r15   /* rflags */
    mov 120+136(%rsp), %r13   /* user rsp */

    /* Copy frame GPRs to save area */
    mov 120+0(%rsp), %rax     /* frame r15 */
    mov %rax, 0(%rsp)
    mov 120+8(%rsp), %rax     /* frame r14 */
    mov %rax, 8(%rsp)
    mov 120+16(%rsp), %rax    /* frame r13 */
    mov %rax, 16(%rsp)
    mov 120+24(%rsp), %rax    /* frame r12 */
    mov %rax, 24(%rsp)
    mov 120+32(%rsp), %rax    /* frame r11 (real user R11) */
    mov %rax, 32(%rsp)
    mov 120+40(%rsp), %rax    /* frame r10 */
    mov %rax, 40(%rsp)
    mov 120+48(%rsp), %rax    /* frame r9 */
    mov %rax, 48(%rsp)
    mov 120+56(%rsp), %rax    /* frame r8 */
    mov %rax, 56(%rsp)
    mov 120+64(%rsp), %rax    /* frame rsi */
    mov %rax, 64(%rsp)
    mov 120+72(%rsp), %rax    /* frame rdi */
    mov %rax, 72(%rsp)
    mov 120+80(%rsp), %rax    /* frame rbp */
    mov %rax, 80(%rsp)
    mov 120+88(%rsp), %rax    /* frame rdx */
    mov %rax, 88(%rsp)
    mov 120+96(%rsp), %rax    /* frame rcx (real user RCX) */
    mov %rax, 96(%rsp)
    mov 120+104(%rsp), %rax   /* frame rbx */
    mov %rax, 104(%rsp)
    mov 120+112(%rsp), %rax   /* frame rax */
    mov %rax, 112(%rsp)

    /* Step 3: Build iretq frame in place (overwrites old frame area).
     * iretq frame at 120(%rsp): [RIP][CS][RFLAGS][RSP][SS] */
    mov %r14, 120(%rsp)       /* RIP */
    movq $0x23, 128(%rsp)     /* CS (user) */
    mov %r15, 136(%rsp)       /* RFLAGS */
    mov %r13, 144(%rsp)       /* RSP (user) */
    movq $0x1b, 152(%rsp)     /* SS (user) */

    /* Step 4: Restore all GPRs from save area */
    mov 0(%rsp), %r15
    mov 8(%rsp), %r14
    mov 16(%rsp), %r13
    mov 24(%rsp), %r12
    mov 32(%rsp), %r11
    mov 40(%rsp), %r10
    mov 48(%rsp), %r9
    mov 56(%rsp), %r8
    mov 64(%rsp), %rsi
    mov 72(%rsp), %rdi
    mov 80(%rsp), %rbp
    mov 88(%rsp), %rdx
    mov 96(%rsp), %rcx
    mov 104(%rsp), %rbx
    mov 112(%rsp), %rax

    /* Step 5: Point RSP at the iretq frame */
    lea 120(%rsp), %rsp

    cli
    .byte 0x0f, 0x01, 0xf8   /* swapgs */
    .byte 0x48, 0xCF          /* iretq */

.Lbad_sysret_rip:
    /* RCX has corrupt RIP. Print "!SR RIP=<hex>\n" to serial and halt. */
    mov %rcx, %r15          /* save corrupt RIP */
    mov %r11, %r14           /* save RFLAGS */
    mov 136(%rsp), %r12      /* save user RSP */
    mov $0x3F8, %dx
    /* "!SR " */
    mov $'!', %al
    call .Lsr_putc
    mov $'S', %al
    call .Lsr_putc
    mov $'R', %al
    call .Lsr_putc
    mov $' ', %al
    call .Lsr_putc
    /* "RIP=" */
    mov $'R', %al
    call .Lsr_putc
    mov $'I', %al
    call .Lsr_putc
    mov $'P', %al
    call .Lsr_putc
    mov $'=', %al
    call .Lsr_putc
    mov %r15, %rbx
    call .Lsr_hex64
    /* " FL=" */
    mov $' ', %al
    call .Lsr_putc
    mov $'F', %al
    call .Lsr_putc
    mov $'L', %al
    call .Lsr_putc
    mov $'=', %al
    call .Lsr_putc
    mov %r14, %rbx
    call .Lsr_hex64
    /* " RSP=" */
    mov $' ', %al
    call .Lsr_putc
    mov $'R', %al
    call .Lsr_putc
    mov $'S', %al
    call .Lsr_putc
    mov $'P', %al
    call .Lsr_putc
    mov $'=', %al
    call .Lsr_putc
    mov %r12, %rbx
    call .Lsr_hex64
    mov $'\n', %al
    call .Lsr_putc
.Lsr_halt:
    hlt
    jmp .Lsr_halt

.Lsr_putc:
    push %rax
    mov $0x3F8, %dx
.Lsr_wait:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Lsr_wait
    sub $5, %dx
    pop %rax
    out %al, %dx
    ret

.Lsr_hex64:
    mov $60, %cl
.Lsr_hl:
    mov %rbx, %rax
    shr %cl, %rax
    and $0xf, %al
    cmp $10, %al
    jb .Lsr_d
    add $('a' - '0' - 10), %al
.Lsr_d:
    add $'0', %al
    call .Lsr_putc
    sub $4, %cl
    jns .Lsr_hl
    ret

.section .note.GNU-stack,"",@progbits
