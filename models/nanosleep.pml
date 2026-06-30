/*
 * Nanosleep BKL livelock model — full sigrestart scenario.
 *
 * Parent: nanosleep(2s). Expects EINTR from child's signal.
 * Child: msleep(50ms), then kill(parent, SIGUSR1).
 *
 * BUGGY nanosleep: busy-loops with sched_yield, BKL held.
 * Child on AP can't acquire BKL until parent's sleep finishes.
 * Parent never sees the signal during sleep (child can't send it).
 * Parent completes full 2s sleep instead of being interrupted.
 *
 * FIXED nanosleep: uses sched_sleep (releases BKL via idle stack).
 * Child can acquire BKL, run, send signal. Parent wakes with EINTR.
 */

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[2];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

byte ticks = 0;
#define PARENT_TARGET 5    /* parent sleeps until tick 5 */
#define CHILD_TARGET  2    /* child sleeps until tick 2, then signals */

bool sig_pending = false;
bool parent_got_eintr = false;
bool child_sent_signal = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* Timer tick: happens regardless of BKL (kernel-mode ISR) */
inline TICK() {
    ticks++
}

/* ═══════════════════════════════════════════════════════
 * BUGGY VERSION: nanosleep busy-loops with BKL held
 * ═══════════════════════════════════════════════════════ */

proctype parent_buggy() {
    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };
    BKL_ACQ(0);

    /* nanosleep: busy-loop */
    do
    :: ticks < PARENT_TARGET && !sig_pending ->
        /* sched_yield: BKL held, search, return */
        skip;
        TICK()
    :: sig_pending ->
        /* Signal pending: return EINTR */
        parent_got_eintr = true;
        break
    :: ticks >= PARENT_TARGET ->
        break
    od;

    BKL_REL(0)
}

proctype child_buggy() {
    atomic { pstate[1] = P_READY; cur[1] = 255 };
    BKL_ACQ(1);

    /* Schedule child on AP */
    pstate[1] = P_RUNNING; cur[1] = 1;
    BKL_REL(1);

    /* Child: msleep(50ms) — also busy-loops with BKL! */
    BKL_ACQ(1);
    do
    :: ticks < CHILD_TARGET ->
        skip;
        TICK()
    :: else -> break
    od;

    /* kill(parent, SIGUSR1) */
    sig_pending = true;
    if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;
    child_sent_signal = true;

    BKL_REL(1)
}

/* ═══════════════════════════════════════════════════════
 * FIXED VERSION: nanosleep uses sched_sleep (releases BKL)
 * ═══════════════════════════════════════════════════════ */

proctype parent_fixed() {
    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };
    BKL_ACQ(0);

    /* FIXED nanosleep: sleep loop that releases BKL */
    byte _piter = 0;
    do
    :: _piter < 6 ->
        _piter++;

        /* Check signals */
        if
        :: sig_pending ->
            parent_got_eintr = true;
            break
        :: else -> skip
        fi;

        /* Check ticks */
        if
        :: ticks >= PARENT_TARGET -> break
        :: else -> skip
        fi;

        /* sched_sleep: set SLEEPING, context_switch to idle, release BKL */
        pstate[0] = P_SLEEPING;
        cur[0] = 255;
        BKL_REL(0);

        /* Sleeping on idle stack. Timer advances ticks.
         * Other CPU can: send signal (sets READY), or timer wakes us. */
        TICK();

        /* Woken (by timer or signal wakeup setting us READY) */
        BKL_ACQ(0);
        pstate[0] = P_RUNNING;
        cur[0] = 0

    :: else -> break
    od;

    BKL_REL(0)
}

proctype child_fixed() {
    atomic { pstate[1] = P_READY; cur[1] = 255 };
    BKL_ACQ(1);

    /* Schedule child on AP */
    pstate[1] = P_RUNNING; cur[1] = 1;
    BKL_REL(1);

    /* Child: msleep — also uses sched_sleep */
    BKL_ACQ(1);
    pstate[1] = P_SLEEPING;
    cur[1] = 255;
    BKL_REL(1);
    skip;  /* sleep done */

    /* kill(parent, SIGUSR1) */
    BKL_ACQ(1);
    sig_pending = true;
    if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;
    child_sent_signal = true;
    BKL_REL(1)
}

/* ═══════════════════════════════════════════════════════ */

/* Uncomment ONE pair to test: */

/* Toggle BUGGY vs FIXED by commenting out one pair */

/* BUGGY: */
init {
    run parent_buggy();
    run child_buggy()
}

/* FIXED:
init {
    run parent_fixed();
    run child_fixed()
}
*/

/* If child sends signal, parent must eventually see it */
ltl signal_not_lost { [] (child_sent_signal -> <> (parent_got_eintr || ticks >= PARENT_TARGET)) }

/* Child must eventually send signal */
ltl child_signals { <> child_sent_signal }
