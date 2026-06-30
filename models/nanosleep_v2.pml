/*
 * Nanosleep model v2 — tick wakeup problem.
 *
 * Problem: sched_wakeup(SCHED_TICK_CHAN) is called in timer_handler
 * (user-mode timer ISR, under BKL). But when all processes are
 * SLEEPING, there's no user-mode timer — only kernel-mode timer
 * (timer_eoi_kernel), which can't call sched_wakeup under SMP
 * (runs without BKL).
 *
 * Models three variants:
 *   BUGGY:  tick wakeup only in timer_handler (user-mode)
 *   FIX_A:  tick wakeup in schedule() idle loop after BKL reacquire
 *   FIX_B:  tick wakeup in timer_eoi_kernel (needs lock-free wakeup)
 *
 * 2 procs: parent (nanosleep), child (msleep then signal).
 */

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[2];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

byte ticks = 0;
#define PARENT_TARGET 4
#define CHILD_TARGET  1

bool sig_pending = false;
bool child_sent_signal = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* Wake all TICK_CHAN sleepers */
inline TICK_WAKEUP() {
    atomic {
        if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;
        if :: pstate[1] == P_SLEEPING -> pstate[1] = P_READY :: else -> skip fi
    }
}

/* Schedule: find READY, switch */
byte _sf;
inline SCHED(c) {
    atomic {
        _sf = 255;
        if :: pstate[0] == P_READY && cur[c] != 0 -> _sf = 0
           :: pstate[1] == P_READY && cur[c] != 1 -> _sf = 1
           :: else -> skip
        fi;
        if
        :: _sf != 255 ->
            byte _old = cur[c];
            if :: _old != 255 && pstate[_old] == P_RUNNING -> pstate[_old] = P_READY
               :: _old != 255 && pstate[_old] != P_RUNNING -> skip
               :: _old == 255 -> skip
            fi;
            pstate[_sf] = P_RUNNING; cur[c] = _sf
        :: else -> skip
        fi
    }
}

/* ═══════════════════════════════════════════════════════
 * Parent: nanosleep using sched_sleep
 * ═══════════════════════════════════════════════════════ */
proctype parent() {
    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };
    BKL_ACQ(0);

    byte _pi = 0;
    do
    :: _pi < 8 ->
        _pi++;
        if :: sig_pending -> break :: else -> skip fi;
        if :: ticks >= PARENT_TARGET -> break :: else -> skip fi;

        /* sched_sleep: set SLEEPING, schedule */
        pstate[0] = P_SLEEPING;
        cur[0] = 255;  /* idle stack */
        /* schedule() finds child or idles */
        SCHED(0);

        if
        :: cur[0] != 255 ->
            /* Switched to child on BSP — child runs.
             * When child sleeps, schedule switches back to idle.
             * Model: BKL released through child's return path. */
            BKL_REL(0);
            skip;  /* child runs, eventually sleeps */
            /* Now both sleeping. Idle loop. */
            BKL_ACQ(0);
            /* FIX_A: tick wakeup after BKL reacquire in idle loop */
            ticks++;
            TICK_WAKEUP();
            /* Re-search */
            SCHED(0);
            if :: cur[0] == 0 -> pstate[0] = P_RUNNING :: else -> skip fi
        :: else ->
            /* Nothing found. Idle hlt. */
            BKL_REL(0);
            ticks++;  /* timer_eoi_kernel increments */
            BKL_ACQ(0);
            /* FIX_A: tick wakeup after BKL reacquire */
            TICK_WAKEUP();
            SCHED(0);
            if :: cur[0] == 0 -> pstate[0] = P_RUNNING :: else -> skip fi
        fi
    :: else -> break
    od;

    if :: bkl_cpu == 0 -> BKL_REL(0) :: else -> skip fi
}

/* ═══════════════════════════════════════════════════════
 * Child: msleep then signal parent
 * ═══════════════════════════════════════════════════════ */
proctype child() {
    atomic { pstate[1] = P_READY; cur[1] = 255 };

    /* Wait to be scheduled (by parent's schedule or AP's idle) */
    BKL_ACQ(1);
    if :: pstate[1] == P_READY -> pstate[1] = P_RUNNING; cur[1] = 1
       :: else -> skip
    fi;
    BKL_REL(1);

    /* msleep: also sched_sleep */
    BKL_ACQ(1);
    byte _ci = 0;
    do
    :: _ci < 4 ->
        _ci++;
        if :: ticks >= CHILD_TARGET -> break :: else -> skip fi;
        pstate[1] = P_SLEEPING;
        cur[1] = 255;
        BKL_REL(1);
        ticks++;
        BKL_ACQ(1);
        TICK_WAKEUP();
        SCHED(1);
        if :: cur[1] == 1 -> pstate[1] = P_RUNNING :: else -> skip fi
    :: else -> break
    od;

    /* kill(parent, SIGUSR1) */
    sig_pending = true;
    if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;
    child_sent_signal = true;

    if :: bkl_cpu == 1 -> BKL_REL(1) :: else -> skip fi
}

init {
    run parent();
    run child()
}

/* If child sends signal while parent sleeping, parent must see it */
ltl signal_not_lost { [] (child_sent_signal -> <> (sig_pending == false || ticks >= PARENT_TARGET)) }

/* Child must eventually send signal */
ltl child_signals { <> child_sent_signal }
