/*
 * BKL + Scheduler model v9 — sched_idle_loop re-entering schedule().
 *
 * Models the actual recursive call pattern:
 *   schedule() -> context_switch to idle -> sched_idle_loop()
 *   sched_idle_loop: bkl_release, hlt, bkl_acquire, schedule()
 *   schedule() (prev=NULL) -> search -> context_switch to proc
 *   ... proc runs, sleeps ...
 *   schedule() -> context_switch to idle -> sched_idle_loop resumes
 *   sched_idle_loop: loops back, bkl_release, hlt, bkl_acquire, schedule()
 *
 * Key: prev=NULL in idle schedule() means context_switch saves
 * idle state to idle_saved_ksp[cpu], not to prev->saved_ksp.
 * When next process later idles, context_switch resumes in
 * sched_idle_loop's schedule() call (which returns).
 *
 * Also models kstack_cpu tracking to detect kstack collisions.
 */

#define N_PROCS 2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

int kstack_cpu[N_PROCS];   /* -1 = free */
bool on_idle[2];            /* CPU is on idle stack */

bool boot_done = false;

/* ── BKL ─────────────────────────────────────────────── */
inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* ── Schedule from process context (prev != NULL) ───────
 * Called from timer_handler or sched_sleep on a process's kstack.
 * May switch to another process or to idle. */
inline SCHED_FROM_PROC(c) {
    byte _prev = cur[c];
    byte _found = 255;
    assert(_prev != 255);
    assert(bkl_cpu == c);

    /* Search */
    atomic {
        if :: pstate[0] == P_READY && 0 != _prev -> _found = 0
           :: pstate[1] == P_READY && 1 != _prev -> _found = 1
           :: else -> skip
        fi
    };

    if
    :: _found != 255 ->
        /* Switch to another process */
        assert(kstack_cpu[_found] == -1);
        atomic {
            if :: pstate[_prev] == P_RUNNING -> pstate[_prev] = P_READY
               :: else -> skip
            fi;
            kstack_cpu[_prev] = -1;
            pstate[_found] = P_RUNNING;
            kstack_cpu[_found] = c;
            cur[c] = _found
        };
        /* context_switch: now on _found's kstack. BKL held.
         * Returns through _found's call chain -> bkl_release. */
        BKL_REL(c)

    :: _found == 255 && pstate[_prev] == P_RUNNING ->
        /* Nothing else ready, prev still running — return */
        skip

    :: _found == 255 && pstate[_prev] != P_RUNNING ->
        /* prev sleeping, nothing ready. Switch to idle stack. */
        /* Release prev's kstack, clear cur */
        atomic {
            kstack_cpu[_prev] = -1;
            cur[c] = 255;
            on_idle[c] = true
        };
        /* context_switch(&prev->saved_ksp, idle_saved_ksp[c])
         * Saves prev, transfers to sched_idle_loop.
         * When prev is later resumed, returns HERE. */
        /* --- sched_idle_loop runs on idle stack --- */
        /* (modeled inline since it calls schedule recursively) */
        byte _idle_iter = 0;
        do
        :: _idle_iter < 3 ->
            _idle_iter++;
            BKL_REL(c);
            skip;  /* hlt */
            BKL_ACQ(c);
            /* schedule() with prev=NULL (on idle stack) */
            byte _ifound = 255;
            atomic {
                if :: pstate[0] == P_READY -> _ifound = 0
                   :: pstate[1] == P_READY -> _ifound = 1
                   :: else -> skip
                fi
            };
            if
            :: _ifound != 255 ->
                /* Switch from idle to process */
                assert(kstack_cpu[_ifound] == -1);
                atomic {
                    pstate[_ifound] = P_RUNNING;
                    kstack_cpu[_ifound] = c;
                    cur[c] = _ifound;
                    on_idle[c] = false
                };
                /* context_switch(&idle_saved_ksp[c], next->saved_ksp)
                 * Saves idle state. Now on process's kstack. */
                BKL_REL(c);
                break
            :: else ->
                skip  /* nothing, hlt again */
            fi
        :: else -> break
        od;
        /* Either found something and broke out (BKL released),
         * or exhausted iters.
         * When prev is resumed by another schedule(), we return here.
         * At that point we're back on prev's kstack. */
        skip
    fi
}

/* ── BSP ─────────────────────────────────────────────── */
proctype bsp() {
    atomic {
        pstate[0] = P_RUNNING; cur[0] = 0;
        kstack_cpu[0] = 0; on_idle[0] = false
    };
    boot_done = true;

    do :: true ->
        if :: cur[0] == 255 -> break :: else -> skip fi;

        /* Userspace */
        skip;

        /* Timer from user / syscall */
        BKL_ACQ(0);

        if
        :: true ->
            /* Preemption */
            SCHED_FROM_PROC(0)
        :: true ->
            /* Sleep */
            byte p = cur[0];
            if :: p != 255 -> pstate[p] = P_SLEEPING :: else -> skip fi;
            SCHED_FROM_PROC(0)
        :: true ->
            /* Wakeup + sched */
            atomic {
                if :: pstate[0]==P_SLEEPING -> pstate[0]=P_READY :: else -> skip fi;
                if :: pstate[1]==P_SLEEPING -> pstate[1]=P_READY :: else -> skip fi
            };
            SCHED_FROM_PROC(0)
        fi;

        /* BKL may or may not be held here depending on path */
        if :: bkl_cpu == 0 -> BKL_REL(0) :: else -> skip fi
    od
}

/* ── AP ──────────────────────────────────────────────── */
proctype ap() {
    atomic { cur[1] = 255; on_idle[1] = true };
    boot_done;

    do :: true ->
        /* AP idle loop (smp.c) */
        BKL_ACQ(1);

        /* schedule() with prev=NULL */
        byte _afound = 255;
        atomic {
            if :: pstate[0] == P_READY -> _afound = 0
               :: pstate[1] == P_READY -> _afound = 1
               :: else -> skip
            fi
        };

        if
        :: _afound != 255 ->
            assert(kstack_cpu[_afound] == -1);
            atomic {
                pstate[_afound] = P_RUNNING;
                kstack_cpu[_afound] = 1;
                cur[1] = _afound;
                on_idle[1] = false
            };
            /* context_switch(&idle_saved_ksp[1], next->saved_ksp) */
            BKL_REL(1);

            /* Process runs in userspace on AP */
            skip;

            /* Timer preempt / syscall on AP */
            BKL_ACQ(1);
            if
            :: true -> SCHED_FROM_PROC(1)
            :: true ->
                byte p2 = cur[1];
                if :: p2 != 255 -> pstate[p2] = P_SLEEPING :: else -> skip fi;
                SCHED_FROM_PROC(1)
            :: true ->
                atomic {
                    if :: pstate[0]==P_SLEEPING -> pstate[0]=P_READY :: else -> skip fi;
                    if :: pstate[1]==P_SLEEPING -> pstate[1]=P_READY :: else -> skip fi
                };
                SCHED_FROM_PROC(1)
            fi;
            if :: bkl_cpu == 1 -> BKL_REL(1) :: else -> skip fi
        :: else ->
            BKL_REL(1)
        fi
    od
}

init {
    pstate[0] = P_READY;
    pstate[1] = P_READY;
    kstack_cpu[0] = -1;
    kstack_cpu[1] = -1;
    run bsp();
    run ap()
}
