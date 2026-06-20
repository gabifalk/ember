/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/proc.h"
#include "ember/sched.h"
#include "ember/console.h"
#include "ember/time.h"
#include "ember/cpu.h"
#include "ember/lapic.h"
#include "ember/bug.h"
#include "ember/signal.h"
#include "ember/syscall.h"
#include "ember/heap.h"
#include "ember/bkl.h"
#include "ember/vectors.h"
#include "ember/paging.h"
#include "ember/acpi.h"

volatile uint64_t kernel_ticks = 0;

/* Syscall tracer (defined in console.c, externs in syscall.c) */
extern volatile int syscall_trace_interval;
extern volatile uint64_t g_last_syscall_nr;
extern volatile uint64_t g_last_syscall_ret;

/* VFS stats (defined in vfs.c) */
extern uint32_t vfs_node_count;
extern uint32_t vfs_nodes_ever;
extern uint32_t vfs_evictions;

/* PMM stats. */
extern uint64_t pmm_get_free_pages(void);

static void trace_print_dec(uint64_t n) {
    char buf[20];
    int pos = 0;
    if (n == 0) { console_write("0"); return; }
    while (n > 0) { buf[pos++] = '0' + (char)(n % 10); n /= 10; }
    while (pos > 0) { char c = buf[--pos]; console_putc(c); }
}

static void trace_print_sdec(int64_t n) {
    if (n < 0) { console_putc('-'); n = -n; }
    trace_print_dec((uint64_t)n);
}

/* Outb provided by ember/io.h (included via ember/bug.h) */

/* PIC ports. */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

/* PIT ports. */
#define PIT_CH0    0x40
#define PIT_CMD    0x43

/* ~100 Hz: 1193182 / 100 = 11932. */
#define PIT_DIVISOR 11932

void pic_init(void) {
    /* ICW1: begin init, expect ICW4. */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);

    /* ICW2: vector offsets. */
    outb(PIC1_DATA, IRQ_VECTOR_BASE);   /* IRQ 0-7  -> vectors 32-39. */
    outb(PIC2_DATA, IRQ_VECTOR_BASE + 8);   /* IRQ 8-15 -> vectors 40-47. */

    /* ICW3: wiring. */
    outb(PIC1_DATA, 4);    /* Slave on IRQ2. */
    outb(PIC2_DATA, 2);    /* Slave identity. */

    /* ICW4: 8086 mode. */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* Mask all IRQs except IRQ0 (timer) on master. */
    outb(PIC1_DATA, 0xFE);
    /* Mask all slave IRQs. */
    outb(PIC2_DATA, 0xFF);
}

void timer_init(void) {
    /* Channel 0, lobyte/hibyte, mode 2 (rate generator) */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0, (uint8_t)(PIT_DIVISOR & 0xFF));
    outb(PIT_CH0, (uint8_t)((PIT_DIVISOR >> 8) & 0xFF));
}

static volatile uint64_t trace_next_tick;

static const char *state_name(int st) {
    switch (st) {
    case PROC_UNUSED:  return "FREE";
    case PROC_READY:   return "REDY";
    case PROC_RUNNING: return "RUN ";
    case PROC_SLEEPING:return "SLP ";
    case PROC_ZOMBIE:  return "ZOMB";
    case PROC_STOPPED: return "STOP";
    default:           return "??? ";
    }
}

void trace_dump(void) {
    if (syscall_trace_interval && kernel_ticks >= trace_next_tick) {
        trace_next_tick = kernel_ticks + (uint64_t)syscall_trace_interval;
        proc_t *p = current_proc;
        console_write("[T] pid=");
        trace_print_dec(p ? (uint64_t)p->pid : 0);
        console_write(" sc=");
        trace_print_dec(g_last_syscall_nr);
        console_write(" ret=");
        trace_print_sdec((int64_t)g_last_syscall_ret);
        if (p && p->exe_path[0]) {
            console_write(" exe=");
            console_write(p->exe_path);
        }
        console_write(" free=");
        trace_print_dec(pmm_get_free_pages());
        console_write("pg\n");
        /* Dump all live processes. */
        for (int _ti = 0; _ti < MAX_PROCS; _ti++) {
            if (procs[_ti].state == PROC_UNUSED) continue;
            console_write("  [");
            trace_print_dec((uint64_t)procs[_ti].pid);
            console_write("] ");
            console_write(state_name(procs[_ti].state));
            console_write(" ppid=");
            trace_print_dec((uint64_t)procs[_ti].ppid);
            console_write(" sc=");
            trace_print_dec(procs[_ti].last_sc);
            console_write(" ret=");
            trace_print_sdec(procs[_ti].last_ret);
            console_write(" sig=");
            console_hex64((uint64_t)procs[_ti].sig_pending);
            if (procs[_ti].exe_path[0]) {
                console_write(" ");
                console_write(procs[_ti].exe_path);
            }
            console_write("\n");
        }
    }
}

