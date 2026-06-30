/*
 * Exec model for ember SMP.
 *
 * Models fork + exec interaction across 2 CPUs:
 *   Parent forks child. Child does execve.
 *   Parent waits for child (wait4).
 *
 * Key state changes during exec (all under BKL):
 *   - cur->pml4_phys = new_pml4 (new address space)
 *   - free old pml4 if not shared
 *   - clear signals, close CLOEXEC fds
 *   - set rip/rsp to new entry point
 *
 * SMP concern: fork creates child with COW-shared pml4.
 * exec frees old pml4 — but parent might still use it.
 * Under BKL, parent can't be in kernel during exec.
 * But parent IS in userspace, using the OLD pml4.
 *
 * THE BUG: if fork shares pml4 (COW), and child execs on AP,
 * exec's "free old pml4 if not shared" check at line 314
 * scans procs[] and sees parent has the same pml4. So it
 * doesn't free. That's correct.
 *
 * But what if schedule() migrates the child to AP, then
 * migrates it BACK to BSP, then the child execs?
 * The "shared" check still works — parent has the old pml4.
 *
 * What if the PARENT execs too? Both parent and child exec.
 * Parent's exec frees old pml4 while child still has it.
 * Under BKL, they can't exec simultaneously.
 *
 * Model the fork + exec + pml4 lifecycle.
 */

#define P_UNUSED   0
#define P_READY    1
#define P_RUNNING  2
#define P_SLEEPING 3
#define P_ZOMBIE   4

byte pstate[2];
byte cur[2];

bool bkl = 0;
byte bkl_cpu = 255;

/* Page table tracking */
#define PML4_PARENT_OLD 1
#define PML4_PARENT_NEW 2
#define PML4_CHILD_NEW  3

byte pml4[2];       /* pml4[proc] = which page table */
bool pml4_freed[4]; /* has this pml4 been freed? */

bool child_exec_done = false;
bool parent_saw_child = false;

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
 * Parent: forks child, waits for it
 * ═══════════════════════════════════════════════════════ */
proctype parent() {
    atomic { pstate[0] = P_RUNNING; cur[0] = 0; pml4[0] = PML4_PARENT_OLD };
    BKL_ACQ(0);

    /* fork: child gets COW-shared pml4 */
    atomic {
        pstate[1] = P_READY;
        pml4[1] = PML4_PARENT_OLD  /* COW shared */
    };

    /* Parent continues in userspace */
    BKL_REL(0);
    skip;  /* userspace */

    /* Parent: wait4 for child */
    BKL_ACQ(0);
    byte _pw = 0;
    do
    :: _pw < 6 ->
        _pw++;
        if
        :: pstate[1] == P_ZOMBIE ->
            parent_saw_child = true;
            pstate[1] = P_UNUSED;
            break
        :: else ->
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

    /* Assert: parent's pml4 was NOT freed by child's exec */
    assert(!pml4_freed[pml4[0]]);

    if :: bkl_cpu == 0 -> BKL_REL(0) :: else -> skip fi
}

/* ═══════════════════════════════════════════════════════
 * Child: gets scheduled, does execve, exits
 * ═══════════════════════════════════════════════════════ */
proctype child() {
    /* Wait to be scheduled */
    BKL_ACQ(1);
    if :: pstate[1] == P_READY -> pstate[1] = P_RUNNING; cur[1] = 1
       :: else -> skip
    fi;
    BKL_REL(1);

    /* Child in userspace, calls execve */
    BKL_ACQ(1);

    /* execve: create new pml4 */
    byte old_pml4 = pml4[1];
    pml4[1] = PML4_CHILD_NEW;

    /* Check if old pml4 is shared with another process */
    bool shared = false;
    if :: pml4[0] == old_pml4 -> shared = true :: else -> skip fi;

    /* Free old pml4 if not shared */
    if
    :: !shared ->
        assert(!pml4_freed[old_pml4]);  /* double-free check */
        pml4_freed[old_pml4] = true
    :: else -> skip
    fi;

    child_exec_done = true;

    /* Return to userspace with new pml4 */
    BKL_REL(1);
    skip;

    /* Child exits */
    BKL_ACQ(1);
    atomic {
        pstate[1] = P_ZOMBIE;
        if :: pstate[0] == P_SLEEPING -> pstate[0] = P_READY :: else -> skip fi
    };
    SCHED(1);
    if :: bkl_cpu == 1 -> BKL_REL(1) :: else -> skip fi
}

init {
    pml4_freed[0] = false;
    pml4_freed[1] = false;
    pml4_freed[2] = false;
    pml4_freed[3] = false;
    run parent();
    run child()
}

/* LTL — check separately:
ltl parent_pml4_safe { [] !pml4_freed[PML4_PARENT_OLD] || !parent_saw_child }
ltl child_execs { <> child_exec_done }
*/
