/*
 * Signal delivery model for ember SMP.
 *
 * Models the exact C code paths:
 *
 * kill(pid, sig):
 *   sched_lock { target->sig_pending |= (1<<sig);
 *                if target SLEEPING -> READY }
 *
 * signal_deliver() on syscall return:
 *   sched_lock { pending = sig_pending & ~sig_mask;
 *                clear bit; }
 *   dispatch handler or default action
 *
 * pipe_wait_readable (signal check before sleep):
 *   sched_lock { check sig_pending; if pending return -EINTR;
 *                set SLEEPING }
 *   recheck pipe; if ready cancel sleep
 *   schedule()
 *
 * Scenario: process A (reader) blocking on pipe. Process B (writer)
 * sends signal to A via kill(). A must wake up and see the signal.
 *
 * Verifies:
 *   - Signal to sleeping process wakes it (SLEEPING -> READY)
 *   - Signal is seen on syscall return (signal_deliver)
 *   - No lost signal (sig_pending bit persists until delivered)
 *   - BKL discipline maintained
 */

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[2];     /* proc 0 = reader/target, proc 1 = sender */
byte cur[2];        /* per-CPU current proc */

bool bkl = 0;
byte bkl_cpu = 255;

/* Signal state for proc 0 */
bool sig_pending = false;
bool sig_delivered = false;

/* Pipe state */
byte pipe_count = 0;

/* Tracking */
bool reader_woke = false;

/* ── BKL ─────────────────────────────────────────────── */
inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* ── Schedule ────────────────────────────────────────── */
byte _sc_f;

inline SCHED(c) {
    atomic {
        _sc_f = 255;
        if :: pstate[0] == P_READY && cur[c] != 0 -> _sc_f = 0
           :: pstate[1] == P_READY && cur[c] != 1 -> _sc_f = 1
           :: else -> skip
        fi;
        if
        :: _sc_f != 255 ->
            byte _old = cur[c];
            if :: _old != 255 && pstate[_old] == P_RUNNING -> pstate[_old] = P_READY
               :: _old != 255 && pstate[_old] != P_RUNNING -> skip
               :: _old == 255 -> skip
            fi;
            pstate[_sc_f] = P_RUNNING;
            cur[c] = _sc_f
        :: else -> skip
        fi
    }
}

/* ════════════════════════════════════════════════════════
 * CPU 0 (BSP): runs reader (proc 0)
 *
 * Models pipe_wait_readable with signal check:
 *   1. Check sig_pending (under sched_lock)
 *   2. If pending: return -EINTR (don't sleep)
 *   3. Check pipe_count (under pipe lock)
 *   4. If data: read, return
 *   5. Set SLEEPING (under sched_lock)
 *   6. Recheck pipe + signals
 *   7. schedule()
 *   8. Woken: loop to step 1
 *
 * After pipe_wait returns:
 *   signal_deliver() checks sig_pending & ~sig_mask
 * ════════════════════════════════════════════════════════ */
proctype cpu0() {
    byte iter;

    atomic { pstate[0] = P_RUNNING; cur[0] = 0 };

    /* Syscall entry: read() on pipe */
    BKL_ACQ(0);

    /* pipe_wait_readable loop */
    iter = 0;
    do
    :: iter < 4 ->
        iter++;

        /* Step 1: check signals (under sched_lock, modeled atomic) */
        if
        :: sig_pending ->
            /* Step 2: return -EINTR */
            reader_woke = true;
            break
        :: else -> skip
        fi;

        /* Step 3: check pipe_count */
        if
        :: pipe_count > 0 ->
            /* Step 4: read data */
            pipe_count--;
            reader_woke = true;
            break
        :: else -> skip
        fi;

        /* Step 5: set SLEEPING (under sched_lock) */
        pstate[0] = P_SLEEPING;

        /* Step 6: recheck pipe + signals */
        if
        :: pipe_count > 0 || sig_pending ->
            /* Cancel sleep */
            atomic {
                if :: pstate[0] == P_SLEEPING -> pstate[0] = P_RUNNING
                   :: else -> skip
                fi
            }
        :: else ->
            /* Step 7: schedule — context_switch to idle, hlt */
            /* First: save prev, switch to idle stack (v8) */
            atomic {
                cur[0] = 255;
                /* kstack released — on idle stack now */
            };
            BKL_REL(0);
            /* hlt — other CPU can wake us */
            skip;
            BKL_ACQ(0);
            /* Check if proc 0 is READY now */
            if
            :: pstate[0] == P_READY ->
                /* Switch back to proc 0 */
                pstate[0] = P_RUNNING;
                cur[0] = 0
            :: else -> skip
            fi
        fi

    :: else -> break
    od;

    /* signal_deliver on syscall return */
    if
    :: sig_pending ->
        sig_pending = false;
        sig_delivered = true
    :: else -> skip
    fi;

    BKL_REL(0);

    /* Back in userspace. If signal not yet delivered, process will
     * re-enter kernel (next syscall or timer preemption) where
     * signal_deliver runs again. Model: loop back. */
    byte outer = 0;
    do
    :: outer < 3 && !sig_delivered ->
        outer++;
        /* Timer preempt or next syscall → kernel entry */
        BKL_ACQ(0);
        /* signal_deliver */
        if
        :: sig_pending ->
            sig_pending = false;
            sig_delivered = true
        :: else -> skip
        fi;
        BKL_REL(0)
    :: else -> break
    od
}

/* ════════════════════════════════════════════════════════
 * CPU 1 (AP): runs sender (proc 1)
 *
 * Models kill(target_pid, sig):
 *   sched_lock { target->sig_pending |= (1<<sig);
 *                if target SLEEPING -> READY }
 * ════════════════════════════════════════════════════════ */
proctype cpu1() {
    atomic { cur[1] = 255 };

    /* Wait for BSP to boot */
    BKL_ACQ(1);

    /* Schedule proc 1 */
    if :: pstate[1] == P_READY ->
        pstate[1] = P_RUNNING;
        cur[1] = 1
    :: else -> skip
    fi;

    BKL_REL(1);

    /* Proc 1 in userspace, calls kill(proc0_pid, SIGUSR1) */
    BKL_ACQ(1);

    /* do_kill_pid: set sig_pending, wake if sleeping */
    atomic {
        sig_pending = true;
        if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY
           :: else -> skip
        fi
    };

    BKL_REL(1)
}

init {
    pstate[0] = P_READY;
    pstate[1] = P_READY;
    pipe_count = 0;
    sig_pending = false;
    sig_delivered = false;
    run cpu0();
    run cpu1()
}

/* Signal must eventually be delivered */
ltl signal_delivered_eventually { <> sig_delivered }

/* If signal is pending and reader returns from syscall, signal is delivered */
ltl no_lost_signal { [] (sig_pending -> <> sig_delivered) }

/* Safety: no assertions fire (BKL discipline checked inline) */
