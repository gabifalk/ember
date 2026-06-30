/*
 * Nested interrupt between swapgs and iretq — gs:0 corruption.
 *
 * Timer ISR user-mode path:
 *   swapgs (user→kernel)
 *   push regs
 *   bkl_acquire
 *   timer_handler → schedule → context_switch (may set gs:0 to new proc)
 *   bkl_release
 *   pop regs
 *   swapgs (kernel→user)    ← interrupts enabled here
 *   iretq                   ← returns to user mode
 *
 * Bug: between swapgs (kernel→user) and iretq, interrupts are enabled.
 * A nested timer fires.  Hardware sees user CS on stack → user-mode path.
 * Nested ISR does swapgs (user→kernel) + schedule.  Schedule may switch
 * to a different process, setting gs:0 to that process's kstack.
 * Nested ISR returns: swapgs (kernel→user) + iretq.
 * Outer ISR's iretq returns to the ORIGINAL process's user mode.
 * Next syscall: swapgs (user→kernel).  gs:0 = WRONG process's kstack!
 *
 * Fix: cli before swapgs in ISR exit.  Prevents nested interrupt.
 *
 * Verify:
 *   spin -a models/nested_isr_swapgs.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define N_CPUS  1    /* single CPU is enough to show the bug */
#define N_PROCS 2

#define NONE 255

byte gs_kstack = NONE;       /* what gs:0 points to */
byte running_proc = 0;       /* process running in user mode */

#define P_READY   1
#define P_RUNNING 2
byte pstate[N_PROCS];

/* gs base state: 0=user, 1=kernel */
byte gs_base = 1;   /* start in kernel */

bool gs_wrong = false;

/* Timer ISR: schedule may switch process, updating gs:0 */
inline TIMER_ISR_SCHEDULE() {
    /* schedule: pick a READY process */
    byte next = NONE;
    if
    :: pstate[0] == P_READY -> next = 0
    :: pstate[1] == P_READY -> next = 1
    :: true -> next = NONE
    fi;

    if
    :: next != NONE && next != running_proc ->
        pstate[running_proc] = P_READY;
        pstate[next] = P_RUNNING;
        running_proc = next;
        gs_kstack = next;      /* syscall_set_kstack */
    :: else -> skip
    fi
}

/* Model the timer ISR return path */
inline TIMER_ISR_RETURN(can_nest) {
    /* bkl_release */
    /* pop regs */

    /* swapgs: kernel → user */
    gs_base = 0;

    /* BUGGY: interrupts enabled, nested timer can fire */
#ifndef FIXED
    if
    :: can_nest ->
        /* Nested timer fires between swapgs and iretq.
         * Hardware sees user CS → takes user-mode path. */
        gs_base = 1;    /* nested swapgs: user → kernel */
        TIMER_ISR_SCHEDULE();
        /* Nested ISR return */
        gs_base = 0;    /* nested swapgs: kernel → user */
        /* nested iretq: returns to outer ISR's iretq point */
    :: true -> skip     /* no nested interrupt */
    fi;
#endif

    /* iretq: returns to user mode */
    gs_base = 0;  /* user mode */
}

proctype cpu() {
    byte iter = 0;
    byte actual_proc;   /* which process is ACTUALLY on this CPU */

    actual_proc = running_proc;

    do
    :: iter < 6 ->
        iter++;

        /* Process running in user mode.
         * actual_proc = which process iretq returned to.
         * running_proc = what schedule thinks is running (may differ after nested ISR). */

        /* Syscall: swapgs (user→kernel), read gs:0 */
        gs_base = 1;

        /* CHECK: gs:0 must match the ACTUAL process on this CPU */
        if
        :: gs_kstack != actual_proc -> gs_wrong = true
        :: else -> skip
        fi;

        /* syscall_dispatch ... */
        /* syscall_return: cli, swapgs, sysretq (safe, interrupts disabled) */
        gs_base = 0;

        /* User mode: timer fires */
        gs_base = 1;   /* swapgs: user → kernel */

        /* Remember which process was running before timer */
        byte before_timer = actual_proc;

        TIMER_ISR_SCHEDULE();

        /* actual_proc updated by schedule */
        actual_proc = running_proc;

        TIMER_ISR_RETURN(true);

        /* After outer iretq: CPU returns to whatever was on the stack.
         * If no nested interrupt, returns to actual_proc (schedule's choice).
         * If nested interrupt changed running_proc, outer iretq STILL
         * returns to the process that was on the stack when the OUTER
         * timer fired — which is before_timer.
         * But we can't easily distinguish this in the model.
         * The key: outer iretq returns to the process that was preempted
         * by the OUTER timer. */

        /* After outer iretq:
         * - Normal (no nesting): iretq returns to schedule's chosen process.
         *   actual_proc = running_proc (set by schedule).
         * - Nested: outer iretq returns to OUTER schedule's process, but
         *   nested ISR changed running_proc/gs_kstack. The outer process
         *   is before_timer (what was on the stack when outer timer fired).
         *   But actually, if outer schedule did context_switch, the stack
         *   is the NEW process's stack. Outer iretq returns to that process.
         *   If nested ISR then changes running_proc, gs_kstack is wrong. */
#ifndef FIXED
        /* In BUGGY mode, nested ISR may have changed running_proc.
         * Outer iretq returns to outer schedule's process (actual_proc),
         * but gs_kstack was set by nested ISR's schedule. */
        actual_proc = actual_proc;  /* outer schedule's choice (before nesting) */
#else
        /* FIXED: no nesting, so actual_proc = running_proc */
        actual_proc = running_proc;
#endif
    od
}

init {
    pstate[0] = P_RUNNING;
    pstate[1] = P_READY;
    running_proc = 0;
    gs_kstack = 0;
    gs_base = 1;
    run cpu()
}

ltl no_gs_wrong { [] !gs_wrong }
