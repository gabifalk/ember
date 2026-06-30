/*
 * sigint_pgid.pml — Ctrl+C / SIGINT delivery vs process groups.
 *
 * Bug: Ctrl+C hangs the system because:
 *   1. TIOCSPGRP is a no-op → fg_pgid never set
 *   2. console_poll_signals sends SIGINT to current_proc->pgid
 *   3. Children inherit parent's pgid (fork copies pgid)
 *   4. Without setpgid, shell + job + children share the same pgid
 *   5. Shell gets SIGINT → SIG_DFL → shell dies → nobody left → hang
 *
 * Code references:
 *   console.c:52-65       console_poll_signals Ctrl+C handler
 *   syscall_helpers.c:9   console_signal_fg (uses current_proc->pgid)
 *   syscall_file_ops.c:87 TIOCSPGRP (no-op!)
 *   syscall_proc_fork.c:27  child->pgid = parent->pgid
 *
 * Processes:
 *   0 = init  (pid 1, pgid 1)
 *   1 = shell (pid 2, pgid 2)
 *   2 = make  (pid 3, forked by shell)
 *   3 = gcc   (pid 4, forked by make)
 *
 * Verify buggy (finds error):
 *   spin -a models/sigint_pgid.pml && gcc -O2 -o pan pan.c && ./pan
 *
 * Verify fixed (0 errors):
 *   spin -a -DFIX_PGID models/sigint_pgid.pml && gcc -O2 -o pan pan.c && ./pan
 */

#define NPROC 4
#define INIT  0
#define SHELL 1
#define MAKE  2
#define GCC   3

/* Process states */
#define ST_UNUSED   0
#define ST_RUNNING  1
#define ST_SLEEPING 2
#define ST_DEAD     3

/* Signal dispositions */
#define SIGDFL  0
#define SIGIGN  1

byte pstate[NPROC];
byte pgid[NPROC];
byte sigint_disp[NPROC];  /* SIGINT disposition: SIGDFL or SIGIGN */

byte fg_pgid;  /* foreground process group (set by TIOCSPGRP) */

/* ── Deliver SIGINT to a process group ── */
inline deliver_sigint_to_pgid(tgt) {
    byte _i;
    for (_i : 0 .. NPROC-1) {
        if
        :: pstate[_i] != ST_UNUSED && pstate[_i] != ST_DEAD && pgid[_i] == tgt ->
            if
            :: sigint_disp[_i] == SIGDFL -> pstate[_i] = ST_DEAD
            :: sigint_disp[_i] == SIGIGN -> skip
            fi
        :: else -> skip
        fi
    }
}

/* ── Ctrl+C: BUGGY — uses current_proc->pgid ──
 * On SMP, current_proc is whichever process the timer fires on.
 * Non-deterministic choice models this. */
inline ctrl_c_buggy() {
    byte _cur_pgid;
    if
    :: pstate[MAKE] == ST_RUNNING -> _cur_pgid = pgid[MAKE]
    :: pstate[GCC] == ST_RUNNING  -> _cur_pgid = pgid[GCC]
    :: pstate[SHELL] == ST_SLEEPING -> _cur_pgid = pgid[SHELL]
    fi;
    deliver_sigint_to_pgid(_cur_pgid)
}

/* ── Ctrl+C: FIXED — uses fg_pgid set by TIOCSPGRP ── */
inline ctrl_c_fixed() {
    if
    :: fg_pgid > 0 -> deliver_sigint_to_pgid(fg_pgid)
    :: fg_pgid == 0 -> skip  /* no foreground group set */
    fi
}

init {
    /* ── Boot: init + shell ── */
    pstate[INIT] = ST_RUNNING;
    pgid[INIT] = 1;
    sigint_disp[INIT] = SIGIGN;

    pstate[SHELL] = ST_RUNNING;
    pgid[SHELL] = 2;
    sigint_disp[SHELL] = SIGDFL;

    pstate[MAKE] = ST_UNUSED;
    pstate[GCC] = ST_UNUSED;
    fg_pgid = 0;

    /* ── Shell forks make ── */
    atomic {
        pstate[MAKE] = ST_RUNNING;
        pgid[MAKE] = pgid[SHELL];  /* fork: child inherits parent pgid */
        sigint_disp[MAKE] = SIGDFL;
    };

#ifdef FIX_PGID
    /* Correct Unix behavior:
     * Shell calls setpgid(child, child) to put make in its own group,
     * then tcsetpgrp (TIOCSPGRP) to make it the foreground group. */
    pgid[MAKE] = 3;   /* setpgid(0, 0) in child */
    fg_pgid = 3;       /* TIOCSPGRP(3) in shell */
#endif

    /* ── Make forks gcc ── */
    atomic {
        pstate[GCC] = ST_RUNNING;
        pgid[GCC] = pgid[MAKE];  /* fork: inherits make's pgid */
        sigint_disp[GCC] = SIGDFL;
    };

    /* ── Shell calls waitpid (sleeps) ── */
    pstate[SHELL] = ST_SLEEPING;

    /* ── User presses Ctrl+C ── */
#ifdef FIX_PGID
    ctrl_c_fixed();
#else
    ctrl_c_buggy();
#endif

    /* ── PROPERTY: shell must survive Ctrl+C ── */
    assert(pstate[SHELL] != ST_DEAD);

    /* Secondary: make and gcc should be killed */
    assert(pstate[MAKE] == ST_DEAD);
    assert(pstate[GCC] == ST_DEAD);
}
