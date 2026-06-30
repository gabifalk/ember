/*
 * wait4/exit sleep/wakeup model for ember SMP.
 *
 * 2 CPUs, 2 processes: parent (proc 0) on CPU0, child (proc 1) on CPU1.
 * Models the actual ember code:
 *   - wait4 (syscall_proc_wait.c): scan for zombie under sched_lock,
 *     set SLEEPING under sched_lock, unlock, schedule().  BKL held.
 *   - exit (syscall_proc_exit.c:82-122): set ZOMBIE + wake parent
 *     under sched_lock.  BKL held.
 *
 * Verifies: parent eventually reaps child (no lost wakeup).
 *
 * Verify:
 *   spin -a models/wait_lost_wakeup.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -a -f -m100000
 */

#define PARENT 0
#define CHILD  1

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3
#define P_ZOMBIE   4

byte pstate[2];
byte cur[2];          /* cur[cpu] = proc index, 255 = idle */

bool bkl = false;
byte bkl_cpu = 255;

bool sched_locked = false;

bool child_zombie = false;
bool parent_reaped = false;

/* ── BKL ─────────────────────────────────────────────── */
inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}

/* ── sched_lock ──────────────────────────────────────── */
inline SCHED_LOCK() {
    atomic { !sched_locked -> sched_locked = true }
}

inline SCHED_UNLOCK() {
    sched_locked = false
}

/* ════════════════════════════════════════════════════════
 * CPU 0: parent runs wait4 loop.
 *
 * Matches syscall_proc_wait.c:20-134:
 *   for (;;) {
 *       sf = spin_lock_irqsave(&sched_lock);
 *       // scan for zombie
 *       if (zombie) { reap; return; }
 *       cur->state = PROC_SLEEPING;     // line 130
 *       spin_unlock_irqrestore(sf);     // line 132
 *       schedule();                     // line 133
 *   }
 * ════════════════════════════════════════════════════════ */
proctype cpu0_wait() {
    cur[0] = PARENT;
    pstate[PARENT] = P_RUNNING;

    byte attempt = 0;

    do :: attempt < 4 ->
        attempt++;

        /* BKL held from syscall entry */
        BKL_ACQ(0);

        /* lock sched_lock; scan for zombie */
        SCHED_LOCK();

        if
        :: child_zombie ->
            /* Found zombie — reap */
            parent_reaped = true;
            SCHED_UNLOCK();
            BKL_REL(0);
            break

        :: !child_zombie ->
            /* No zombie — sleep.
             * Line 130: cur->state = PROC_SLEEPING (under sched_lock + BKL) */
            pstate[PARENT] = P_SLEEPING;

            /* Line 132: spin_unlock */
            SCHED_UNLOCK();

            /* Line 133: schedule() — releases BKL internally when going idle */
            BKL_REL(0);

            /* Block until woken */
            (pstate[PARENT] == P_READY);
            pstate[PARENT] = P_RUNNING
        fi

    od
}

/* ════════════════════════════════════════════════════════
 * CPU 1: child runs exit.
 *
 * Matches syscall_proc_exit.c:82-122:
 *   sf = spin_lock_irqsave(&sched_lock);
 *   // find parent, set sig_pending, wake if SLEEPING
 *   cur->state = PROC_ZOMBIE;
 *   spin_unlock_irqrestore(sf);
 *   schedule();
 * ════════════════════════════════════════════════════════ */
proctype cpu1_exit() {
    cur[1] = CHILD;
    pstate[CHILD] = P_RUNNING;

    /* Nondeterministic delay — child may exit at any point */
    if :: true -> skip :: true -> skip fi;

    /* BKL held from syscall entry */
    BKL_ACQ(1);

    /* lock sched_lock */
    SCHED_LOCK();

    /* Set zombie */
    child_zombie = true;
    pstate[CHILD] = P_ZOMBIE;

    /* Wake parent if sleeping (line 92-93) */
    if
    :: pstate[PARENT] == P_SLEEPING ->
        pstate[PARENT] = P_READY
    :: else -> skip
    fi;

    SCHED_UNLOCK();

    cur[1] = 255;
    BKL_REL(1)
}

init {
    pstate[PARENT] = P_READY;
    pstate[CHILD] = P_READY;
    cur[0] = 255;
    cur[1] = 255;
    run cpu0_wait();
    run cpu1_exit()
}

/* Liveness: parent eventually reaps child */
ltl liveness { <> parent_reaped }
