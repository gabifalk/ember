/*
 * TSS.RSP0 / gs:0 stale stack model for ember SMP.
 *
 * Hardware uses TSS.RSP0 as the kernel stack when an interrupt
 * arrives in user mode.  gs:0 (kstack_top) is used by syscall_entry.
 * Both must point to the CURRENT process's kstack on THIS CPU.
 *
 * Bug: when a process is switched OFF a CPU, TSS.RSP0 and gs:0
 * may still point to that process's kstack.  If an interrupt (e.g.,
 * TLB shootdown IPI, timer) arrives on the now-idle CPU, hardware
 * pushes a frame onto the stale kstack — which may be in use by
 * another CPU running that process.
 *
 * This model tracks per-CPU tss_rsp0 and gs_kstack separately,
 * and checks that interrupts never write to a stack owned by
 * another CPU.
 *
 * Code: sched.c (schedule), syscall_entry.S (gs:0), isr_entry.S (TSS.RSP0)
 *
 * Verify:
 *   spin -a models/tss_rsp0_stale.pml && \
 *   gcc -O2 -DNFAIR=2 -o pan pan.c && ./pan -E -m200000
 */

#define N_CPUS  2
#define N_PROCS 2
#define NONE    255
#define IDLE_STACK_0  100   /* CPU 0 idle stack id */
#define IDLE_STACK_1  101   /* CPU 1 idle stack id */
#define PROC_STACK_0  0     /* proc 0 kstack id */
#define PROC_STACK_1  1     /* proc 1 kstack id */

/* Per-CPU state */
byte running[N_CPUS];       /* which proc is running (NONE = idle) */
byte gs_kstack[N_CPUS];     /* what gs:0 points to (stack id) */
byte tss_rsp0[N_CPUS];      /* what TSS.RSP0 points to (stack id) */

/* Per-process state */
#define P_READY   1
#define P_RUNNING 2
byte pstate[N_PROCS];

/* Per-stack: which CPU owns it (-1 = free) */
int stack_owner[N_PROCS];   /* indexed by proc id = stack id */

/* Per-stack: has saved state from context_switch (saved_ksp valid) */
bool has_saved_state[N_PROCS];

/* BKL */
bool bkl = false;

/* Error */
bool stack_corruption = false;

inline BKL_ACQ() {
    atomic { !bkl -> bkl = true }
}
inline BKL_REL() {
    bkl = false
}

inline IDLE_STACK(cpu, result) {
    if
    :: cpu == 0 -> result = IDLE_STACK_0
    :: cpu == 1 -> result = IDLE_STACK_1
    fi
}

/* Check if writing to a stack would corrupt saved state or another CPU's data */
inline CHECK_STACK_CONFLICT(stack_id, my_cpu) {
    if
    :: stack_id < N_PROCS ->
        if
        :: stack_owner[stack_id] != -1 && stack_owner[stack_id] != my_cpu ->
            /* Another CPU owns this stack — corruption */
            stack_corruption = true
        :: has_saved_state[stack_id] ->
            /* Stack has saved context from context_switch.
             * Writing an interrupt frame here overwrites saved regs. */
            stack_corruption = true
        :: else -> skip
        fi
    :: else -> skip
    fi
}

/* Interrupt arrives on a CPU: hardware uses TSS.RSP0 to find stack.
 * Models TLB shootdown IPI or timer arriving at any time. */
inline INTERRUPT(cpu) {
    /* Hardware pushes frame onto tss_rsp0[cpu] */
    CHECK_STACK_CONFLICT(tss_rsp0[cpu], cpu)
}

