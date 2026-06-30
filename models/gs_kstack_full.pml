/*
 * Full gs:0/TSS.RSP0 lifecycle model for ember schedule().
 *
 * Tracks gs:0 through ALL schedule paths:
 *   A. Timer preempt, same process (line 144-146: return, no switch)
 *   B. Going idle with prev (line 160-186: idle gs:0, context_switch, resume, return)
 *   C. Proc-to-proc switch (line 210-256: set gs:0, context_switch)
 *   D. Idle-to-proc switch (line 248-250: from idle stack)
 *   E. Idle loop (smp.c:191-195: schedule returns to idle loop)
 *
 * The bug: path B sets gs:0=idle, context_switches away. When resumed
 * (by path C or D on another CPU), gs:0 is set by the re-scheduling
 * CPU before context_switch. Process resumes at path B's return point.
 * If gs:0 on the re-scheduling CPU is correct, the process returns to
 * user mode with correct gs:0.
 *
 * But what if the process resumes and then schedule is called again
 * on the SAME CPU? Does gs:0 ever revert to idle incorrectly?
 *
 * Verify:
 *   spin -a models/gs_kstack_full.pml && \
 *   gcc -O2 -DNFAIR=2 -o pan pan.c && ./pan -E -m200000
 */

#define N_CPUS  2
#define N_PROCS 2
#define NONE    255

byte running[N_CPUS];
byte gs_kstack[N_CPUS];     /* NONE=idle, 0/1=proc kstack */
byte tss_rsp0[N_CPUS];

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3
byte pstate[N_PROCS];

bool bkl = false;
bool gs_mismatch = false;

inline BKL_ACQ() { atomic { !bkl -> bkl = true } }
inline BKL_REL() { bkl = false }

/* Check: when process does syscall, gs:0 must be its kstack */
inline CHECK_GS(cpu) {
    if
    :: running[cpu] != NONE && gs_kstack[cpu] != running[cpu] ->
        gs_mismatch = true
    :: else -> skip
    fi
}

/* Full schedule model matching sched.c */
inline SCHEDULE(cpu) {
    byte prev = running[cpu];
    byte next = NONE;

    /* Find READY process */
    if
    :: pstate[0] == P_READY -> next = 0
    :: pstate[1] == P_READY -> next = 1
    :: true -> next = NONE
    fi;

    if
    :: next == NONE && prev != NONE && pstate[prev] == P_RUNNING ->
        /* Path A: nothing ready, prev still running, return to it */
        skip

    :: next == NONE && prev != NONE && pstate[prev] != P_RUNNING ->
        /* Path B: going idle. Set gs:0=idle, context_switch to idle.
         * Process saved. When resumed, returns from HERE. */
        running[cpu] = NONE;
        gs_kstack[cpu] = NONE;    /* line 172: idle stack */
        tss_rsp0[cpu] = NONE;
        pstate[prev] = P_READY;
        /* context_switch saves prev. Idle loop continues.
         * When prev is re-scheduled (path C/D on some CPU),
         * that CPU sets gs:0 before context_switch.
         * prev resumes HERE, returns from schedule(). */

    :: next == NONE && prev == NONE ->
        /* Path E: idle, nothing ready, continue idle loop */
        skip

    :: next != NONE && next == prev ->
        /* Same process, keep running */
        skip

    :: next != NONE && next != prev && prev != NONE ->
        /* Path C: proc-to-proc switch */
        pstate[prev] = P_READY;
        running[cpu] = next;
        pstate[next] = P_RUNNING;
        gs_kstack[cpu] = next;     /* line 221 */
        tss_rsp0[cpu] = next;      /* line 222 */
        /* context_switch: prev saved, next resumed */

    :: next != NONE && prev == NONE ->
        /* Path D: idle-to-proc switch */
        running[cpu] = next;
        pstate[next] = P_RUNNING;
        gs_kstack[cpu] = next;     /* line 221 */
        tss_rsp0[cpu] = next;      /* line 222 */
        /* context_switch: idle saved, next resumed */
    fi
}

/* CPU loop: idle loop or user-mode process */
proctype cpu_loop(byte cpu) {
    byte iter = 0;
    do
    :: iter < 10 ->
        iter++;

        BKL_ACQ();

        /* Nondeterministic: timer preempt, syscall, or sleep */
        if
        :: true ->
            /* Timer preempt: schedule */
            SCHEDULE(cpu)
        :: running[cpu] != NONE ->
            /* Syscall: check gs:0 */
            CHECK_GS(cpu)
        :: running[cpu] != NONE ->
            /* Process sleeps (e.g., pipe read) */
            pstate[running[cpu]] = P_SLEEPING;
            SCHEDULE(cpu)
        fi;

        BKL_REL()
    od
}

init {
    gs_kstack[0] = NONE;
    gs_kstack[1] = NONE;
    tss_rsp0[0] = NONE;
    tss_rsp0[1] = NONE;
    running[0] = NONE;
    running[1] = NONE;
    pstate[0] = P_READY;
    pstate[1] = P_READY;
    run cpu_loop(0);
    run cpu_loop(1)
}

ltl no_gs_mismatch { [] !gs_mismatch }