/*
 * Called from isr_timer for kernel-mode timer ticks (EOI only, no scheduling).
 *
 * Under SMP, this runs WITHOUT BKL (e.g. during schedule() idle hlt or
 * AP idle hlt). Verified in models/bkl_sched_fixed.pml: MUST NOT access
 * shared state -- only LAPIC EOI and tick increment are safe.
 */
void timer_eoi_kernel(void) {
    /*
     * Model invariant (TIMER_EOI_SAFE): under SMP this may run WITHOUT BKL
     * (during schedule idle hlt or AP idle hlt). Must NOT access shared state.
     * The cpu_count<=1 gate below enforces this.
     */

    /*
     * Check FULL LAPIC page table chain integrity BEFORE touching LAPIC.
     * Walk: CR3 -> PML4[256] -> PDPT[3] -> PD[503] -> PT[0].
     * Any broken link -> triple fault on LAPIC access.
     */
    {
        extern uint64_t lapic_guard_pt_pa;
        extern uint64_t lapic_guard_pd_pa;
        if (lapic_guard_pd_pa) {
            uint64_t cr3;
            __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
            uint64_t pml4_pa = cr3 & 0x000FFFFFFFFFF000ULL;
            volatile uint64_t *pml4 = (volatile uint64_t *)phys_to_virt(pml4_pa);
            uint64_t pml4e = pml4[256];
            if (!(pml4e & 1)) {
                bug_serial_str("\n!!! LAPIC: PML4[256] NOT PRESENT !!!\n  cr3=");
                bug_serial_hex(cr3);
                bug_serial_str(" pml4e="); bug_serial_hex(pml4e);
                bug_serial_str(" cpu="); bug_serial_hex((uint64_t)this_cpu_id());
                bug_serial_str("\n"); __asm__ __volatile__("cli; hlt");
            }
            volatile uint64_t *pdpt = (volatile uint64_t *)phys_to_virt(pml4e & 0x000FFFFFFFFFF000ULL);
            uint64_t pdpte = pdpt[3];
            if (!(pdpte & 1)) {
                bug_serial_str("\n!!! LAPIC: PDPT[3] NOT PRESENT !!!\n  cr3=");
                bug_serial_hex(cr3);
                bug_serial_str(" pdpte="); bug_serial_hex(pdpte);
                bug_serial_str(" cpu="); bug_serial_hex((uint64_t)this_cpu_id());
                bug_serial_str("\n"); __asm__ __volatile__("cli; hlt");
            }
            volatile uint64_t *pd = (volatile uint64_t *)phys_to_virt(pdpte & 0x000FFFFFFFFFF000ULL);
            uint64_t pde = pd[503];
            if (!(pde & 1)) {
                bug_serial_str("\n!!! LAPIC: PD[503] NOT PRESENT !!!\n  cr3=");
                bug_serial_hex(cr3);
                bug_serial_str(" pde="); bug_serial_hex(pde);
                bug_serial_str(" cpu="); bug_serial_hex((uint64_t)this_cpu_id());
                bug_serial_str("\n"); __asm__ __volatile__("cli; hlt");
            }
            volatile uint64_t *pt = (volatile uint64_t *)phys_to_virt(pde & 0x000FFFFFFFFFF000ULL);
            uint64_t pte = pt[0];
            if (!(pte & 1)) {
                bug_serial_str("\n!!! LAPIC: PT[0] NOT PRESENT !!!\n  cr3=");
                bug_serial_hex(cr3);
                bug_serial_str(" pte="); bug_serial_hex(pte);
                bug_serial_str(" pt_pa="); bug_serial_hex(pde & 0x000FFFFFFFFFF000ULL);
                bug_serial_str(" cpu="); bug_serial_hex((uint64_t)this_cpu_id());
                bug_serial_str("\n"); __asm__ __volatile__("cli; hlt");
            }
        }
    }

    if (cpu_count > 1)
        lapic_eoi();
    else
        outb(PIC1_CMD, 0x20);
    kernel_ticks++;
    /*
     * Poll serial for Ctrl+T/C even in kernel-mode timer ticks.
     * On SMP this runs WITHOUT BKL, but serial I/O and sched_lock
     * are safe (sched_lock is irqsave, independent of BKL).
     * Without this, Ctrl+C/T are invisible when a process holds BKL.
     */
    console_poll_signals();
    trace_dump();
    if (cpu_count <= 1) {
        /*
         * Wake tick-sleeping processes (nanosleep, poll) on single CPU.
         * On SMP this is done in timer_handler (under BKL).
         */
        sched_wakeup(SCHED_TICK_CHAN);
    }
}

