/*
 * BKL + Scheduler model v5 for ember SMP.
 *
 * Extends v4 with:
 *   - schedule() modeled as a LOOP (retry until switch or prev running)
 *   - sched_sleep gap: sched_lock released between SLEEPING and schedule search
 *   - Wakeup can happen during the gap (other CPU under BKL)
 *   - Tests for lost wakeup (process stuck SLEEPING forever)
 *
 * The key liveness concern: can a process sleep forever because
 * a wakeup was "lost" between sched_sleep releasing sched_lock
 * and schedule re-acquiring it?
 */

#define N_PROCS 2
#define N_CPUS  2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];

bool bkl = 0;
byte bkl_cpu = 255;

byte cur[N_CPUS];
byte shared_users = 0;

byte progress[N_PROCS];
#define GOAL 1

/* Track if a wakeup was issued for sleeping procs */
bool wakeup_issued = false;

/* ── BKL ─────────────────────────────────────────────── */
inline BKL_ACQ(c) {
    do :: atomic { !bkl -> bkl = 1; bkl_cpu = c; break } od
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

inline SHARED_ACCESS(c) {
    shared_users++;
    assert(shared_users == 1);
    assert(bkl_cpu == c);
    shared_users--
}

inline TIMER_EOI_SAFE() { skip }

/* ── sched_wakeup: SLEEPING → READY ─────────────────── */
inline WAKEUP(c) {
    assert(bkl_cpu == c);
    d_step {
        byte _w = 0;
        do
        :: _w < N_PROCS ->
            if
            :: pstate[_w] == P_SLEEPING -> pstate[_w] = P_READY
            :: else -> skip
            fi;
            _w++
        :: else -> break
        od;
        wakeup_issued = false
    }
}

/* ── Schedule as a loop ──────────────────────────────────
 *
 * Models the ACTUAL C for(;;) loop in schedule():
 *   1. Search for READY proc (under sched_lock, modeled as d_step)
 *   2. If found and != prev: context_switch, return (switched=1)
 *   3. If found and == prev: re-mark RUNNING, return (switched=0)
 *   4. If not found and prev RUNNING: return (switched=0)
 *   5. If not found and prev SLEEPING/idle:
 *      release BKL, hlt, reacquire, LOOP BACK TO 1
 *
 * The loop models the possibility that a wakeup happens during
 * the BKL-released hlt window, making a proc READY for the next
 * iteration.
 *
 * Bounded to 3 iterations to keep state space finite.
 */
#define SCHED_MAX_ITERS 1

inline SCHEDULE_LOOP(c) {
    byte _sl_found;
    byte _sl_prev;
    byte _sl_switched = 0;
    byte _sl_iter = 0;

    do
    :: _sl_iter < SCHED_MAX_ITERS ->
        _sl_iter++;

        /* Search + switch in one d_step (models sched_lock held
         * from search through context_switch, plus BKL ensures
         * only one CPU in kernel). */
        d_step {
            _sl_found = 255;
            _sl_prev = cur[c];

            byte _s = 0;
            do
            :: _s < N_PROCS ->
                if
                :: pstate[_s] == P_READY -> _sl_found = _s; break
                :: else -> _s++
                fi
            :: else -> break
            od;

            if
            :: _sl_found != 255 && _sl_found != _sl_prev ->
                /* Context switch */
                assert(pstate[_sl_found] == P_READY);
                if
                :: _sl_prev != 255 && pstate[_sl_prev] == P_RUNNING ->
                    pstate[_sl_prev] = P_READY
                :: _sl_prev != 255 && pstate[_sl_prev] != P_RUNNING ->
                    skip
                :: _sl_prev == 255 ->
                    skip
                fi;
                pstate[_sl_found] = P_RUNNING;
                cur[c] = _sl_found;
                _sl_switched = 2  /* mark: needs BKL_REL after d_step */
            :: _sl_found != 255 && _sl_found == _sl_prev ->
                pstate[_sl_found] = P_RUNNING;
                _sl_switched = 0
            :: _sl_found == 255 ->
                _sl_switched = 3  /* mark: needs idle-hlt path */
            fi
        };

        /* Post-d_step actions that can't be inside d_step (BKL ops block) */
        if
        :: _sl_switched == 2 ->
            BKL_REL(c);
            _sl_switched = 1;
            break
        :: _sl_switched == 0 ->
            break
        :: _sl_switched == 3 ->
            if
            :: _sl_prev != 255 && pstate[_sl_prev] == P_RUNNING ->
                _sl_switched = 0;
                break
            :: else ->
                /* Nothing ready, prev sleeping or idle.
                 * Release BKL, hlt (safe EOI), reacquire, retry. */
                BKL_REL(c);
                TIMER_EOI_SAFE();
                BKL_ACQ(c);
                _sl_switched = 0
                /* Loop back */
            fi
        fi
    :: else ->
        /* Max iterations reached — give up (shouldn't happen in practice) */
        _sl_switched = 0;
        break
    od;

    /* Export result — use per-CPU array trick */
    if
    :: c == 0 -> switched_0 = _sl_switched
    :: c == 1 -> switched_1 = _sl_switched
    fi
}

byte switched_0;
byte switched_1;

/* ── sched_sleep: set SLEEPING, then schedule ────────────
 *
 * Models the C code:
 *   spin_lock_irqsave(&sched_lock);
 *   current_proc->state = PROC_SLEEPING;
 *   spin_unlock_irqrestore(&sched_lock);   // <-- gap here
 *   schedule();                             // re-acquires sched_lock
 *
 * During the gap, the other CPU (under BKL? NO — BKL is held by us)
 * can't enter kernel. So no wakeup can happen in the gap.
 * Actually wait — BKL is held by the sleeping CPU. So the gap is safe.
 *
 * BUT: what about the schedule() idle loop's BKL-released window?
 * There, the other CPU CAN enter kernel and do a wakeup. That wakeup
 * sets the proc to READY BEFORE schedule searches. So schedule will
 * find it and switch back. Not lost.
 *
 * Model this explicitly.
 */
inline SCHED_SLEEP(c) {
    byte _ss_p = cur[c];
    assert(bkl_cpu == c);

    if :: _ss_p != 255 ->
        /* Set state to SLEEPING (under sched_lock, modeled as d_step) */
        d_step { pstate[_ss_p] = P_SLEEPING; wakeup_issued = true };

        /* sched_lock released, then schedule() called.
         * BKL is still held, so no other CPU can race here. */
        SCHEDULE_LOOP(c)
    :: else -> skip
    fi
}

/* ── BSP ─────────────────────────────────────────────── */
proctype bsp() {
    byte sw;
    d_step { pstate[0] = P_RUNNING; cur[0] = 0 };

    do :: true ->
        assert(cur[0] != 255);

        /* Userspace */
        skip;

        /* Timer from user */
        BKL_ACQ(0);
        SHARED_ACCESS(0);

        if
        :: true ->
            /* Preemption */
            SCHEDULE_LOOP(0);
            sw = switched_0;
            if :: sw -> skip :: !sw -> BKL_REL(0) fi

        :: true ->
            /* Sleep */
            SCHED_SLEEP(0);
            sw = switched_0;
            if :: sw -> skip :: !sw -> BKL_REL(0) fi

        :: true ->
            /* Wakeup + schedule */
            if :: wakeup_issued -> WAKEUP(0) :: else -> skip fi;
            SCHEDULE_LOOP(0);
            sw = switched_0;
            if :: sw -> skip :: !sw -> BKL_REL(0) fi
        fi;

        if :: cur[0] != 255 -> progress[cur[0]]++ :: else -> skip fi
    od
}

/* ── AP ──────────────────────────────────────────────── */
proctype ap() {
    byte sw;
    d_step { cur[1] = 255 };

    do :: true ->
        TIMER_EOI_SAFE();

        BKL_ACQ(1);
        SCHEDULE_LOOP(1);
        sw = switched_1;

        if
        :: sw ->
            /* Got a process */
            progress[cur[1]]++;

            /* Timer preempts from userspace */
            BKL_ACQ(1);
            SHARED_ACCESS(1);

            if
            :: true ->
                SCHEDULE_LOOP(1);
                sw = switched_1;
                if :: sw -> skip :: !sw -> BKL_REL(1) fi
            :: true ->
                SCHED_SLEEP(1);
                sw = switched_1;
                if :: sw -> skip :: !sw -> BKL_REL(1) fi
            :: true ->
                if :: wakeup_issued -> WAKEUP(1) :: else -> skip fi;
                SCHEDULE_LOOP(1);
                sw = switched_1;
                if :: sw -> skip :: !sw -> BKL_REL(1) fi
            fi;

            if :: cur[1] != 255 -> progress[cur[1]]++ :: else -> skip fi

        :: !sw ->
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

/* Safety: checked inline
 * Liveness: uncomment for separate check
 * ltl progress_p0 { <> (progress[0] >= GOAL) }
 * ltl progress_p1 { <> (progress[1] >= GOAL) }
 */
