/*
 * Syscall rdi corruption via timer preempt model.
 *
 * syscall_entry sets rdi = gs:0 - 152 (frame pointer).
 * Between sti and call syscall_dispatch, a timer can fire.
 * Timer ISR saves all GPRs including rdi on the kstack.
 * timer_handler → schedule → context_switch saves callee-saved regs.
 * Process is preempted.
 *
 * When rescheduled: context_switch restores callee-saved regs.
 * Returns to schedule → timer_handler → timer ISR stub.
 * ISR stub pops all GPRs including rdi from kstack.
 * rdi should be the original frame pointer.
 *
 * Bug hypothesis: kstack corruption between save and restore.
 * If another CPU's interrupt writes to this kstack via stale
 * TSS.RSP0, the saved rdi is overwritten.
 *
 * Verify:
 *   spin -a models/syscall_rdi_preempt.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define CORRECT_RDI  1
#define BAD_RDI      2

byte rdi_on_stack = 0;    /* saved rdi on kstack */
byte rdi_in_cpu = 0;      /* rdi register value */
bool rdi_wrong = false;

/* TSS.RSP0 points to this process's kstack? */
bool tss_points_here = true;

/* Can another CPU write to our kstack via TSS.RSP0? */
bool kstack_corrupted = false;

proctype syscall_and_preempt() {
    /* syscall_entry: rdi = frame pointer (correct) */
    rdi_in_cpu = CORRECT_RDI;

    /* sti: timer can now fire */

    /* Timer fires: ISR saves rdi to kstack */
    rdi_on_stack = rdi_in_cpu;   /* push rdi */

    /* timer_handler → schedule → context_switch */
    /* Process preempted. TSS.RSP0 updated to next process.
     * But with cli-across-context_switch fix, no interrupt window. */

    /* While preempted: can another CPU corrupt our kstack? */
    if
    :: true ->
        /* Another CPU's interrupt via stale TSS.RSP0 overwrites our kstack */
        rdi_on_stack = BAD_RDI;
        kstack_corrupted = true
    :: true ->
        skip  /* no corruption */
    fi;

    /* Rescheduled: context_switch returns, ISR restores rdi from kstack */
    rdi_in_cpu = rdi_on_stack;   /* pop rdi */

    /* call syscall_dispatch with rdi */
    if
    :: rdi_in_cpu != CORRECT_RDI -> rdi_wrong = true
    :: else -> skip
    fi
}

init {
    run syscall_and_preempt()
}

ltl no_rdi_wrong { [] !rdi_wrong }
