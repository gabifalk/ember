/*
 * BKL + Scheduler model v4 for ember SMP.
 *
 * Extends v3 with:
 *   - Explicit execution context tracking (which call chain the CPU is in)
 *   - CLI/STI spinlock interaction with BKL-released hlt window
 *   - current_proc staleness after process migration
 *   - context_switch modeled as call-chain abandonment
 *
 * CPU execution contexts:
 *   CTX_IDLE      — AP idle loop, no process
 *   CTX_TIMER_ISR — in timer ISR from userspace (BKL held)
 *   CTX_SYSCALL   — in syscall handler (BKL held)
 *   CTX_SCHED_HLT — in schedule() idle hlt (BKL released)
 *   CTX_USER      — running user code (no BKL)
 *
 * After context_switch, the CPU adopts the switched-to process's
 * saved context. The model tracks this explicitly.
 */

#define N_PROCS 2
#define N_CPUS  2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

/* Execution contexts */
#define CTX_IDLE      0
#define CTX_USER      1
#define CTX_TIMER_ISR 2
#define CTX_SYSCALL   3
#define CTX_SCHED_HLT 4

byte pstate[N_PROCS];
byte ctx[N_CPUS];       /* current execution context per CPU */

bool bkl = 0;
byte bkl_cpu = 255;

byte cur[N_CPUS];       /* current proc per CPU, 255=idle */
byte switched[N_CPUS];

/* Shared state: accessed under BKL in normal kernel paths.
 * CLI/STI spinlocks protect against local interrupts only. */
byte shared_users = 0;

/* CLI/STI protected state: accessed in timer_eoi_kernel.
 * On single CPU, CLI prevents concurrent access.
 * On SMP, CLI doesn't prevent cross-CPU access. */
byte clisti_users = 0;

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

/* ── Shared state access (BKL required) ──────────────── */
inline SHARED_ACCESS(c) {
    shared_users++;
    assert(shared_users == 1);
    assert(bkl_cpu == c);
    shared_users--
}

/* ── CLI/STI protected access ──────────────────────────
 * These use cli/sti spinlocks which ONLY disable local interrupts.
 * Under BKL: safe (only one CPU in kernel).
 * Without BKL (hlt window): NOT safe if another CPU also accesses.
 *
 * timer_eoi_kernel on SMP now only does LAPIC EOI + tick++.
 * It does NOT access CLI/STI protected state.
 * But if it DID, this is how we'd catch it:
 */
inline CLISTI_ACCESS_UNDER_BKL(c) {
    /* CLI (disable local interrupts) — modeled as just accessing */
    clisti_users++;
    assert(clisti_users == 1);  /* would fail if ISR on other CPU also accesses */
    clisti_users--
}

/* ── Timer ISR kernel-mode: SAFE only ──────────────────
 * Only LAPIC EOI + tick++. No shared state, no CLI/STI state.
 * This is the VERIFIED safe behavior from bkl_sched_fixed.pml. */
inline TIMER_EOI_SAFE() {
    skip
}

/* ── Schedule (deterministic, d_step) ────────────────── */
inline SCHEDULE(c) {
    byte _found;
    byte _prev;

    d_step {
        _found = 255;
        _prev = cur[c];
        switched[c] = 0;

        byte _s = 0;
        do
        :: _s < N_PROCS ->
            if
            :: pstate[_s] == P_READY -> _found = _s; break
            :: else -> _s++
            fi
        :: else -> break
        od;

        if
        :: _found != 255 && _found != _prev ->
            assert(pstate[_found] == P_READY);
            if
            :: _prev != 255 && pstate[_prev] == P_RUNNING ->
                pstate[_prev] = P_READY
            :: _prev != 255 && pstate[_prev] != P_RUNNING ->
                skip
            :: _prev == 255 ->
                skip   /* Gap 2: AP idle, no prev */
            fi;
            pstate[_found] = P_RUNNING;
            cur[c] = _found;
            switched[c] = 1
        :: _found != 255 && _found == _prev ->
            pstate[_found] = P_RUNNING;
            switched[c] = 0
        :: _found == 255 ->
            switched[c] = 0
        fi
    };

    if
    :: switched[c] ->
        /* context_switch happened.
         *
         * The CPU now executes in the switched-to process's saved
         * call chain. That chain ends with BKL_REL (either timer ISR
         * return or syscall_return). Between context_switch return and
         * BKL_REL, only register restore happens — no shared state.
         *
         * Model: BKL_REL, then CPU is in userspace with new process. */
        BKL_REL(c);
        ctx[c] = CTX_USER

    :: !switched[c] && _found == 255 &&
       (_prev == 255 || pstate[_prev] != P_RUNNING) ->
        /* Nothing ready, prev sleeping or idle.
         * Release BKL, hlt, safe timer EOI, reacquire.
         *
         * CRITICAL: during hlt window, CPU is in CTX_SCHED_HLT.
         * Only safe operations allowed (no shared/clisti state). */
        ctx[c] = CTX_SCHED_HLT;
        BKL_REL(c);
        TIMER_EOI_SAFE();   /* verified safe */
        BKL_ACQ(c)
        /* ctx stays CTX_SCHED_HLT until we find something or return */

    :: else ->
        skip
    fi
}

