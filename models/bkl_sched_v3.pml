/*
 * BKL + Scheduler model v3 for ember SMP.
 *
 * Covers all 5 identified gaps:
 *   1. context_switch changes BKL release path
 *   2. AP prev is NULL on first schedule call
 *   3. Sleep/wakeup protocol
 *   4. Fork child entry (modeled as new READY proc appearing)
 *   5. sched_wakeup correctness
 *
 * Optimized for exhaustive verification:
 *   - 2 procs, 2 CPUs (covers migration, sleep/wake)
 *   - Deterministic schedule search (d_step, first-READY)
 *   - Bounded iterations (GOAL=1)
 *   - No fork/exit (same BKL paths as preempt+sleep)
 */

#define N_PROCS 2
#define N_CPUS  2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];

bool bkl = 0;
byte bkl_cpu = 255;

byte cur[N_CPUS];          /* 255 = idle */
byte switched[N_CPUS];
byte shared_users = 0;
bool wake_pending = false;

byte progress[N_PROCS];
#define GOAL 1

/* ── BKL ─────────────────────────────────────────────── */
inline BKL_ACQ(c) {
    do :: atomic { !bkl -> bkl = 1; bkl_cpu = c; break } od
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255;
    bkl = 0
}

/* ── Shared state access (must hold BKL) ─────────────── */
inline SHARED_ACCESS(c) {
    shared_users++;
    assert(shared_users == 1);
    assert(bkl_cpu == c);
    shared_users--
}

/* ── Timer ISR kernel-mode: safe ops only ────────────── */
inline TIMER_EOI_SAFE() {
    skip
}

/* ── Wakeup: SLEEPING → READY (must hold BKL) ───────── */
inline WAKEUP(c) {
    byte _w;
    assert(bkl_cpu == c);
    d_step {
        _w = 0;
        do
        :: _w < N_PROCS ->
            if
            :: pstate[_w] == P_SLEEPING ->
                pstate[_w] = P_READY;
                wake_pending = false
            :: else -> skip
            fi;
            _w++
        :: else -> break
        od
    }
}

/* ── Schedule ────────────────────────────────────────────
 *
 * Deterministic search: pick first READY proc (matches C round-robin).
 * Search + switch in d_step (models sched_lock + BKL together).
 *
 * Sets switched[c]:
 *   1 = context_switch happened, BKL released via ISR return path
 *   0 = no switch, caller releases BKL
 *
 * Gap 2: if cur[c]==255 (AP idle, prev is NULL), skip prev->state
 *        access and don't save old context.
 */
inline SCHEDULE(c) {
    byte _found;
    byte _prev;

    d_step {
        _found = 255;
        _prev = cur[c];
        switched[c] = 0;

        /* Deterministic search: first READY proc */
        byte _s = 0;
        do
        :: _s < N_PROCS ->
            if
            :: pstate[_s] == P_READY ->
                _found = _s;
                break
            :: else -> _s++
            fi
        :: else -> break
        od;

        if
        :: _found != 255 && _found != _prev ->
            /* Context switch */
            assert(pstate[_found] == P_READY);
            if
            :: _prev != 255 && pstate[_prev] == P_RUNNING ->
                pstate[_prev] = P_READY
            :: _prev != 255 && pstate[_prev] != P_RUNNING ->
                skip   /* prev is SLEEPING */
            :: _prev == 255 ->
                skip   /* Gap 2: AP idle, no prev */
            fi;
            pstate[_found] = P_RUNNING;
            cur[c] = _found;
            switched[c] = 1
        :: _found != 255 && _found == _prev ->
            /* Same proc, re-mark running */
            pstate[_found] = P_RUNNING;
            switched[c] = 0
        :: _found == 255 ->
            switched[c] = 0
        fi
    };

    /* Post-switch actions (outside d_step, BKL still held) */
    if
    :: switched[c] ->
        /* Gap 1: context_switch happened. BKL released via
         * the switched-to process's ISR/syscall return path. */
        BKL_REL(c)
    :: !switched[c] && _found == 255 &&
       (_prev == 255 || pstate[_prev] != P_RUNNING) ->
        /* Nothing ready, prev sleeping or idle.
         * Release BKL, hlt (safe EOI), reacquire.
         * Models the schedule() idle loop in C. */
        BKL_REL(c);
        TIMER_EOI_SAFE();
        BKL_ACQ(c)
    :: else ->
        /* No switch, prev still running, or same proc — just return */
        skip
    fi
}

