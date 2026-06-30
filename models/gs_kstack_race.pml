/*
 * Per-CPU gs:0 (kstack_top) correctness model for ember SMP.
 *
 * syscall_entry reads gs:0 to switch to the kernel stack.
 * schedule() writes gs:0 when switching to a new process.
 * Each CPU has its own gs:0 via per-CPU cpu_local struct.
 *
 * Invariant: when a process does a syscall on CPU X, gs:0 on CPU X
 * must point to THAT process's kernel stack (set by schedule when
 * the process was last switched IN on CPU X).
 *
 * Potential bug: process P scheduled on CPU A (gs_kstack[A] = P's stack).
 * Timer preempts P. P goes READY. CPU B picks up P (gs_kstack[B] = P's stack).
 * P runs on CPU B, does syscall. gs_kstack[B] correct.
 * But what about gs_kstack[A]? It still points to P's stack — stale.
 * If another process Q is now on CPU A, gs_kstack[A] should be Q's stack.
 *
 * The question: does schedule always update gs:0 before the process
 * returns to user mode?
 *
 * Verify:
 *   spin -a models/gs_kstack_race.pml && \
 *   gcc -O2 -DNFAIR=2 -o pan pan.c && ./pan -E -m200000
 */

#define N_CPUS  2
#define N_PROCS 2

#define PROC_NONE 255

/* Per-CPU gs:0 kstack — who it points to */
byte gs_kstack[N_CPUS];   /* PROC_NONE or proc id */

/* Per-CPU: which process is running */
byte running[N_CPUS];     /* PROC_NONE or proc id */

/* Process state */
#define P_READY   1
#define P_RUNNING 2
#define P_IDLE    3    /* no process */

byte pstate[N_PROCS];

/* BKL */
bool bkl = false;
byte bkl_cpu = 255;

/* Error */
bool gs_mismatch = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    bkl_cpu = 255; bkl = false
}

/* Schedule: pick a READY process, set gs_kstack, run it */
inline SCHEDULE(cpu) {
    byte picked = PROC_NONE;
    /* Find a READY process */
    if
    :: pstate[0] == P_READY -> picked = 0
    :: pstate[1] == P_READY -> picked = 1
    :: else -> picked = PROC_NONE
    fi;

    if
    :: picked != PROC_NONE ->
        /* Context switch: update per-CPU state */
        pstate[picked] = P_RUNNING;
        running[cpu] = picked;
        gs_kstack[cpu] = picked;  /* syscall_set_kstack(proc's stack) */
    :: else ->
        running[cpu] = PROC_NONE
    fi
}

/* Syscall: process does syscall, gs:0 must match */
inline SYSCALL(cpu) {
    byte proc = running[cpu];
    if
    :: proc != PROC_NONE ->
        /* Check: gs_kstack[cpu] must equal the running process */
        if
        :: gs_kstack[cpu] != proc -> gs_mismatch = true
        :: else -> skip
        fi
    :: else -> skip  /* no process, no syscall */
    fi
}

/* Timer preempt: move running process back to READY */
inline TIMER_PREEMPT(cpu) {
    byte proc = running[cpu];
    if
    :: proc != PROC_NONE ->
        pstate[proc] = P_READY;
        running[cpu] = PROC_NONE
    :: else -> skip
    fi
}

/* CPU: idle loop with timer-driven scheduling */
proctype cpu_loop(byte cpu) {
    byte iter = 0;
    do
    :: iter < 6 ->
        iter++;

        BKL_ACQ(cpu);

        /* Nondeterministic: schedule, syscall, or timer preempt */
        if
        :: true -> SCHEDULE(cpu)
        :: true -> SYSCALL(cpu)
        :: true -> TIMER_PREEMPT(cpu)
        fi;

        BKL_REL(cpu)
    od
}

init {
    gs_kstack[0] = PROC_NONE;
    gs_kstack[1] = PROC_NONE;
    running[0] = PROC_NONE;
    running[1] = PROC_NONE;
    pstate[0] = P_READY;
    pstate[1] = P_READY;

    run cpu_loop(0);
    run cpu_loop(1)
}

/* Safety: gs_kstack always matches running process at syscall time */
ltl no_gs_mismatch { [] !gs_mismatch }