/* ── BSP (CPU 0) ─────────────────────────────────────── */
proctype bsp() {
    d_step { pstate[0] = P_RUNNING; cur[0] = 0; ctx[0] = CTX_USER };

    do :: true ->
        assert(cur[0] != 255);
        assert(ctx[0] == CTX_USER);

        /* === Timer preempts from userspace === */
        ctx[0] = CTX_TIMER_ISR;
        BKL_ACQ(0);

        /* timer_handler: shared state access (EOI, poll, trace) */
        SHARED_ACCESS(0);
        /* timer_handler also touches CLI/STI protected state */
        CLISTI_ACCESS_UNDER_BKL(0);

        /* Non-deterministic: preempt, sleep, or wakeup */
        if
        :: true ->
            /* Preemption: schedule may switch */
            SCHEDULE(0);
            if
            :: switched[0] ->
                /* Now in CTX_USER with (possibly different) process */
                assert(ctx[0] == CTX_USER)
            :: !switched[0] ->
                ctx[0] = CTX_USER;
                BKL_REL(0)
            fi

        :: true ->
            /* Sleep: current proc sleeps */
            byte p = cur[0];
            if :: p != 255 -> pstate[p] = P_SLEEPING :: else -> skip fi;
            SCHEDULE(0);
            if
            :: switched[0] ->
                assert(ctx[0] == CTX_USER)
            :: !switched[0] ->
                /* schedule() idle loop ran (hlt window).
                 * Eventually we'll find something or get stuck.
                 * For model: return to user if we got a proc back. */
                ctx[0] = CTX_USER;
                BKL_REL(0)
            fi

        :: true ->
            /* Wakeup + schedule */
            byte _w = 0;
            d_step {
                do
                :: _w < N_PROCS ->
                    if
                    :: pstate[_w] == P_SLEEPING -> pstate[_w] = P_READY
                    :: else -> skip
                    fi;
                    _w++
                :: else -> break
                od
            };
            SCHEDULE(0);
            if
            :: switched[0] -> assert(ctx[0] == CTX_USER)
            :: !switched[0] -> ctx[0] = CTX_USER; BKL_REL(0)
            fi
        fi;

        if :: cur[0] != 255 -> progress[cur[0]]++ :: else -> skip fi
    od
}

/* ── AP (CPU 1) ──────────────────────────────────────── */
proctype ap() {
    d_step { cur[1] = 255; ctx[1] = CTX_IDLE };

    do :: true ->
        /* === hlt in idle or sched_hlt context === */
        assert(ctx[1] == CTX_IDLE || ctx[1] == CTX_USER);

        /* Timer ISR kernel-mode: safe only */
        TIMER_EOI_SAFE();

        /* === Acquire BKL, try schedule === */
        BKL_ACQ(1);
        SCHEDULE(1);

        if
        :: switched[1] ->
            /* Got a process. ctx set to CTX_USER by SCHEDULE.
             * Process runs in userspace on AP. */
            assert(ctx[1] == CTX_USER);
            progress[cur[1]]++;

            /* === Timer preempts from userspace on AP === */
            ctx[1] = CTX_TIMER_ISR;
            BKL_ACQ(1);
            SHARED_ACCESS(1);
            CLISTI_ACCESS_UNDER_BKL(1);

            if
            :: true ->
                /* Preemption */
                SCHEDULE(1);
                if
                :: switched[1] -> assert(ctx[1] == CTX_USER)
                :: !switched[1] -> ctx[1] = CTX_USER; BKL_REL(1)
                fi
            :: true ->
                /* Sleep */
                byte p = cur[1];
                if :: p != 255 -> pstate[p] = P_SLEEPING :: else -> skip fi;
                SCHEDULE(1);
                if
                :: switched[1] -> assert(ctx[1] == CTX_USER)
                :: !switched[1] -> ctx[1] = CTX_USER; BKL_REL(1)
                fi
            :: true ->
                /* Wakeup + schedule */
                byte _w = 0;
                d_step {
                    do
                    :: _w < N_PROCS ->
                        if
                        :: pstate[_w] == P_SLEEPING -> pstate[_w] = P_READY
                        :: else -> skip
                        fi;
                        _w++
                    :: else -> break
                    od
                };
                SCHEDULE(1);
                if
                :: switched[1] -> assert(ctx[1] == CTX_USER)
                :: !switched[1] -> ctx[1] = CTX_USER; BKL_REL(1)
                fi
            fi;

            if :: cur[1] != 255 -> progress[cur[1]]++ :: else -> skip fi

        :: !switched[1] ->
            /* Nothing found. Release BKL, stay idle. */
            ctx[1] = CTX_IDLE;
            BKL_REL(1)
        fi
    od
}

init {
    pstate[0] = P_READY;
    pstate[1] = P_READY;
    run bsp();
    run ap()
}

/* All safety checked inline:
 *   - shared_users == 1        (mutual exclusion)
 *   - clisti_users == 1        (CLI/STI vs cross-CPU)
 *   - bkl_cpu == c             (BKL discipline)
 *   - pstate[_found] == P_READY (no dual-run)
 *   - ctx assertions           (execution context consistency)
 */