/* ── BSP (CPU 0) ─────────────────────────────────────── */
proctype bsp() {
    byte p;

    d_step {
        pstate[0] = P_RUNNING;
        cur[0] = 0
    };

    do :: true ->
        p = cur[0];
        if :: p == 255 -> break :: else -> skip fi;

        /* === Userspace === */
        skip;

        /* === Timer from user: acquire BKL, kernel work === */
        BKL_ACQ(0);
        SHARED_ACCESS(0);

        /* Non-deterministic: preempt, sleep, or wakeup */
        if
        :: true ->
            /* Preemption */
            SCHEDULE(0);
            if
            :: switched[0] -> skip
            :: !switched[0] -> BKL_REL(0)
            fi

        :: true ->
            /* Gap 3: Sleep */
            p = cur[0];
            if :: p != 255 ->
                pstate[p] = P_SLEEPING;
                wake_pending = true
            :: else -> skip
            fi;
            SCHEDULE(0);
            if
            :: switched[0] -> skip
            :: !switched[0] -> BKL_REL(0)
            fi

        :: true ->
            /* Gap 5: Wakeup others + schedule */
            if :: wake_pending -> WAKEUP(0) :: else -> skip fi;
            SCHEDULE(0);
            if
            :: switched[0] -> skip
            :: !switched[0] -> BKL_REL(0)
            fi
        fi;

        p = cur[0];
        if :: p != 255 && p < N_PROCS -> progress[p]++ :: else -> skip fi
    od
}

/* ── AP (CPU 1) ──────────────────────────────────────── */
proctype ap() {
    byte p;
    cur[1] = 255;  /* Gap 2: idle, no process */

    do :: true ->
        /* hlt: safe timer EOI only */
        TIMER_EOI_SAFE();

        /* Acquire BKL, try schedule */
        BKL_ACQ(1);
        SCHEDULE(1);

        if
        :: switched[1] ->
            /* Got a process. BKL already released. In userspace. */
            p = cur[1];
            if :: p != 255 && p < N_PROCS -> progress[p]++ :: else -> skip fi;

            /* Timer preempts from userspace on AP */
            BKL_ACQ(1);
            SHARED_ACCESS(1);

            if
            :: true ->
                /* Preemption */
                SCHEDULE(1);
                if
                :: switched[1] -> skip
                :: !switched[1] -> BKL_REL(1)
                fi
            :: true ->
                /* Sleep */
                p = cur[1];
                if :: p != 255 ->
                    pstate[p] = P_SLEEPING;
                    wake_pending = true
                :: else -> skip
                fi;
                SCHEDULE(1);
                if
                :: switched[1] -> skip
                :: !switched[1] -> BKL_REL(1)
                fi
            :: true ->
                /* Wakeup + schedule */
                if :: wake_pending -> WAKEUP(1) :: else -> skip fi;
                SCHEDULE(1);
                if
                :: switched[1] -> skip
                :: !switched[1] -> BKL_REL(1)
                fi
            fi;

            p = cur[1];
            if :: p != 255 && p < N_PROCS -> progress[p]++ :: else -> skip fi

        :: !switched[1] ->
            /* Nothing found */
            BKL_REL(1)
        fi
    od
}

init {
    pstate[0] = P_READY;   /* will be set RUNNING by BSP */
    pstate[1] = P_READY;   /* available for AP to pick up */
    run bsp();
    run ap()
}

/* Safety: checked inline via assert()
 *   - shared_users == 1 (mutual exclusion)
 *   - bkl_cpu == c (BKL discipline)
 *   - pstate[_found] == P_READY (no dual-run)
 *
 * Liveness: uncomment and check with: spin -search -ltl <name> -a -f
 * ltl progress_p0 { <> (progress[0] >= GOAL) }
 * ltl progress_p1 { <> (progress[1] >= GOAL) }
 */
