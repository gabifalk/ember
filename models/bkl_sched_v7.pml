/*
 * BKL + Scheduler model v7 — fix for stale-prev dual-run bug.
 *
 * v6 proved: after idle hlt + BKL reacquire, the stale prev->state
 * check sees PROC_RUNNING (set by other CPU) and returns, causing
 * dual-run.
 *
 * Fix: after reacquiring BKL in the idle loop, do NOT check
 * prev->state. Just re-search unconditionally. The prev->state
 * check is only valid BEFORE the first BKL release (same iteration,
 * BKL held continuously from search through check).
 *
 * In C: move the "if (prev && prev->state == PROC_RUNNING) return"
 * check to BEFORE the idle hlt path, not after.
 */

#define N_PROCS 2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

bool boot_done = false;

/* ── BSP: proc 0 sleeps, schedule with FIXED idle loop ── */
proctype bsp() {
    byte prev;

    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };

    /* BSP finishes boot, releases APs */
    boot_done = true;

    BKL_ACQ(0);
    pstate[0] = P_SLEEPING;

    /* schedule(): cache prev once */
    prev = cur[0];

    /* FIX: check prev->state BEFORE any BKL release.
     * At this point BKL is held, prev state is authoritative.
     * prev is SLEEPING (we just set it), so this returns false. */
    if
    :: prev != 255 && pstate[prev] == P_RUNNING ->
        /* prev still running, nothing to do */
        BKL_REL(0);
        goto done
    :: else -> skip
    fi;

    /* Search: nothing READY */
    if
    :: pstate[1] == P_READY ->
        pstate[1] = P_RUNNING;
        cur[0] = 1;
        BKL_REL(0);
        goto done
    :: else -> skip
    fi;

    /* FIX: clear cur[0] — we have no process now.
     * Prevents other CPU from thinking prev is still ours. */
    cur[0] = 255;

    /* Idle: release BKL, hlt, reacquire */
    BKL_REL(0);
    skip;  /* hlt */
    BKL_ACQ(0);

    /* FIX: do NOT check stale prev->state here.
     * Just re-search unconditionally. */
    if
    :: pstate[1] == P_READY ->
        pstate[1] = P_RUNNING;
        cur[0] = 1;
        BKL_REL(0);
        goto done
    :: pstate[0] == P_READY ->
        /* prev was woken by AP, now READY. Switch back to it. */
        pstate[0] = P_RUNNING;
        cur[0] = 0;
        BKL_REL(0);
        goto done
    :: else ->
        /* Still nothing. Return (will re-enter schedule from caller). */
        BKL_REL(0)
    fi;

done:
    /* Verify: no dual-run */
    assert(!(cur[0] == cur[1] && cur[1] != 255 && pstate[cur[0]] == P_RUNNING))
}

/* ── AP: wakes proc 0, schedules it ──────────────────── */
proctype ap() {
    cur[1] = 255;

    /* Wait for BSP to finish boot */
    boot_done;

    BKL_ACQ(1);
    if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;
    if
    :: pstate[0] == P_READY ->
        pstate[0] = P_RUNNING;
        cur[1] = 0
    :: else -> skip
    fi;
    BKL_REL(1);

    /* Verify: no dual-run */
    assert(!(cur[0] == cur[1] && cur[1] != 255 && pstate[cur[0]] == P_RUNNING))
}

init {
    pstate[0] = P_READY;
    pstate[1] = P_SLEEPING;
    run bsp();
    run ap()
}
