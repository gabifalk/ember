/*
 * FPU/SSE state save/restore model for ember SMP.
 *
 * Each process has its own FPU state. On context_switch:
 *   - fxsave: save current CPU FPU to prev->fpu_state
 *   - fxrstor: restore next->fpu_state to CPU FPU
 *
 * Without save/restore: process A's FPU state is trashed when
 * process B runs on the same CPU (or either CPU under SMP).
 *
 * Verifies:
 *   - After context_switch, CPU FPU matches the running process
 *   - No FPU state corruption across preemption
 *   - Works with process migration between CPUs
 */

#define P_READY    1
#define P_RUNNING  2

/* Per-process saved FPU state (what fxsave stores) */
byte saved_fpu[2];    /* saved_fpu[proc] */

/* Per-CPU live FPU state (what the CPU registers hold) */
byte cpu_fpu[2];      /* cpu_fpu[cpu] */

/* Per-process expected FPU value (set by userspace, checked on resume) */
#define FPU_PROC0  10
#define FPU_PROC1  20

byte pstate[2];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

bool corruption = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* ═══════════════════════════════════════════════════════
 * BUGGY: no FPU save/restore on context_switch
 * ═══════════════════════════════════════════════════════ */

proctype buggy_scenario() {
    /* Init: proc 0 on CPU 0, proc 1 READY */
    atomic {
        pstate[0] = P_RUNNING; cur[0] = 0;
        pstate[1] = P_READY; cur[1] = 255;
        saved_fpu[0] = FPU_PROC0;
        saved_fpu[1] = FPU_PROC1;
        cpu_fpu[0] = FPU_PROC0;  /* proc 0 set its FPU state */
        cpu_fpu[1] = 0
    };

    /* Proc 0 runs on CPU 0, uses FPU (value = FPU_PROC0) */
    assert(cpu_fpu[0] == FPU_PROC0);

    /* Timer preempts proc 0 → schedule switches to proc 1 */
    BKL_ACQ(0);
    /* BUGGY: no fxsave/fxrstor */
    atomic {
        pstate[0] = P_READY;
        pstate[1] = P_RUNNING;
        cur[0] = 1
    };
    BKL_REL(0);

    /* Proc 1 runs on CPU 0, uses FPU (sets its own value) */
    cpu_fpu[0] = FPU_PROC1;

    /* Timer preempts proc 1 → schedule switches back to proc 0 */
    BKL_ACQ(0);
    /* BUGGY: no fxsave/fxrstor */
    atomic {
        pstate[1] = P_READY;
        pstate[0] = P_RUNNING;
        cur[0] = 0
    };
    BKL_REL(0);

    /* Proc 0 resumes — expects FPU == FPU_PROC0 */
    if
    :: cpu_fpu[0] != FPU_PROC0 -> corruption = true
    :: else -> skip
    fi;

    assert(!corruption)
}

/* ═══════════════════════════════════════════════════════
 * FIXED: fxsave/fxrstor on context_switch
 * ═══════════════════════════════════════════════════════ */

proctype fixed_scenario() {
    atomic {
        pstate[0] = P_RUNNING; cur[0] = 0;
        pstate[1] = P_READY; cur[1] = 255;
        saved_fpu[0] = FPU_PROC0;
        saved_fpu[1] = FPU_PROC1;
        cpu_fpu[0] = FPU_PROC0;
        cpu_fpu[1] = 0
    };

    assert(cpu_fpu[0] == FPU_PROC0);

    /* Timer preempts proc 0 → switch to proc 1 */
    BKL_ACQ(0);
    /* FIXED: fxsave prev, fxrstor next */
    atomic {
        saved_fpu[0] = cpu_fpu[0];  /* fxsave: CPU FPU → prev's storage */
        pstate[0] = P_READY;
        pstate[1] = P_RUNNING;
        cur[0] = 1;
        cpu_fpu[0] = saved_fpu[1]   /* fxrstor: next's storage → CPU FPU */
    };
    BKL_REL(0);

    /* Proc 1 runs, modifies FPU */
    cpu_fpu[0] = FPU_PROC1;

    /* Switch back to proc 0 */
    BKL_ACQ(0);
    atomic {
        saved_fpu[1] = cpu_fpu[0];  /* fxsave */
        pstate[1] = P_READY;
        pstate[0] = P_RUNNING;
        cur[0] = 0;
        cpu_fpu[0] = saved_fpu[0]   /* fxrstor */
    };
    BKL_REL(0);

    /* Proc 0 resumes — FPU must be FPU_PROC0 */
    assert(cpu_fpu[0] == FPU_PROC0);

    /* Also verify proc 1's state was saved correctly */
    assert(saved_fpu[1] == FPU_PROC1)
}

/* ═══════════════════════════════════════════════════════
 * FIXED + MIGRATION: proc moves between CPUs
 * ═══════════════════════════════════════════════════════ */

proctype fixed_migrate() {
    atomic {
        pstate[0] = P_RUNNING; cur[0] = 0;
        pstate[1] = P_READY; cur[1] = 255;
        saved_fpu[0] = FPU_PROC0;
        saved_fpu[1] = FPU_PROC1;
        cpu_fpu[0] = FPU_PROC0;
        cpu_fpu[1] = 0
    };

    /* Proc 0 on CPU 0 */
    cpu_fpu[0] = FPU_PROC0;

    /* Preempt proc 0, switch to idle on CPU 0 */
    BKL_ACQ(0);
    atomic {
        saved_fpu[0] = cpu_fpu[0];  /* fxsave */
        pstate[0] = P_READY;
        cur[0] = 255
    };
    BKL_REL(0);

    /* AP (CPU 1) picks up proc 0 — migration */
    BKL_ACQ(1);
    atomic {
        pstate[0] = P_RUNNING;
        cur[1] = 0;
        cpu_fpu[1] = saved_fpu[0]   /* fxrstor on CPU 1 */
    };
    BKL_REL(1);

    /* Proc 0 now on CPU 1 — FPU must be correct */
    assert(cpu_fpu[1] == FPU_PROC0);

    /* AP picks up proc 1 too (after proc 0 sleeps) */
    BKL_ACQ(1);
    atomic {
        saved_fpu[0] = cpu_fpu[1];  /* fxsave proc 0 */
        pstate[0] = P_READY;
        pstate[1] = P_RUNNING;
        cur[1] = 1;
        cpu_fpu[1] = saved_fpu[1]   /* fxrstor proc 1 */
    };
    BKL_REL(1);

    assert(cpu_fpu[1] == FPU_PROC1);
    assert(saved_fpu[0] == FPU_PROC0)
}

/* Toggle scenario */

/* BUGGY:
init { run buggy_scenario() }
*/

/* FIXED:
init { run fixed_scenario() }
*/

/* FIXED + MIGRATION: */
init { run fixed_migrate() }
