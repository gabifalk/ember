/*
 * BKL + Scheduler model v6 — stale prev bug check.
 *
 * Minimal model: 2 procs, 2 CPUs. Proc 0 sleeps, proc 1 wakes it.
 * Tests if schedule()'s stale prev causes dual-run.
 *
 * prev is cached ONCE at schedule() entry (matching C code).
 * After idle hlt + BKL reacquire, prev->state is checked but
 * prev may have migrated to the other CPU.
 */

#define N_PROCS 2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

/* ── BKL ─────────────────────────────────────────────── */
inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* ── Focused scenario ────────────────────────────────────
 *
 * 1. BSP runs proc 0 (RUNNING). AP idle.
 * 2. Proc 0 sleeps: set SLEEPING, call schedule().
 * 3. schedule() caches prev = proc 0.
 * 4. schedule() searches: proc 1 is READY. Switch to proc 1.
 *    prev (proc 0) set to READY (was RUNNING... wait, it's SLEEPING).
 *    Actually: proc 0 was set to SLEEPING by sched_sleep BEFORE
 *    schedule() was called. So prev->state == SLEEPING. schedule()
 *    won't set it to READY (only sets READY if prev was RUNNING).
 *    Proc 1 becomes RUNNING on BSP.
 *
 * 5. Proc 1 runs on BSP, wakes proc 0: SLEEPING → READY.
 * 6. Timer preempts proc 1 on BSP. schedule() called.
 *    Finds proc 0 READY. Switch: proc 1 → READY, proc 0 → RUNNING on BSP.
 *
 * Alternative path after step 3:
 * 4'. schedule() searches: nothing READY (proc 1 not ready yet).
 * 5'. schedule() idle: BKL_REL, hlt.
 * 6'. AP acquires BKL, wakes proc 0 somehow, schedules proc 0 on AP.
 *     Proc 0 is now RUNNING on AP.
 * 7'. BSP reacquires BKL. Checks prev->state == PROC_RUNNING (true!
 *     because AP set it). Returns from schedule(). BSP thinks it's
 *     running proc 0 — but proc 0 is on AP. DUAL RUN.
 *
 * Model the alternative path explicitly.
 */

proctype bsp() {
    byte prev;

    /* BSP starts with proc 0 running */
    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };

    /* Proc 0 makes a syscall that sleeps */
    BKL_ACQ(0);

    /* sched_sleep: set SLEEPING */
    pstate[0] = P_SLEEPING;

    /* schedule(): cache prev */
    prev = cur[0];  /* prev = 0 (proc 0) */
    assert(prev == 0);

    /* Search for READY proc (under sched_lock + BKL) */
    /* Proc 1 might or might not be READY yet */
    if
    :: pstate[1] == P_READY ->
        /* Switch to proc 1 */
        /* prev is SLEEPING, not RUNNING, so don't set READY */
        pstate[1] = P_RUNNING;
        cur[0] = 1;
        BKL_REL(0);
        /* Now running proc 1 on BSP. Proc 1 will wake proc 0. */
        /* (Modeled in the AP path for simplicity) */
        skip

    :: else ->
        /* Nothing READY. Idle: release BKL, hlt, reacquire */
        BKL_REL(0);
        skip;  /* hlt — AP can run now */
        BKL_ACQ(0);

        /* Re-enter search loop. Check stale prev. */
        /* THIS IS THE BUG LINE: prev was cached before idle hlt */
        if
        :: pstate[prev] == P_RUNNING ->
            /* C code: return (prev is "still running").
             * But prev might be RUNNING on the OTHER CPU!
             * Assert: prev must NOT be running on the other CPU. */
            assert(cur[1] != prev || pstate[prev] != P_RUNNING);  /* DUAL RUN check */
            BKL_REL(0)
        :: else ->
            /* prev not running, search again */
            if
            :: pstate[1] == P_READY ->
                pstate[1] = P_RUNNING;
                cur[0] = 1;
                BKL_REL(0)
            :: else ->
                BKL_REL(0)
            fi
        fi
    fi
}

proctype ap() {
    cur[1] = 255;

    /* AP idle: wait for BKL */
    BKL_ACQ(1);

    /* AP wakes proc 0 and schedules it */
    if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;

    /* Schedule: find proc 0 READY, switch to it */
    if
    :: pstate[0] == P_READY ->
        pstate[0] = P_RUNNING;
        cur[1] = 0
    :: else -> skip
    fi;

    BKL_REL(1)
}

init {
    pstate[0] = P_READY;
    pstate[1] = P_SLEEPING;  /* writer not ready yet — forces BSP idle path */
    run bsp();
    run ap()
}
