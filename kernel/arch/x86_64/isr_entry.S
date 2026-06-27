// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 Gabi Falk
    .text
    .globl isr_de
    .globl isr_db
    .globl isr_ud
    .globl isr_bp
    .globl isr_np
    .globl isr_ss
    .globl isr_gp
    .globl isr_pf
    .globl isr_timer
    .globl isr_tlb_shootdown
    .globl isr_df

/* No error code: push dummy 0, then vector */
isr_de:
    pushq $0
    pushq $0
    jmp isr_common

isr_db:
    pushq $0
    pushq $1
    jmp isr_common

isr_bp:
    pushq $0
    pushq $3
    jmp isr_common

isr_ud:
    pushq $0
    pushq $6
    jmp isr_common

/* CPU pushes error code; we push vector */
isr_np:
    pushq $11
    jmp isr_common

isr_ss:
    pushq $12
    jmp isr_common

isr_gp:
    pushq $13
    jmp isr_common

isr_pf:
    pushq $14
    jmp isr_common

isr_common:
    /* If from user mode, swap GS so kernel gs: accesses work
       (matches isr_timer's swapgs on user-mode entry).
       CS is at rsp+24: [vector][error_code][rip][cs]... */
    testq $3, 24(%rsp)
    jz .Lisr_no_swapgs_entry
    .byte 0x0f, 0x01, 0xf8  /* swapgs */
.Lisr_no_swapgs_entry:

    /* Save 15 GPRs */
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    /* Acquire BKL (no-op if already held, e.g. kernel-mode exception) */
    call bkl_acquire_entry

    /* Pass frame pointer to C handler */
    mov %rsp, %rdi
    call isr_handler

    /* Check return: 0 = resume, nonzero = killed */
    test %eax, %eax
    jnz .Lkilled

    /* Release BKL if returning to user mode */
    testq $3, 0x90(%rsp)   /* CS is at rsp + 15*8(GPRs) + 8(vec) + 8(err) + 8(rip) */
    jz .Lisr_no_bkl_release
    call bkl_release_entry
.Lisr_no_bkl_release:

    /* Resume: restore regs and iretq */
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    /* If returning to user mode, swap GS back.
     * cli first to prevent nested interrupt between swapgs and iretq.
     * Verified: models/nested_isr_swapgs.pml */
    testq $3, 24(%rsp)   /* [vector][error_code][rip][cs]... */
    jz .Lisr_no_swapgs_exit
    /* User-mode return: swapgs + RIP assertion */
    cli
    .byte 0x0f, 0x01, 0xf8  /* swapgs */
    add $16, %rsp       /* skip vector and error_code */
    /* ── ASSERTION: iretq RIP must be valid user address ── */
    push %rax
    mov 8(%rsp), %rax   /* rax = iretq RIP */
    test %rax, %rax
    jz .Lbad_isr_rip
    test %rax, %rax
    js .Lbad_isr_rip
    pop %rax
    .byte 0x48, 0xCF    /* iretq */
.Lisr_no_swapgs_exit:
    /* Kernel-mode return: no swapgs, no RIP check */
    add $16, %rsp       /* skip vector and error_code */
    .byte 0x48, 0xCF    /* iretq */

.Lkilled:
    /* Release BKL if from user mode */
    testq $3, 0x90(%rsp)
    jz .Lkilled_no_bkl_release
    call bkl_release_entry
.Lkilled_no_bkl_release:
    /* Load per-CPU resume_rsp BEFORE swapgs (gs: still points to kernel cpu_local) */
    mov %gs:32, %rax
    /* Undo entry swapgs if from user mode */
    testq $3, 0x90(%rsp)
    jz .Lkilled_no_swapgs
    cli
    .byte 0x0f, 0x01, 0xf8  /* swapgs */
.Lkilled_no_swapgs:
    /* Return to kernel (enter_user caller) using per-CPU resume_rsp */
    mov %rax, %rsp
    ret

