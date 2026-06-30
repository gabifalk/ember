/*
 * BKL + Scheduler model for ember SMP — FIXED version.
 *
 * Fix: timer ISR kernel-mode path does ONLY safe operations:
 *   - LAPIC EOI (hardware register, CPU-local)
 *   - kernel_ticks++ (atomic, read-only by other CPUs)
 * NO console_poll_signals, NO trace_dump, NO current_proc access.
 *
 * The schedule() idle loop no longer has an unsafe shared-state
 * access window, because the timer ISR that fires during the
 * BKL-released hlt doesn't access shared state.
 */

#define N_PROCS 2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];
byte pcpu[N_PROCS];

bool bkl = 0;
byte bkl_cpu = 255;

byte cur[2];

byte shared_users = 0;

byte done[N_PROCS];
#define GOAL 3

/* ── BKL ops ─────────────────────────────────────────── */
inline BKL_ACQ(c) {
    do :: atomic { !bkl -> bkl=1; bkl_cpu=c; break } od
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu==c);
    bkl_cpu=255; bkl=0
}

/* Shared state access under BKL */
inline SHARED_ACCESS(c) {
    shared_users++;
    assert(shared_users == 1);
    assert(bkl_cpu == c);
    skip;
    shared_users--
}

/* Timer ISR kernel-mode: SAFE operations only.
 * No shared state access. Just EOI + tick. */
inline TIMER_EOI_SAFE() {
    /* LAPIC EOI: CPU-local MMIO write, no shared state */
    /* kernel_ticks++: atomic increment, safe without lock */
    skip
}

/* ── Schedule ────────────────────────────────────────── */
inline SCHEDULE(c) {
    byte _found = 255;
    byte _prev = cur[c];

    atomic {
        byte _s = 0;
        do
        :: _s < N_PROCS ->
            if
            :: pstate[_s] == P_READY -> _found = _s; break
            :: else -> _s++
            fi
        :: else -> break
        od
    };

    if
    :: _found != 255 ->
        atomic {
            if
            :: _prev != 255 && pstate[_prev] == P_RUNNING ->
                pstate[_prev] = P_READY
            :: else -> skip
            fi;
            pstate[_found] = P_RUNNING;
            pcpu[_found] = c;
            cur[c] = _found
        }
    :: _found == 255 && _prev != 255 && pstate[_prev] == P_RUNNING ->
        /* prev running, nothing else — return */
        skip
    :: _found == 255 && (_prev == 255 || pstate[_prev] != P_RUNNING) ->
        /* Nothing ready, prev sleeping or idle.
         * Release BKL, hlt, timer fires (safe EOI only), reacquire. */
        BKL_REL(c);
        TIMER_EOI_SAFE();   /* safe: no shared state */
        BKL_ACQ(c)
    fi
}

/* ── BSP ─────────────────────────────────────────────── */
proctype bsp() {
    atomic {
        pstate[0] = P_RUNNING;
        pcpu[0] = 0;
        cur[0] = 0
    };

    do :: true ->
        byte p = cur[0];
        assert(p != 255);

        /* User space */
        skip;

        /* Timer from user: acquire BKL, work, schedule, release */
        BKL_ACQ(0);
        SHARED_ACCESS(0);
        SCHEDULE(0);
        BKL_REL(0);

        p = cur[0];
        if :: p != 255 -> done[p]++ :: else -> skip fi
    od
}

/* ── AP ──────────────────────────────────────────────── */
proctype ap() {
    cur[1] = 255;

    do :: true ->
        /* hlt: timer ISR kernel-mode does SAFE EOI only */
        TIMER_EOI_SAFE();

        /* AP idle loop: acquire BKL, schedule, release */
        BKL_ACQ(1);

        if
        :: cur[1] == 255 ->
            SCHEDULE(1);

            if
            :: cur[1] != 255 ->
                /* Got a proc. BKL held. Release (return through ISR path). */
                BKL_REL(1);

                /* User space on AP */
                done[cur[1]]++;

                /* Timer preempts on AP (user mode) */
                BKL_ACQ(1);
                SHARED_ACCESS(1);
                SCHEDULE(1);
                BKL_REL(1);

                if :: cur[1] != 255 -> done[cur[1]]++ :: else -> skip fi
            :: else ->
                BKL_REL(1)
            fi
        :: else ->
            BKL_REL(1)
        fi
    od
}

init {
    byte i = 1;
    do
    :: i < N_PROCS -> pstate[i] = P_READY; i++
    :: else -> break
    od;
    run bsp();
    run ap()
}

/* Safety: mutual exclusion (checked inline) */
/* No shared_access_violation needed — UNSAFE_SHARED_ACCESS removed */

/* Liveness */
ltl progress_p0 { <> (done[0] >= GOAL) }
ltl progress_p1 { <> (done[1] >= GOAL) }
