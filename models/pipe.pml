/*
 * Pipe sleep/wakeup model for ember SMP.
 *
 * 2 CPUs, 2 kernel processes: reader (proc 0) and writer (proc 1).
 * Promela proctypes represent CPUs, not processes.
 * Each CPU checks cur[cpu] to see which process it's running,
 * then executes that process's next action.
 *
 * Verifies: no lost wakeup — reader eventually reads data.
 */

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[2];
byte cur[2];     /* cur[cpu] = proc index, 255 = idle */

bool bkl = 0;
byte bkl_cpu = 255;

/* Pipe state */
byte pipe_count = 0;

/* Process progress: what each process does next */
#define ACT_READ_CHECK  0
#define ACT_READ_SLEEP  1
#define ACT_READ_DONE   2
#define ACT_WRITE       3
#define ACT_WRITE_DONE  4

byte action[2];  /* next action per process */

bool reader_done = false;

/* ── BKL ─────────────────────────────────────────────── */
inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* ── Schedule: find READY, switch ────────────────────── */
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
 * CPU 0 (BSP): starts with reader (proc 0)
 * ════════════════════════════════════════════════════════ */
proctype cpu0() {
    cur[0] = 0;
    pstate[0] = P_RUNNING;
    action[0] = ACT_READ_CHECK;

    do :: true ->
        /* Syscall/timer entry: acquire BKL */
        BKL_ACQ(0);

        byte p = cur[0];
        if :: p == 255 -> BKL_REL(0); break :: else -> skip fi;

        /* Execute the current process's next action */
        if
        :: p == 0 && action[0] == ACT_READ_CHECK ->
            /* pipe_wait_readable: check pipe_count */
            if
            :: pipe_count > 0 ->
                /* Read data */
                pipe_count--;
                reader_done = true;
                action[0] = ACT_READ_DONE;
                /* Wake writer */
                if :: pstate[1] == P_SLEEPING -> pstate[1] = P_READY :: else -> skip fi
            :: else ->
                /* Empty: go to sleep */
                action[0] = ACT_READ_SLEEP
            fi;
            BKL_REL(0)

        :: p == 0 && action[0] == ACT_READ_SLEEP ->
            /* Set SLEEPING (under sched_lock) */
            pstate[0] = P_SLEEPING;
            /* Recheck pipe_count (the recheck-after-sleep pattern) */
            if
            :: pipe_count > 0 ->
                /* Cancel sleep */
                if :: pstate[0] == P_SLEEPING -> pstate[0] = P_RUNNING :: else -> skip fi;
                action[0] = ACT_READ_CHECK
            :: else ->
                /* Really sleep: schedule */
                SCHED(0);
                if
                :: cur[0] != 255 && pstate[cur[0]] == P_RUNNING ->
                    skip  /* switched to another proc */
                :: else ->
                    /* Nothing else: idle hlt */
                    BKL_REL(0);
                    skip;  /* hlt — AP can run writer */
                    BKL_ACQ(0);
                    SCHED(0)
                fi;
                /* When reader wakes, it re-checks */
                action[0] = ACT_READ_CHECK
            fi;
            BKL_REL(0)

        :: p == 0 && action[0] == ACT_READ_DONE ->
            BKL_REL(0);
            break

        :: p == 1 && action[1] == ACT_WRITE ->
            /* pipe write */
            pipe_count++;
            /* Wake reader */
            if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;
            action[1] = ACT_WRITE_DONE;
            /* Schedule: maybe switch back to reader */
            SCHED(0);
            BKL_REL(0)

        :: p == 1 && action[1] == ACT_WRITE_DONE ->
            BKL_REL(0)

        :: else ->
            BKL_REL(0)
        fi
    od
}

/* ════════════════════════════════════════════════════════
 * CPU 1 (AP): idle, picks up work
 * ════════════════════════════════════════════════════════ */
proctype cpu1() {
    cur[1] = 255;

    do :: true ->
        /* hlt, timer wakes us */
        skip;

        BKL_ACQ(1);
        SCHED(1);

        byte p = cur[1];
        if
        :: p != 255 ->
            /* Execute this process's action */
            if
            :: p == 1 && action[1] == ACT_WRITE ->
                pipe_count++;
                if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;
                action[1] = ACT_WRITE_DONE;
                SCHED(1)
            :: p == 0 && action[0] == ACT_READ_CHECK ->
                if
                :: pipe_count > 0 ->
                    pipe_count--;
                    reader_done = true;
                    action[0] = ACT_READ_DONE;
                    if :: pstate[1] == P_SLEEPING -> pstate[1] = P_READY :: else -> skip fi
                :: else ->
                    action[0] = ACT_READ_SLEEP
                fi
            :: else -> skip
            fi;
            BKL_REL(1)
        :: else ->
            BKL_REL(1)
        fi
    od
}

init {
    pstate[0] = P_READY;
    pstate[1] = P_READY;
    action[0] = ACT_READ_CHECK;
    action[1] = ACT_WRITE;
    run cpu0();
    run cpu1()
}

ltl no_lost_wakeup { <> reader_done }