/*
 * Timer ISR (vector 32, IRQ0).
 * If from user mode: swapgs, save all GPRs, call timer_handler,
 * restore GPRs, swapgs, iretq.  timer_handler may context_switch,
 * in which case the restore/iretq happen on the NEW process's stack.
 * If from kernel mode: just send EOI and return.
 */
isr_timer:
    testq $3, 8(%rsp)       /* check RPL of saved CS */
    jz .Ltimer_kernel

    /* ---- user-mode path ---- */
    .byte 0x0f, 0x01, 0xf8 /* swapgs */

    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    /* Acquire BKL before entering kernel from user timer */
    call bkl_acquire_entry

    call timer_handler

    /* Deliver pending signals before returning to user mode.
     * Pass RSP (ISR frame pointer) so the handler can modify
     * the return RIP/RSP to redirect to the signal handler.
     * Verified: models/unified_smp.pml P10. */
    mov %rsp, %rdi
    call timer_deliver_signals

    /* Release BKL before returning to user */
    call bkl_release_entry

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    /* ── ASSERTION: iretq RIP must be valid user address ──
     * RSP points to iretq frame: [RIP][CS][RFLAGS][RSP][SS].
     * Check RIP != 0 and RIP not in kernel half.
     * Catches the RIP=0x246 (RFLAGS) corruption bug.
     * Verified: models/syscall_timer_rip.pml (v2, 3.76M states) */
    push %rax
    mov 8(%rsp), %rax       /* rax = iretq RIP */
    test %rax, %rax
    jz .Lbad_timer_rip
    test %rax, %rax
    js .Lbad_timer_rip       /* bit 63 set = kernel address */
    pop %rax

    /* Disable interrupts before swapgs to prevent nested interrupt
     * between swapgs (kernel→user GS) and iretq.  A nested timer
     * would see user CS on stack, do swapgs (user→kernel), schedule
     * a different process (setting gs:0 to wrong kstack), then return.
     * Outer iretq returns to the original process with wrong gs:0.
     * Verified: models/nested_isr_swapgs.pml */
    cli
    .byte 0x0f, 0x01, 0xf8 /* swapgs */
    .byte 0x48, 0xCF        /* iretq */

.Ltimer_kernel:
    /* kernel-mode: EOI and return */
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    call timer_eoi_kernel
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax
    .byte 0x48, 0xCF        /* iretq */

/*
 * Schedule kick IPI (vector 0x40).
 * Wakes an idle CPU from hlt so it can pick up a READY process.
 * Sent by schedule() after making a process READY.
 * Minimal handler: just LAPIC EOI and return.
 */
    .globl isr_sched_kick
isr_sched_kick:
    push %rax
    /*
     * LAPIC EOI via lapic_base global (already HHDM-adjusted).
     * Previous code read LAPIC base from MSR 0x1B and added HHDM
     * offset using a 10-byte movq imm64 instruction.  Crash data
     * (cr2=0xFEE000B0, rax=0xFEE00000, rdx=0) showed the HHDM base
     * was not added.  Using the global avoids the imm64 encoding
     * and is simpler (2 instructions instead of 8).
     * Verified: models/sched_kick_eoi.pml
     */
    mov lapic_base(%rip), %rax
    movl $0, 0xB0(%rax)
    pop %rax
    .byte 0x48, 0xCF        /* iretq */

/*
 * TLB shootdown IPI (vector 0x41).
 * Sent by smp_flush_tlb() when munmap/mprotect modifies PTEs for a
 * shared address space (CLONE_VM threads).  Reloads CR3 to flush TLB,
 * increments tlb_ack_count, then EOI + iretq.
 * Verified: models/unified_smp.pml (P9, BUGGY_THREAD_TLB).
 */
    .globl isr_tlb_shootdown
isr_tlb_shootdown:
    push %rax
    /* Flush TLB: mov cr3, cr3 */
    mov %cr3, %rax
    mov %rax, %cr3
    /* Increment ack counter: lock incl tlb_ack_count */
    lock incl tlb_ack_count(%rip)
    /* LAPIC EOI via lapic_base global — same as isr_sched_kick. */
    mov lapic_base(%rip), %rax
    movl $0, 0xB0(%rax)
    pop %rax
    .byte 0x48, 0xCF        /* iretq */