/*
 * Check iretq RIP at kstack_top-40 for the given process.
 * tag: "PRE" (before schedule) or "POST" (after schedule).
 * If corrupt, prints full diagnostics and halts.
 */
static void check_iretq_rip(proc_t *p, const char *tag) {
    uint64_t kstack_top = (uint64_t)(uintptr_t)(p->kstack + PROC_KSTACK_SIZE);
    uint64_t rip = *(volatile uint64_t *)(kstack_top - 40);
    uint64_t rfl = *(volatile uint64_t *)(kstack_top - 24);
    uint64_t cs  = *(volatile uint64_t *)(kstack_top - 32);
    if (rip == 0 || (rip & 0x8000000000000000ULL)) {
        console_write("\n!!! CORRUPT IRETQ RIP (");
        console_write(tag);
        console_write(") !!!\npid=");
        trace_print_dec((uint64_t)p->pid);
        console_write(" RIP=");
        console_hex64(rip);
        console_write(" RFLAGS=");
        console_hex64(rfl);
        console_write(" CS=");
        console_hex64(cs);
        console_write("\nkstack_top=");
        console_hex64(kstack_top);
        console_write(" &RIP=");
        console_hex64(kstack_top - 40);
        if (p->exe_path[0]) {
            console_write(" exe=");
            console_write(p->exe_path);
        }
        console_write("\nlast_sc=");
        trace_print_dec(g_last_syscall_nr);
        console_write(" ret=");
        trace_print_sdec((int64_t)g_last_syscall_ret);
        console_write("\n");
        serial_flush();
        for (;;) __asm__ __volatile__("hlt");
    }
}

/* Called from isr_timer stub (user-mode interrupts only, BKL held) */
void timer_handler(void) {
    BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
    /* Send EOI. */
    if (cpu_count > 1)
        lapic_eoi();
    else
        outb(PIC1_CMD, 0x20);

    kernel_ticks++;

    /* Poll serial for Ctrl+C -- delivers SIGINT to foreground pgrp. */
    console_poll_signals();
    trace_dump();

    /* Wake processes sleeping on tick channel (nanosleep, pause) */
    sched_wakeup(SCHED_TICK_CHAN);

    if (current_proc && current_proc->state == PROC_RUNNING) {
        /*
         * ASSERTION: check iretq frame BEFORE schedule.
         * The CPU just pushed the iretq frame at current_proc's kstack_top.
         * If RIP is already corrupt, the bug is in the frame push (TSS/kstack).
         */
        check_iretq_rip(current_proc, "PRE");

        schedule();

        /*
         * ASSERTION: check iretq frame AFTER schedule.
         * We may now be on a DIFFERENT process's kstack (context_switch).
         * If RIP is corrupt here but was OK at PRE, the bug is in schedule
         * or context_switch (stale kstack, wrong saved_ksp, etc.).
         */
        proc_t *me = current_proc;
        if (me)
            check_iretq_rip(me, "POST");

        /*
         * Signal delivery moved to timer_deliver_signals (called from ISR
         * assembly with frame pointer).  Verified: unified_smp.pml P10.
         */
    }

    /*
     * LAPIC mapping integrity check: verify the PD entry for the LAPIC
     * HHDM range still points to the correct PT page.  If corrupted,
     * the next LAPIC access triple-faults.
     */
    {
        extern uint64_t lapic_guard_pt_pa;
        if (lapic_guard_pt_pa) {
            uint64_t pml4_phys = read_cr3() & 0x000FFFFFFFFFF000ULL;
            uint64_t lapic_vaddr = 0xffff800000000000ULL + acpi_lapic_base;
            uint64_t *pte = paging_walk_pte(pml4_phys, lapic_vaddr);
            if (!pte || !(*pte & 1)) {
                console_write("\n!!! LAPIC MAPPING DESTROYED !!!\n");
                console_write("  cr3=");
                console_hex64(pml4_phys);
                console_write(" expected_pt_pa=");
                console_hex64(lapic_guard_pt_pa);
                if (current_proc) {
                    console_write(" pid=");
                    trace_print_dec((uint64_t)current_proc->pid);
                }
                console_write("\n");
                serial_flush();
                for (;;) __asm__ __volatile__("hlt");
            }
        }
    }
}

