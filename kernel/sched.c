/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/proc.h"
#include "ember/sched.h"
#include "ember/syscall.h"
#include "ember/paging.h"
#include "ember/console.h"
#include "ember/spinlock.h"
#include "ember/bug.h"
#include "ember/bkl.h"
#include "ember/lapic.h"

#define IA32_FS_BASE 0xC0000100u

extern void context_switch(uint64_t *old_ksp, uint64_t new_ksp);

extern void tss_update_rsp0(uint64_t val);

static inline void wrmsr_sched(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

atomic_spinlock_t bkl = ATOMIC_SPINLOCK_INIT;
volatile int bkl_holder_cpu = -1;
uint64_t kernel_idle_cr3;  /* Safe CR3 for idle CPUs (set by kmain) */

/*
 * Non-inline wrappers callable from assembly entry points.
 *
 * Nesting: bkl_acquire_entry skips if already held (nested exception
 * during syscall).  bkl_release_entry always releases.  For kernel-mode
 * exceptions, isr_common's CS check skips the release call entirely,
 * so the nested acquire/release is balanced.  For user-mode returns,
 * the release is correct (the caller that acquired BKL is returning).
 */
void bkl_acquire_entry(void) {
    if (cpu_count <= 1) return;          /* Fast path: single CPU. */
    if (bkl_held_by_this_cpu()) return;  /* Nested entry (interrupt in kernel) */
    bkl_acquire();
}

void bkl_release_entry(void) {
    if (cpu_count <= 1) return;
    bkl_release();
}

/*
 * Per-CPU idle stacks for schedule() idle path.
 * When a process sleeps and nothing is READY, we context_switch to
 * the idle stack before releasing BKL. This frees the process's kstack
 * so another CPU can safely schedule it. (Verified: bkl_sched_v8.pml)
 */
#define IDLE_STACK_SIZE 16384
static uint8_t idle_stacks[MAX_CPUS][IDLE_STACK_SIZE] __attribute__((aligned(16)));
static uint64_t idle_saved_ksp[MAX_CPUS];
static int idle_stack_initialized[MAX_CPUS];

/*
 * Kstack ownership tracking (matches bkl_sched_v8.pml kstack_owner[]).
 * kstack_cpu[proc_idx] = CPU using this process's kstack, or -1 if free.
 * A process can only be context_switched to if its kstack is free.
 */
static int kstack_cpu[MAX_PROCS];

spinlock_t sched_lock = SPINLOCK_INIT;
static int sched_idx; /* Round-robin cursor. */
/*
 * Upper bound (exclusive) for proc table scans.  Only indices < sched_proc_limit
 * need to be examined -- all slots at or above are guaranteed PROC_UNUSED.
 * Updated by sched_note_slot() when a slot becomes active.
 */
static int sched_proc_limit;

/*
 * Per-CPU idle loop: entered via context_switch when schedule() needs to
 * idle on SMP. BKL is held on entry (from the schedule() that switched us).
 * Loops: release BKL, hlt, reacquire, call schedule() to find work.
 * When schedule() context_switches away, idle_saved_ksp[cpu] is saved.
 * When a process needs to idle, we resume here from the schedule() call.
 */
/*
 * Quick racy check: is any process READY or tick-sleeping?
 * No locks -- just a hint to avoid BKL contention from idle CPUs.
 * False negatives (miss a wakeup) are harmless: next tick retries.
 */
int sched_any_work(void) {
    int limit = sched_proc_limit;
    for (int i = 0; i < limit; i++) {
        int st = procs[i].state;
        if (st == PROC_READY || st == PROC_SLEEPING)
            return 1;
    }
    return 0;
}

static void sched_idle_loop(void) {
    for (;;) {
        bkl_release();
        /*
         * Idle: hlt, wake on timer tick, trylock BKL.
         * Only one CPU wins trylock per tick -- others go back to hlt.
         * No sched_any_work gate: must always wake so tick-sleeping
         * processes (nanosleep, console read) get sched_wakeup.
         */
        do {
            __asm__ __volatile__("sti; hlt; cli" ::: "memory");
        } while (!bkl_tryacquire());
        /* Wake tick-sleeping processes after BKL reacquire. */
        sched_wakeup(SCHED_TICK_CHAN);
        schedule();
    }
}

void sched_init(void) {
    sched_idx = 0;
    sched_proc_limit = 0;
    for (int i = 0; i < MAX_PROCS; i++)
        kstack_cpu[i] = -1;
}

/*
 * Initialize idle stack for a CPU. Called once per CPU.
 * Sets up idle_saved_ksp[cpu] so that context_switch to it
 * "returns" into sched_idle_loop().
 */
void sched_init_idle(int cpu) {
    /*
     * context_switch pops: r15, r14, r13, r12, rbx, rbp, then ret.
     * We set up 7 slots: 6 dummy regs + return address.
     */
    uint64_t *sp = (uint64_t *)(idle_stacks[cpu] + IDLE_STACK_SIZE);
    sp -= 7;
    sp[0] = 0;  /* R15. */
    sp[1] = 0;  /* R14. */
    sp[2] = 0;  /* R13. */
    sp[3] = 0;  /* R12. */
    sp[4] = 0;  /* Rbx. */
    sp[5] = 0;  /* Rbp. */
    sp[6] = (uint64_t)(uintptr_t)sched_idle_loop;  /* Return address. */
    idle_saved_ksp[cpu] = (uint64_t)(uintptr_t)sp;
    idle_stack_initialized[cpu] = 1;
}


/*
 * Notify scheduler that proc slot 'idx' is now in use.
 * Called from proc_alloc() after assigning a slot.
 */
void sched_note_slot(int idx) {
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    if (idx + 1 > sched_proc_limit)
        sched_proc_limit = idx + 1;
    spin_unlock_irqrestore(&sched_lock, flags);
}

void schedule(void) {
    /* Model invariant: BKL must be held when scheduling under SMP. */
    BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());

    proc_t *prev = current_proc;
    if (prev)
        proc_check_stack(prev);

    /* Round-robin: find next READY process. */
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sched_lock);
        proc_t *next = 0;
        int limit = sched_proc_limit;
        if (limit > 0) {
            int start = sched_idx;
            if (start >= limit) start = 0;
            for (int i = 0; i < limit; i++) {
                int idx = start + i;
                if (idx >= limit) idx -= limit;
                if (procs[idx].state == PROC_READY) {
                    next = &procs[idx];
                    sched_idx = idx + 1;
                    break;
                }
            }
        }

        if (!next) {
            /*
             * Nothing to switch to. If prev is still RUNNING (timer preempt,
             * nothing else ready), return to it. Only valid before first idle
             * (BKL held continuously). Verified: bkl_sched_v7.pml.
             */
            if (prev && prev->state == PROC_RUNNING) {
                spin_unlock_irqrestore(&sched_lock, flags);
                return;
            }
            /*
             * Process is sleeping (or CPU idle) and nothing is READY.
             *
             * Verified (bkl_sched_v8.pml): MUST context_switch off prev's
             * kstack before releasing BKL, otherwise another CPU can schedule
             * prev and both CPUs share the same kstack = corruption.
             *
             * SMP with prev: context_switch to per-CPU idle stack.
             * Keep interrupts disabled until TSS.RSP0 is reset to idle
             * stack -- otherwise an interrupt pushes onto prev's released
             * kstack.  Verified: tss_rsp0_stale.pml (FIXED path).
             */
            spin_unlock(&sched_lock);
            /* Interrupts still disabled from spin_lock_irqsave. */
            if (cpu_count > 1 && prev) {
                int cpu = this_cpu_id();
                int prev_idx = (int)(prev - procs);
                BUG_ON(!idle_stack_initialized[cpu]);
                set_this_cpu_proc(0);
                /* Release prev's kstack (v8 model: kstack_owner = 255) */
                kstack_cpu[prev_idx] = -1;
                /*
                 * Reset gs:0, TSS.RSP0, and CR3 for idle.
                 * CR3 must switch to a safe kernel PML4 -- prev's PML4
                 * may be freed by exec/exit on another CPU.
                 * Without this: dangling CR3 -> PML4 reused -> kernel
                 * mapping destroyed -> triple fault on next interrupt.
                 */
                {
                    uint64_t idle_top = (uint64_t)(idle_stacks[cpu] + IDLE_STACK_SIZE);
                    syscall_set_kstack(idle_top);
                    tss_update_rsp0(idle_top);
                    extern uint64_t kernel_idle_cr3;
                    if (kernel_idle_cr3)
                        write_cr3(kernel_idle_cr3);
                }
                __asm__ __volatile__("fxsave (%0)" : : "r"(fxsave_ptr(prev)) : "memory");
                context_switch(&prev->saved_ksp, idle_saved_ksp[cpu]);
                /* Resumed: on process kstack, TSS.RSP0 set by re-scheduling CPU. */
                if (flags & 0x200)
                    __asm__ __volatile__("sti" ::: "memory");
                /* Restore FPU. */
                {
                    proc_t *me = current_proc;
                    if (me)
                        __asm__ __volatile__("fxrstor (%0)" : : "r"(fxsave_ptr(me)) : "memory");
                }
                return;
            }
            /*
             * SMP idle (prev==NULL, on idle stack) or single CPU: release
             * BKL, hlt, reacquire. prev's kstack is already free.
             */
            if (cpu_count > 1) bkl_release();
            if (cpu_count > 1) {
                /* Idle: hlt, wake on tick, trylock BKL (no spin) */
                do {
                    __asm__ __volatile__("sti; hlt; cli" ::: "memory");
                } while (!bkl_tryacquire());
                /*
                 * Wake tick-sleeping processes (nanosleep, poll).
                 * Ticks advanced by timer_eoi_kernel (safe without BKL).
                 * Must wake here under BKL so they can be found READY.
                 */
                sched_wakeup(SCHED_TICK_CHAN);
            } else {
                __asm__ __volatile__("sti; hlt; cli" ::: "memory");
            }
            continue;
        }

        if (next == prev) {
            /* Only runnable process -- just mark it running again. */
            next->state = PROC_RUNNING;
            spin_unlock_irqrestore(&sched_lock, flags);
            return;
        }

        /*
         * Switch to next -- model invariants (v8): next must be READY,
         * and its kstack must be free (no CPU using it).
         */
        {
            int next_idx = (int)(next - procs);
            BUG_ON(next->state != PROC_READY);
            BUG_ON(cpu_count > 1 && kstack_cpu[next_idx] != -1);
            kstack_cpu[next_idx] = this_cpu_id();
        }
        next->state = PROC_RUNNING;
        set_this_cpu_proc(next);

        uint64_t kstack_top = (uint64_t)(uintptr_t)(next->kstack + PROC_KSTACK_SIZE);
        syscall_set_kstack(kstack_top);
        tss_update_rsp0(kstack_top);
        syscall_set_user_cr3(next->pml4_phys);
        write_cr3(next->pml4_phys);
        wrmsr_sched(IA32_FS_BASE, next->fs_base);

        if (prev) {
            if (prev->state == PROC_RUNNING) {
                prev->state = PROC_READY;
                /* Kick idle CPUs to pick up the now-READY process. */
                if (cpu_count > 1 && lapic_enabled)
                    lapic_send_ipi_all_excl_self(0x40);
            }
            /* Release prev's kstack before context_switch. */
            kstack_cpu[(int)(prev - procs)] = -1;
        }

        /*
         * Release sched_lock but keep interrupts DISABLED.
         * TSS.RSP0 points to next's kstack; an interrupt before
         * context_switch would push a frame onto next's saved state.
         * Verified: models/tss_rsp0_stale.pml (FIX2)
         */
        spin_unlock(&sched_lock);
        /* Interrupts still disabled from spin_lock_irqsave. */

        /*
         * Save prev's FPU/SSE state before context_switch.
         * Verified: models/fpu.pml -- without this, FPU state corrupted.
         */
        if (prev)
            __asm__ __volatile__("fxsave (%0)" : : "r"(fxsave_ptr(prev)) : "memory");

        if (prev) {
            context_switch(&prev->saved_ksp, next->saved_ksp);
        } else {
            int cpu = this_cpu_id();
            context_switch(&idle_saved_ksp[cpu], next->saved_ksp);
        }

        /*
         * context_switch done -- on next's kstack, TSS.RSP0 matches.
         * Re-enable interrupts (restore flags from spin_lock_irqsave).
         */
        if (flags & 0x200)
            __asm__ __volatile__("sti" ::: "memory");

        /*
         * Restore resumed process's FPU/SSE state.
         * After context_switch, we are "next" (resumed from earlier preemption).
         * current_proc was set to us before context_switch.
         */
        {
            proc_t *me = current_proc;
            if (me)
                __asm__ __volatile__("fxrstor (%0)" : : "r"(fxsave_ptr(me)) : "memory");
        }
        return;
    }
}

void sched_yield(void) {
    schedule();
}

void sched_sleep(int chan) {
    BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
    if (!current_proc) return;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    current_proc->state = PROC_SLEEPING;
    current_proc->wait_chan = chan;
    spin_unlock_irqrestore(&sched_lock, flags);
    schedule();
}

void sched_wakeup(int chan) {
    BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    int limit = sched_proc_limit;
    for (int i = 0; i < limit; i++) {
        if (procs[i].state == PROC_SLEEPING && procs[i].wait_chan == chan) {
            procs[i].state = PROC_READY;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
}

int sched_wakeup_n(int chan, int max_wake) {
    BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
    int woken = 0;
    uint64_t flags = spin_lock_irqsave(&sched_lock);
    int limit = sched_proc_limit;
    for (int i = 0; i < limit && woken < max_wake; i++) {
        if (procs[i].state == PROC_SLEEPING && procs[i].wait_chan == chan) {
            procs[i].state = PROC_READY;
            woken++;
        }
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return woken;
}
