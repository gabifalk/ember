/*
 * BKL + Scheduler model v8 — kernel stack ownership.
 *
 * v7 fixed the stale-prev state check. But a deeper bug remains:
 * when schedule() enters the idle hlt loop, the CPU is still on
 * the sleeping process's kernel stack. If another CPU schedules
 * that process, both CPUs use the same kstack → corruption.
 *
 * New invariant: kstack_owner[p] tracks which CPU is physically
 * using process p's kernel stack. A process can only be scheduled
 * on a CPU if no other CPU is on its kstack.
 *
 * Fix: before idle hlt, switch to a per-CPU idle stack
 * (context_switch to idle context), releasing the process's kstack.
 * Only then can another CPU safely schedule the process.
 */

#define N_PROCS 2

#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3

byte pstate[N_PROCS];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

/* NEW: which CPU is physically on each process's kstack.
 * 255 = nobody. A process can only be context_switched to
 * if kstack_owner[p] == 255 (no CPU using its stack). */
byte kstack_owner[N_PROCS];

/* Per-CPU idle stack flag: CPU is on its own idle stack, not
 * any process's kstack. */
bool on_idle_stack[2];

bool boot_done = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = 1; bkl_cpu = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = 0
}

/* ── BSP: proc 0 sleeps ─────────────────────────────── */
proctype bsp() {
    byte prev;

    atomic { pstate[0] = P_RUNNING; cur[0] = 0; kstack_owner[0] = 0 };
    boot_done = true;

    BKL_ACQ(0);
    pstate[0] = P_SLEEPING;

    prev = cur[0];  /* cached once */

    /* Search: nothing READY (proc 1 sleeping) */
    if
    :: pstate[1] == P_READY ->
        /* Found something — switch normally.
         * Release prev's kstack via context_switch. */
        assert(kstack_owner[1] == 255);  /* target kstack must be free */
        pstate[1] = P_RUNNING;
        cur[0] = 1;
        kstack_owner[0] = 255;  /* context_switch saves to prev, releases its stack */
        kstack_owner[1] = 0;    /* now on proc 1's kstack */
        BKL_REL(0);
        goto bsp_done
    :: else -> skip
    fi;

    /* Nothing READY. Must idle.
     *
     * BUGGY version (v6): just release BKL and hlt while still
     * on proc 0's kstack. If AP schedules proc 0, stack collision.
     *
     * FIXED version (v8): context_switch to per-CPU idle stack
     * before releasing BKL. This saves our state into prev->saved_ksp
     * and switches RSP to the idle stack. Now proc 0's kstack is free.
     */

    /* Fix: switch to idle stack (modeled as releasing kstack ownership) */
    kstack_owner[0] = 255;  /* prev's kstack released */
    on_idle_stack[0] = true;
    cur[0] = 255;            /* no current process */

    BKL_REL(0);
    skip;  /* hlt on idle stack */
    BKL_ACQ(0);

    /* After waking: re-search */
    if
    :: pstate[0] == P_READY ->
        /* prev was woken. Switch back. */
        assert(kstack_owner[0] == 255);  /* kstack must be free */
        pstate[0] = P_RUNNING;
        cur[0] = 0;
        kstack_owner[0] = 0;
        on_idle_stack[0] = false;
        BKL_REL(0)
    :: pstate[1] == P_READY ->
        assert(kstack_owner[1] == 255);
        pstate[1] = P_RUNNING;
        cur[0] = 1;
        kstack_owner[1] = 0;
        on_idle_stack[0] = false;
        BKL_REL(0)
    :: else ->
        BKL_REL(0)
    fi;

bsp_done:
    skip
}

/* ── AP: wakes proc 0, schedules it ──────────────────── */
proctype ap() {
    cur[1] = 255;
    on_idle_stack[1] = true;

    boot_done;

    BKL_ACQ(1);

    /* Wake proc 0 */
    if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi;

    /* Schedule proc 0 on AP */
    if
    :: pstate[0] == P_READY ->
        /* CRITICAL CHECK: proc 0's kstack must not be in use by BSP */
        assert(kstack_owner[0] == 255);
        pstate[0] = P_RUNNING;
        cur[1] = 0;
        kstack_owner[0] = 1;  /* AP now on proc 0's kstack */
        on_idle_stack[1] = false
    :: else -> skip
    fi;

    BKL_REL(1)
}

init {
    pstate[0] = P_READY;
    pstate[1] = P_SLEEPING;
    kstack_owner[0] = 255;
    kstack_owner[1] = 255;
    run bsp();
    run ap()
}
