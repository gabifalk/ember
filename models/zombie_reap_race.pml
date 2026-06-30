/*
 * Zombie reap vs reparent-auto-reap race model for ember SMP.
 *
 * Models two kernel paths that can both target the same zombie:
 *
 *   Path A — do_exit reparent + auto-reap (syscall_proc_exit.c:82-122):
 *     Under sched_lock: reparent child to init. If init has SIG_IGN,
 *     auto-reap: save zpml4, zero pml4_phys, set UNUSED.
 *     Unlock. Check shared, free zpml4. All under BKL.
 *
 *   Path B — wait4 reap (syscall_proc_wait.c:44-66):
 *     Under sched_lock: find zombie, save zpml4, zero pml4_phys,
 *     set UNUSED. Unlock. Check shared, free zpml4. All under BKL.
 *
 * Both paths zero pml4_phys under sched_lock AND both hold BKL.
 * The BKL serializes the two paths entirely.
 *
 * Verify: no double-free.
 *   spin -a models/zombie_reap_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define P_UNUSED   0
#define P_ZOMBIE   4

byte cstate = P_ZOMBIE;
byte c_pml4_phys = 1;       /* 1 = allocated, 0 = cleared */
byte pml4_free_count = 0;

bool sched_lk = false;
bool bkl = false;
byte bkl_cpu = 255;

inline SCHED_LOCK() {
    atomic { !sched_lk -> sched_lk = true }
}
inline SCHED_UNLOCK() {
    sched_lk = false
}
inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}

/* ═══════════════════════════════════════════════════════
 * Reaper A: do_exit auto-reap path (syscall_proc_exit.c:98-116)
 *
 * Under BKL + sched_lock:
 *   zpml4 = cur->pml4_phys;    // line 100
 *   cur->pml4_phys = 0;        // line 101
 *   cur->state = PROC_UNUSED;  // line 102
 *   spin_unlock;               // line 104
 *   // check shared, free zpml4  // lines 106-116
 * ═══════════════════════════════════════════════════════ */
proctype reaper_A() {
    byte zpml4 = 0;

    BKL_ACQ(0);
    SCHED_LOCK();

    if
    :: cstate == P_ZOMBIE ->
        zpml4 = c_pml4_phys;
        c_pml4_phys = 0;
        cstate = P_UNUSED;
        SCHED_UNLOCK();

        if
        :: zpml4 != 0 ->
            pml4_free_count++;
            assert(pml4_free_count <= 1)
        :: else -> skip
        fi
    :: else ->
        SCHED_UNLOCK()
    fi;

    BKL_REL(0)
}

/* ═══════════════════════════════════════════════════════
 * Reaper B: wait4 reap path (syscall_proc_wait.c:44-66)
 *
 * Under BKL + sched_lock:
 *   zpml4 = zombie->pml4_phys;   // line 48
 *   zombie->pml4_phys = 0;       // line 50
 *   zombie->state = PROC_UNUSED; // line 51
 *   spin_unlock;                 // line 53
 *   // check shared, free zpml4    // lines 56-66
 * ═══════════════════════════════════════════════════════ */
proctype reaper_B() {
    byte zpml4 = 0;

    BKL_ACQ(1);
    SCHED_LOCK();

    if
    :: cstate == P_ZOMBIE ->
        zpml4 = c_pml4_phys;
        c_pml4_phys = 0;
        cstate = P_UNUSED;
        SCHED_UNLOCK();

        if
        :: zpml4 != 0 ->
            pml4_free_count++;
            assert(pml4_free_count <= 1)
        :: else -> skip
        fi
    :: else ->
        SCHED_UNLOCK()
    fi;

    BKL_REL(1)
}

init {
    cstate = P_ZOMBIE;
    c_pml4_phys = 1;
    pml4_free_count = 0;
    run reaper_A();
    run reaper_B()
}

/* Safety: pml4 freed at most once */
ltl no_double_free { [] (pml4_free_count < 2) }