/*
 * Generic device-IRQ entry (vectors VEC_IRQ_BASE..VEC_IRQ_MAX).
 * Each stub pushes its vector number then jmps to isr_irq_common.
 * Unlike the IPI stubs, device handlers touch shared kernel state, so we
 * take the BKL and follow the same swapgs discipline as isr_common.
 * EOI is issued by irq_dispatch() (C), not here.
 *
 * BKL discipline: bkl_acquire_entry returns 1 iff it acquired the lock.
 * isr_irq_common stashes that flag in r15 and releases iff r15 != 0,
 * regardless of user/kernel mode.  Nested kernel entries (already held)
 * and single-CPU runs return 0 and skip the release - balanced.
 * swapgs entry/exit remain CS-gated as before.  The kernel-mode SMP
 * acquire-and-leak hazard is now closed.
 */
    .globl isr_irq_0
    .globl isr_irq_1
    .globl isr_irq_2
    .globl isr_irq_3
    .globl isr_irq_4
    .globl isr_irq_5
    .globl isr_irq_6
    .globl isr_irq_7
    .globl isr_irq_8
    .globl isr_irq_9
    .globl isr_irq_10
    .globl isr_irq_11
    .globl isr_irq_12
    .globl isr_irq_13

isr_irq_0:  pushq $0x42 ; jmp isr_irq_common
isr_irq_1:  pushq $0x43 ; jmp isr_irq_common
isr_irq_2:  pushq $0x44 ; jmp isr_irq_common
isr_irq_3:  pushq $0x45 ; jmp isr_irq_common
isr_irq_4:  pushq $0x46 ; jmp isr_irq_common
isr_irq_5:  pushq $0x47 ; jmp isr_irq_common
isr_irq_6:  pushq $0x48 ; jmp isr_irq_common
isr_irq_7:  pushq $0x49 ; jmp isr_irq_common
isr_irq_8:  pushq $0x4a ; jmp isr_irq_common
isr_irq_9:  pushq $0x4b ; jmp isr_irq_common
isr_irq_10: pushq $0x4c ; jmp isr_irq_common
isr_irq_11: pushq $0x4d ; jmp isr_irq_common
isr_irq_12: pushq $0x4e ; jmp isr_irq_common
isr_irq_13: pushq $0x4f ; jmp isr_irq_common

isr_irq_common:
    /* Stack on entry: [vector][rip][cs][rflags][rsp][ss].
     * CS is at rsp+16 right now (before saving GPRs). */
    testq $3, 16(%rsp)
    jz .Lirq_no_swapgs_entry
    .byte 0x0f, 0x01, 0xf8  /* swapgs */
.Lirq_no_swapgs_entry:

    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    /* Acquire BKL; eax = 1 iff acquired here. Keep it in r15 across the C call
     * (callee-saved, original r15 already saved on the stack). */
    call bkl_acquire_entry
    mov %eax, %r15d

    /* irq_dispatch(vector): vector at rsp + 15*8 = rsp + 0x78. */
    mov 0x78(%rsp), %rdi
    call irq_dispatch

    /* Release the BKL iff we acquired it (any mode), not based on CS. */
    test %r15, %r15
    jz .Lirq_no_bkl_release
    call bkl_release_entry
.Lirq_no_bkl_release:

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    /* Stack now: [vector][rip][cs]... CS at rsp+16. */
    testq $3, 16(%rsp)
    jz .Lirq_kernel_ret
    cli
    .byte 0x0f, 0x01, 0xf8  /* swapgs */
    add $8, %rsp            /* pop vector */
    .byte 0x48, 0xCF        /* iretq */
.Lirq_kernel_ret:
    add $8, %rsp            /* pop vector */
    .byte 0x48, 0xCF        /* iretq */