/* Schedule: pick READY process, update gs:0, TSS.RSP0, context_switch */
inline SCHEDULE(cpu) {
    byte prev = running[cpu];
    byte next = NONE;
    byte idle_stk;

    /* Find a READY process */
    if
    :: pstate[0] == P_READY -> next = 0
    :: pstate[1] == P_READY -> next = 1
    :: true -> next = NONE  /* nothing ready */
    fi;

    if
    :: next == NONE && prev != NONE ->
        /* Going idle.  Real code: spin_unlock_irqrestore re-enables
         * interrupts, THEN resets TSS.RSP0 to idle stack, THEN
         * context_switch.  Interrupt in that window uses stale
         * TSS.RSP0 (still points to prev's kstack). */
        pstate[prev] = P_READY;
        running[cpu] = NONE;
        stack_owner[prev] = -1;

#ifdef FIXED
        /* FIX: reset TSS.RSP0 BEFORE re-enabling interrupts
         * (keep cli from spin_lock_irqsave, don't unlock yet) */
        IDLE_STACK(cpu, idle_stk);
        gs_kstack[cpu] = idle_stk;
        tss_rsp0[cpu] = idle_stk;
#else
        /* BUGGY: interrupts re-enabled, TSS.RSP0 still = prev */
        if
        :: true -> INTERRUPT(cpu)   /* interrupt in the window */
        :: true -> skip
        fi;
        IDLE_STACK(cpu, idle_stk);
        gs_kstack[cpu] = idle_stk;
        tss_rsp0[cpu] = idle_stk;
#endif
        has_saved_state[prev] = true;  /* context_switch saves prev */

    :: next != NONE && next != prev && prev != NONE ->
        /* Switch from one process to another.
         * Update gs:0 and TSS.RSP0 BEFORE context_switch.
         * Release prev's kstack BEFORE context_switch.
         * BUG: between TSS.RSP0 update and context_switch,
         * TSS.RSP0 points to next's kstack while we're still
         * on prev's kstack.  An interrupt uses next's kstack
         * for its frame, corrupting next's saved state. */
        pstate[prev] = P_READY;

        pstate[next] = P_RUNNING;
        running[cpu] = next;
        gs_kstack[cpu] = next;     /* syscall_set_kstack */
        tss_rsp0[cpu] = next;      /* tss_update_rsp0 */

        /* Window: spin_unlock_irqrestore re-enables interrupts.
         * TSS.RSP0 = next's kstack, but we're still on prev's.
         * An interrupt fires here → pushes onto next's kstack. */

        /* context_switch saves prev's state, restores next's.
         * FIX2: TSS.RSP0 update + context_switch must be atomic
         * w.r.t. interrupts (cli around the window). */
#ifdef FIX2
        /* Atomic: no interrupt can fire between TSS.RSP0 and context_switch */
        has_saved_state[prev] = true;
        has_saved_state[next] = false;
        stack_owner[prev] = -1;
        stack_owner[next] = cpu;
#else
        has_saved_state[prev] = true;

        /* Interrupt window: TSS.RSP0 points to next (has saved state) */
        if
        :: true -> INTERRUPT(cpu)
        :: true -> skip
        fi;

        has_saved_state[next] = false;
        stack_owner[prev] = -1;
        stack_owner[next] = cpu;
#endif

    :: next != NONE && prev == NONE ->
        /* Idle → process: context_switch restores next's saved state */
        pstate[next] = P_RUNNING;
        running[cpu] = next;
        stack_owner[next] = cpu;
        has_saved_state[next] = false;  /* restored by context_switch */
        gs_kstack[cpu] = next;
        tss_rsp0[cpu] = next;

    :: next == NONE && prev == NONE ->
        skip  /* still idle */

    :: next == prev && prev != NONE ->
        /* Same process, keep running */
        pstate[next] = P_RUNNING;

    fi
}

/* Syscall: process on this CPU does syscall via gs:0 */
inline SYSCALL(cpu) {
    if
    :: running[cpu] != NONE ->
        CHECK_STACK_CONFLICT(gs_kstack[cpu], cpu)
    :: else -> skip
    fi
}

/* CPU loop: BKL-serialized schedule/syscall + unserialized interrupts */
proctype cpu_loop(byte cpu) {
    byte iter = 0;
    do
    :: iter < 8 ->
        iter++;
        if
        :: true ->
            /* Interrupt: can happen WITHOUT BKL (e.g., TLB IPI) */
            INTERRUPT(cpu)
        :: true ->
            /* Schedule or syscall: under BKL */
            BKL_ACQ();
            if
            :: true -> SCHEDULE(cpu)
            :: true -> SYSCALL(cpu)
            fi;
            BKL_REL()
        fi
    od
}

init {
    byte idle0, idle1;
    IDLE_STACK(0, idle0);
    IDLE_STACK(1, idle1);

    running[0] = NONE;
    running[1] = NONE;
    gs_kstack[0] = idle0;
    gs_kstack[1] = idle1;
    tss_rsp0[0] = idle0;
    tss_rsp0[1] = idle1;

    pstate[0] = P_READY;
    pstate[1] = P_READY;
    stack_owner[0] = -1;
    stack_owner[1] = -1;
    has_saved_state[0] = false;
    has_saved_state[1] = false;

    run cpu_loop(0);
    run cpu_loop(1)
}

/* Safety: no interrupt writes to a stack owned by another CPU */
ltl no_stack_corruption { [] !stack_corruption }
