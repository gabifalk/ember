/*
 * BKL + Scheduler model for ember SMP — v2.
 *
 * Precisely models the ACTUAL C code paths:
 *
 * BSP flow:
 *   user_run → timer_isr_user → bkl_acquire → timer_handler →
 *   schedule → [context_switch | idle_loop] → bkl_release → iretq
 *
 * AP flow:
 *   idle: hlt → [timer_isr_kernel → eoi → iretq] →
 *   bkl_acquire → schedule → [context_switch | return] → bkl_release
 *
 * schedule idle loop (when nothing READY, proc sleeping):
 *   bkl_release → sti;hlt → [TIMER ISR WITHOUT BKL!] → cli → bkl_acquire
 *
 * Verifies:
 *   1. Mutual exclusion: shared state accessed only under BKL
 *   2. No deadlock
 *   3. Progress: every process eventually runs
 *   4. Timer ISR kernel-mode: detects unsafe shared state access
 */

#define N_PROCS 2   /* keep small for exhaustive search */

/* ── Process states ──────────────────────────────────── */
#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];
byte pcpu[N_PROCS];    /* which CPU runs this proc, 255=none */

/* ── BKL ─────────────────────────────────────────────── */
bool bkl = 0;
byte bkl_cpu = 255;    /* holder, 255=nobody */

/* ── Per-CPU current proc ────────────────────────────── */
byte cur[2];            /* cur[cpu]=proc idx, 255=idle */

/* ── Shared-state access tracking ────────────────────── */
byte shared_users = 0;  /* count of concurrent accessors */
bool shared_access_violation = false;

/* ── Progress tracking ───────────────────────────────── */
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

/* Access shared state — MUST be under BKL */
inline SHARED_ACCESS(c) {
    shared_users++;
    assert(shared_users == 1);  /* mutual exclusion */
    assert(bkl_cpu == c);        /* must hold BKL */
    skip;  /* do work */
    shared_users--
}

/* Unsafe shared access — called from timer ISR kernel-mode.
 * If this ever runs while another CPU holds shared state, it's a bug. */
inline UNSAFE_SHARED_ACCESS() {
    shared_users++;
    if
    :: shared_users > 1 -> shared_access_violation = true
    :: else -> skip
    fi;
    shared_users--
}

/* ── Schedule: find READY, context_switch ─────────────── */
/* When context_switch happens, the CPU continues as the new proc.
 * The old proc's saved_ksp saves the return address (after context_switch call).
 * The new proc resumes at ITS saved return address.
 *
 * KEY: after context_switch, BKL is still held. The release happens
 * when the CPU returns through the new proc's call chain. */
inline SCHEDULE(c) {
    byte _found = 255;
    byte _prev = cur[c];

    /* Find a READY process */
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
        /* Context switch */
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
        /* Nothing else ready but prev is running — just return */
        skip
    :: _found == 255 && (_prev == 255 || pstate[_prev] != P_RUNNING) ->
        /* Nothing ready, prev is sleeping or idle.
         * CURRENT C CODE: bkl_release; sti;hlt;cli; bkl_acquire; loop
         * During hlt, timer ISR fires WITHOUT BKL. */
        BKL_REL(c);

        /* --- Window without BKL --- */
        /* Timer ISR kernel-mode fires here.
         * timer_eoi_kernel accesses shared state: */
        UNSAFE_SHARED_ACCESS();

        BKL_ACQ(c);
        /* Loop back in schedule — but for model simplicity, just return.
         * The caller will retry. */
    fi
}

/* ── BSP (CPU 0) ─────────────────────────────────────── */
proctype bsp() {
    /* Start process 0 */
    atomic {
        pstate[0] = P_RUNNING;
        pcpu[0] = 0;
        cur[0] = 0
    };

    do :: true ->
        byte p = cur[0];
        assert(p != 255);  /* BSP always has a proc after boot */

        /* === User space (no BKL) === */
        skip;

        /* === Timer from user mode === */
        BKL_ACQ(0);
        SHARED_ACCESS(0);    /* timer_handler: EOI, poll, etc. */
        SCHEDULE(0);
        BKL_REL(0);

        /* === Back in user space (possibly different proc) === */
        p = cur[0];
        if :: p != 255 -> done[p]++ :: else -> skip fi
    od
}

/* ── AP (CPU 1) ──────────────────────────────────────── */
proctype ap() {
    cur[1] = 255;  /* idle */

    do :: true ->
        /* === hlt: waiting for timer === */
        /* Timer ISR kernel-mode: no BKL!
         * timer_eoi_kernel does shared state access: */
        if
        :: true -> UNSAFE_SHARED_ACCESS()  /* timer accesses shared state */
        :: true -> skip                     /* or timer only does safe EOI */
        fi;

        /* === After hlt: AP idle loop === */
        BKL_ACQ(1);

        if
        :: cur[1] == 255 ->
            /* Idle: try to pick up work */
            SCHEDULE(1);

            if
            :: cur[1] != 255 ->
                /* Got a proc via context_switch.
                 * BKL still held. Release (timer ISR return path). */
                BKL_REL(1);

                /* User space on AP */
                byte p2 = cur[1];
                done[p2]++;

                /* Timer preempts on AP (user mode): */
                BKL_ACQ(1);
                SHARED_ACCESS(1);
                SCHEDULE(1);
                BKL_REL(1);

                /* Back in user (maybe different proc or idle) */
                if :: cur[1] != 255 -> done[cur[1]]++ :: else -> skip fi
            :: else ->
                /* Nothing found */
                BKL_REL(1)
            fi
        :: else ->
            /* AP has a running proc (resumed after hlt from user timer) */
            /* This shouldn't really happen in this model flow */
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

/* Safety: no shared_access_violation */
ltl no_violation { [] (!shared_access_violation) }

/* Liveness */
ltl progress_p0 { <> (done[0] >= GOAL) }
ltl progress_p1 { <> (done[1] >= GOAL) }