/*
 * Double fault handler (vector 8).
 * Prints "DOUBLE FAULT" to serial and halts.  Without this, a double
 * fault causes an immediate triple fault (silent VM exit).
 */
    .data
    .globl df_lock
df_lock:
    .long 0
    .text

isr_df:
    cli
    /* Serialize: only one CPU prints DF output.  Others halt immediately. */
    mov $1, %eax
    xchg %eax, df_lock
    test %eax, %eax
    jnz .Ldf_halt              /* another CPU already printing */

    /* Print "DF@" + faulting RIP (hex) via direct serial.
     * Stack frame: [error_code(0)][RIP][CS][RFLAGS][RSP][SS]
     * RIP is at 8(%rsp). */

    /* Save rip from frame into r15 (callee-saved, safe to clobber in halt path) */
    mov 8(%rsp), %r15
    /* Also grab RSP from frame */
    mov 32(%rsp), %r14

    /* Print "DF@" */
    mov $0x3F8, %dx
.Ldf_w1:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_w1
    sub $5, %dx
    mov $'D', %al
    out %al, %dx
.Ldf_w2:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_w2
    sub $5, %dx
    mov $'F', %al
    out %al, %dx
.Ldf_w3:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_w3
    sub $5, %dx
    mov $'@', %al
    out %al, %dx

    /* Print RIP as 16 hex digits */
    mov $60, %cl     /* bit position: 60, 56, 52, ... 0 */
.Ldf_hex_loop:
    mov %r15, %rax
    shr %cl, %rax
    and $0xf, %al
    cmp $10, %al
    jb .Ldf_digit
    add $('a' - '0' - 10), %al
.Ldf_digit:
    add $'0', %al
    mov $0x3F8, %dx
.Ldf_w4:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_w4
    sub $5, %dx
    /* reload the nibble since we clobbered al */
    mov %r15, %rax
    shr %cl, %rax
    and $0xf, %al
    cmp $10, %al
    jb .Ldf_digit2
    add $('a' - '0' - 10), %al
.Ldf_digit2:
    add $'0', %al
    out %al, %dx
    sub $4, %cl
    jns .Ldf_hex_loop

    /* Print " RSP=" */
    mov $0x3F8, %dx
.Ldf_w5:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_w5
    sub $5, %dx
    mov $' ', %al
    out %al, %dx

    /* Print " CR2=" and CR2 value (fault address) */
    mov $0x3F8, %dx
.Ldf_cr2_sp:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_cr2_sp
    sub $5, %dx
    mov $'C', %al
    out %al, %dx
.Ldf_cr2_R:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_cr2_R
    sub $5, %dx
    mov $'R', %al
    out %al, %dx
.Ldf_cr2_2:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_cr2_2
    sub $5, %dx
    mov $'2', %al
    out %al, %dx
.Ldf_cr2_eq:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_cr2_eq
    sub $5, %dx
    mov $'=', %al
    out %al, %dx
    /* Print CR2 as 16 hex digits */
    mov %cr2, %r15
    mov $60, %cl
.Ldf_cr2_hex:
    mov %r15, %rax
    shr %cl, %rax
    and $0xf, %al
    cmp $10, %al
    jb .Ldf_cr2_d
    add $('a' - '0' - 10), %al
.Ldf_cr2_d:
    add $'0', %al
    mov $0x3F8, %dx
.Ldf_cr2_w:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_cr2_w
    sub $5, %dx
    mov %r15, %rax
    shr %cl, %rax
    and $0xf, %al
    cmp $10, %al
    jb .Ldf_cr2_d2
    add $('a' - '0' - 10), %al
.Ldf_cr2_d2:
    add $'0', %al
    out %al, %dx
    sub $4, %cl
    jns .Ldf_cr2_hex

    /* Print newline */
.Ldf_w6:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Ldf_w6
    sub $5, %dx
    mov $'\n', %al
    out %al, %dx

.Ldf_halt:
    hlt
    jmp .Ldf_halt