/*
 * =======================================================
 * Signal delivery from timer ISR (user-mode preempt).
 *
 * Called from isr_timer assembly with RSP pointing to the ISR frame.
 * Handles BOTH SIG_DFL (terminate) and custom handlers (redirect to
 * user handler via signal trampoline).
 * Verified: models/unified_smp.pml P10.
 * =======================================================
 */

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} timer_isr_frame_t;

typedef struct {
    uint64_t sp;
    uint64_t saved_frame_addr;
    uint64_t siginfo_addr;
} sig_frame_info_t;
extern sig_frame_info_t setup_signal_frame(
    proc_t *cur, int sig, uint64_t user_sp,
    uint64_t ucr3, uint64_t old_cr3);
extern void do_exit_from_isr(int sig);

void timer_deliver_signals(timer_isr_frame_t *frame) {
    proc_t *cur = current_proc;
    if (!cur) return;

    uint32_t pending = cur->sig_pending & ~cur->sig_mask;
    if (!pending) return;

    int sig = 0;
    for (int i = 1; i < NSIG; i++) {
        if (pending & (1u << i)) { sig = i; break; }
    }
    if (!sig) return;

    uint64_t handler = cur->sig_handlers[sig];

    if (handler == SIG_IGN) {
        cur->sig_pending &= ~(1u << sig);
        return;
    }

    if (handler == SIG_DFL) {
        if (sig == SIGCHLD || sig == SIGCONT) return;
        if (sig == SIGSTOP || sig == SIGTSTP ||
            sig == SIGTTIN || sig == SIGTTOU) return;
        cur->sig_pending &= ~(1u << sig);
        do_exit_from_isr(sig);
        return;
    }

    /* Custom handler: build signal frame, redirect ISR return. */
    cur->sig_pending &= ~(1u << sig);

    uint64_t old_cr3 = read_cr3();
    uint64_t ucr3 = cur->pml4_phys;

    sig_frame_info_t info = setup_signal_frame(
        cur, sig, frame->rsp, ucr3, old_cr3);

    /* Write saved registers as syscall_frame_t for rt_sigreturn. */
    if (ucr3) write_cr3(ucr3);
    {
        syscall_frame_t saved;
        saved.r15 = frame->r15; saved.r14 = frame->r14;
        saved.r13 = frame->r13; saved.r12 = frame->r12;
        saved.r11 = frame->r11; saved.r10 = frame->r10;
        saved.r9  = frame->r9;  saved.r8  = frame->r8;
        saved.rsi = frame->rsi; saved.rdi = frame->rdi;
        saved.rbp = frame->rbp; saved.rdx = frame->rdx;
        saved.rcx = frame->rcx; saved.rbx = frame->rbx;
        saved.rax = frame->rax;
        saved.rip = frame->rip;
        saved.rflags = frame->rflags;
        saved.rsp = frame->rsp;
        saved.orig_rax = 0;
        kmemcpy((void *)(uintptr_t)info.saved_frame_addr, &saved, sizeof(saved));
        uint64_t *qw = (uint64_t *)(uintptr_t)info.saved_frame_addr;
        uint64_t cksum = 0xDEAD5164DEAD5164ULL;
        for (int qi = 0; qi < (int)(sizeof(syscall_frame_t)/8); qi++)
            cksum ^= qw[qi];
        *(uint64_t *)(uintptr_t)(info.saved_frame_addr + sizeof(syscall_frame_t)) = cksum;
    }
    if (ucr3) write_cr3(old_cr3);

    frame->rip = handler;
    frame->rdi = (uint64_t)sig;
    frame->rsp = info.sp;
    frame->rsi = info.siginfo_addr ? info.siginfo_addr : 0;
    frame->rdx = 0;

    cur->sig_mask |= cur->sig_sa_mask[sig] | (1u << sig);
    cur->sig_mask &= ~(1u << SIGKILL);
}
