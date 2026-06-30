/*
 * Exit/wait4 + SIGSTOP/SIGCONT model for ember SMP.
 *
 * Models:
 *   do_exit: child sets ZOMBIE, sends SIGCHLD to parent, wakes parent
 *   wait4:   parent scans for ZOMBIE/STOPPED child, sleeps if none
 *   SIGSTOP: child sets PROC_STOPPED, sends SIGCHLD, schedule()
 *   SIGCONT: sets PROC_STOPPED -> PROC_READY
 *
 * 2 procs: parent (proc 0), child (proc 1).
 * 2 CPUs: BSP (cpu 0), AP (cpu 1).
 *
 * Verifies:
 *   - Parent always sees child ZOMBIE (no lost SIGCHLD wakeup)
 *   - Parent always sees child STOPPED (no lost SIGCHLD)
 *   - SIGCONT correctly wakes STOPPED child
 *   - BKL discipline
 */

#define P_UNUSED   0
#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3
#define P_ZOMBIE   4
#define P_STOPPED  5

byte pstate[2];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

bool sigchld_pending = false;  /* SIGCHLD pending for parent */
bool child_zombie = false;
bool child_stopped = false;
bool parent_saw_zombie = false;
bool parent_saw_stopped = false;
bool child_continued = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

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
 * Scenario 1: Exit + wait4
 *
 * Parent calls wait4 → sleeps (PROC_SLEEPING).
 * Child calls exit → ZOMBIE + SIGCHLD + wake parent.
 * Parent wakes → scans → finds ZOMBIE → reaps.
 * ═══════════════════════════════════════════════════════ */

proctype parent_wait() {
    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };
    BKL_ACQ(0);

    /* wait4: scan for zombie/stopped child */
    byte _pw = 0;
    do
    :: _pw < 6 ->
        _pw++;

        /* Scan under sched_lock */
        if
        :: pstate[1] == P_ZOMBIE ->
            parent_saw_zombie = true;
            pstate[1] = P_UNUSED;  /* reap */
            break
        :: pstate[1] == P_STOPPED ->
            parent_saw_stopped = true;
            break
        :: else ->
            /* No zombie/stopped. Check signals. */
            if :: sigchld_pending -> sigchld_pending = false :: else -> skip fi;

            /* Sleep (set SLEEPING, schedule) */
            pstate[0] = P_SLEEPING;
            cur[0] = 255;

            /* Context switch to idle stack (v8) */
            BKL_REL(0);
            skip;  /* hlt */
            BKL_ACQ(0);

            /* Woken: re-enter loop */
            if :: pstate[0] == P_READY -> pstate[0] = P_RUNNING; cur[0] = 0
               :: else -> skip
            fi
        fi
    :: else -> break
    od;

    if :: bkl_cpu == 0 -> BKL_REL(0) :: else -> skip fi
}

proctype child_exit() {
    atomic { pstate[1] = P_READY; cur[1] = 255 };

    /* Child gets scheduled */
    BKL_ACQ(1);
    if :: pstate[1] == P_READY -> pstate[1] = P_RUNNING; cur[1] = 1
       :: else -> skip
    fi;
    BKL_REL(1);

    /* Child runs in userspace, then exits */
    BKL_ACQ(1);

    /* do_exit: set ZOMBIE, send SIGCHLD, wake parent */
    atomic {
        pstate[1] = P_ZOMBIE;
        child_zombie = true;
        sigchld_pending = true;
        /* Wake parent if sleeping */
        if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY
           :: else -> skip
        fi
    };

    /* schedule() — we're zombie, find something else */
    SCHED(1);
    if :: bkl_cpu == 1 -> BKL_REL(1) :: else -> skip fi
}

/* ═══════════════════════════════════════════════════════
 * Scenario 2: SIGSTOP + SIGCONT
 *
 * Child receives SIGSTOP → PROC_STOPPED + SIGCHLD to parent.
 * Parent receives SIGCHLD, sees stopped child via wait4.
 * Parent sends SIGCONT → child PROC_STOPPED → PROC_READY.
 * ═══════════════════════════════════════════════════════ */

proctype parent_stop_cont() {
    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };
    BKL_ACQ(0);

    /* Wait for child to stop */
    byte _psc = 0;
    do
    :: _psc < 6 ->
        _psc++;
        if
        :: pstate[1] == P_STOPPED ->
            parent_saw_stopped = true;
            /* Send SIGCONT to child */
            atomic {
                if :: pstate[1] == P_STOPPED ->
                    pstate[1] = P_READY;
                    child_continued = true
                :: else -> skip
                fi
            };
            break
        :: else ->
            /* Sleep waiting for SIGCHLD */
            pstate[0] = P_SLEEPING;
            cur[0] = 255;
            BKL_REL(0);
            skip;
            BKL_ACQ(0);
            if :: pstate[0] == P_READY -> pstate[0] = P_RUNNING; cur[0] = 0
               :: else -> skip
            fi
        fi
    :: else -> break
    od;

    if :: bkl_cpu == 0 -> BKL_REL(0) :: else -> skip fi
}

proctype child_stop() {
    atomic { pstate[1] = P_READY; cur[1] = 255 };

    BKL_ACQ(1);
    if :: pstate[1] == P_READY -> pstate[1] = P_RUNNING; cur[1] = 1
       :: else -> skip
    fi;
    BKL_REL(1);

    /* Child runs, receives SIGSTOP via signal_deliver */
    BKL_ACQ(1);

    /* SIGSTOP: set STOPPED, send SIGCHLD, wake parent, schedule */
    atomic {
        pstate[1] = P_STOPPED;
        child_stopped = true;
        sigchld_pending = true;
        if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY
           :: else -> skip
        fi
    };

    /* schedule() — we're stopped */
    cur[1] = 255;
    SCHED(1);
    if :: bkl_cpu == 1 -> BKL_REL(1) :: else -> skip fi;

    /* Child gets SIGCONT (parent sets READY), resumes here */
    BKL_ACQ(1);
    if :: pstate[1] == P_READY -> pstate[1] = P_RUNNING; cur[1] = 1
       :: else -> skip
    fi;
    if :: bkl_cpu == 1 -> BKL_REL(1) :: else -> skip fi
}

/* ═══════════════════════════════════════════════════════
 * Toggle scenario: uncomment one
 * ═══════════════════════════════════════════════════════ */

/* Scenario 1: exit/wait4
init {
    run parent_wait();
    run child_exit()
}
*/

/* Scenario 2: stop/cont */
init {
    run parent_stop_cont();
    run child_stop()
}

/* Exit: parent must see zombie */
ltl parent_sees_zombie { <> parent_saw_zombie }

/* Stop/cont: parent must see stopped, child must continue */
ltl parent_sees_stopped { <> parent_saw_stopped }
ltl child_resumes { <> child_continued }