/*
 * Bad RIP handlers: timer and ISR iretq detected corrupt RIP.
 * Stack at entry: [saved_rax][RIP][CS][RFLAGS][RSP][SS]
 * Prints "!TM RIP=<hex16>\n" or "!EX RIP=<hex16>\n" to serial, then halts.
 */
.Lbad_timer_rip:
    mov %rax, %r15          /* r15 = corrupt RIP (already in rax) */
    pop %rax                /* discard saved rax -- we're halting */
    /* r15 = bad RIP. Also grab RFLAGS and CS from the frame. */
    mov 16(%rsp), %r14      /* r14 = RFLAGS */
    mov 8(%rsp), %r13       /* r13 = CS */
    mov 24(%rsp), %r12      /* r12 = user RSP */

    /* Print "!TM " */
    mov $0x3F8, %dx
    mov $'!', %al
    call .Lserial_putc
    mov $'T', %al
    call .Lserial_putc
    mov $'M', %al
    call .Lserial_putc
    mov $' ', %al
    call .Lserial_putc
    jmp .Lbad_rip_common

.Lbad_isr_rip:
    mov %rax, %r15
    pop %rax
    mov 16(%rsp), %r14
    mov 8(%rsp), %r13
    mov 24(%rsp), %r12

    /* Print "!EX " */
    mov $0x3F8, %dx
    mov $'!', %al
    call .Lserial_putc
    mov $'E', %al
    call .Lserial_putc
    mov $'X', %al
    call .Lserial_putc
    mov $' ', %al
    call .Lserial_putc
    jmp .Lbad_rip_common

.Lbad_rip_common:
    /* Print "RIP=" */
    mov $'R', %al
    call .Lserial_putc
    mov $'I', %al
    call .Lserial_putc
    mov $'P', %al
    call .Lserial_putc
    mov $'=', %al
    call .Lserial_putc
    /* Print r15 (bad RIP) as 16 hex digits */
    mov %r15, %rbx
    call .Lprint_hex64

    /* Print " FL=" */
    mov $' ', %al
    call .Lserial_putc
    mov $'F', %al
    call .Lserial_putc
    mov $'L', %al
    call .Lserial_putc
    mov $'=', %al
    call .Lserial_putc
    mov %r14, %rbx
    call .Lprint_hex64

    /* Print " CS=" */
    mov $' ', %al
    call .Lserial_putc
    mov $'C', %al
    call .Lserial_putc
    mov $'S', %al
    call .Lserial_putc
    mov $'=', %al
    call .Lserial_putc
    mov %r13, %rbx
    call .Lprint_hex64

    /* Print " RSP=" */
    mov $' ', %al
    call .Lserial_putc
    mov $'R', %al
    call .Lserial_putc
    mov $'S', %al
    call .Lserial_putc
    mov $'P', %al
    call .Lserial_putc
    mov $'=', %al
    call .Lserial_putc
    mov %r12, %rbx
    call .Lprint_hex64

    /* Print newline */
    mov $'\n', %al
    call .Lserial_putc

.Lbad_rip_halt:
    hlt
    jmp .Lbad_rip_halt

/* Serial putc: write AL to COM1 (0x3F8), wait for TX ready.
 * Clobbers: dx (set to 0x3F8 on return). */
.Lserial_putc:
    push %rax
    mov $0x3F8, %dx
.Lspc_wait:
    add $5, %dx
    in %dx, %al
    test $0x20, %al
    jz .Lspc_wait
    sub $5, %dx
    pop %rax
    out %al, %dx
    ret

/* Print rbx as 16 hex digits to serial.
 * Clobbers: rax, rcx, rdx. */
.Lprint_hex64:
    mov $60, %cl
.Lph_loop:
    mov %rbx, %rax
    shr %cl, %rax
    and $0xf, %al
    cmp $10, %al
    jb .Lph_digit
    add $('a' - '0' - 10), %al
.Lph_digit:
    add $'0', %al
    call .Lserial_putc
    sub $4, %cl
    jns .Lph_loop
    ret

    .data
    .globl kernel_resume_rsp
kernel_resume_rsp:
    .quad 0

.section .note.GNU-stack,"",@progbits
