/*
 * do_exit_from_isr BKL model for ember SMP.
 *
 * Models the interaction between:
 *   CPU0: in kernel with BKL held, executing fork (modifying proc table)
 *   CPU1: in ISR handler for fatal user-mode fault, calls do_exit_from_isr
 *
 * Key finding: isr_entry.S:79 calls bkl_acquire_entry BEFORE the C handler,
 * so do_exit_from_isr runs WITH BKL held.  do_exit also asserts BKL
 * (syscall_proc_exit.c:9).  The BKL serializes ISR exit with fork.
 *
 * This model verifies that the BKL prevents concurrent proc table
 * modification between fork on CPU0 and ISR-triggered exit on CPU1.
 *
 * Verify:
 *   spin -a models/exit_isr_bkl_bypass.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define P_UNUSED   0
#define P_READY    1
#define P_RUNNING  2
#define P_ZOMBIE   4
#define N_PROCS    4

byte pstate[N_PROCS];
byte ppid[N_PROCS];
byte pml4_id[N_PROCS];
byte pml4_ref[3];

bool bkl = false;
byte bkl_cpu = 255;

/* Track concurrent proc table modifications */
byte in_modify = 0;

bool fork_completed = false;
bool exit_completed = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}

/* CPU0: fork (syscall entry, BKL held) */
proctype cpu0_fork() {
    BKL_ACQ(0);
    in_modify++;

    /* Allocate child slot */
    assert(pstate[2] == P_UNUSED);
    pstate[2] = P_READY;
    ppid[2] = 1;

    /* Clone pml4 */
    pml4_ref[1] = pml4_ref[1] + 1;
    pml4_id[2] = 2;
    pml4_ref[2] = 1;

    pstate[2] = P_READY;

    in_modify--;
    fork_completed = true;
    BKL_REL(0)
}

/* CPU1: ISR fatal fault -> do_exit_from_isr
 *
 * isr_entry.S:79 acquires BKL before calling C handler.
 * do_exit_from_isr (syscall_proc_exit.c:129) calls do_exit.
 * do_exit (line 9) asserts BKL held.
 */
proctype cpu1_exit_isr() {
    /* ISR entry stub acquires BKL (isr_entry.S:79) */
    BKL_ACQ(1);

    in_modify++;

    /* do_exit: reparent children (lines 46-78) */
    byte i = 0;
    do
    :: i < N_PROCS ->
        if
        :: pstate[i] != P_UNUSED && ppid[i] == 3 ->
            ppid[i] = 0
        :: else -> skip
        fi;
        i++
    :: else -> break
    od;

    /* Set self to ZOMBIE (line 118) */
    pstate[3] = P_ZOMBIE;

    /* Free own pml4 if not shared */
    byte my_pml4 = pml4_id[3];
    if
    :: my_pml4 != 0 ->
        pml4_ref[my_pml4] = pml4_ref[my_pml4] - 1
    :: else -> skip
    fi;

    in_modify--;
    exit_completed = true;

    /* ISR exit stub releases BKL (isr_entry.S:122-123) */
    BKL_REL(1)
}

init {
    pstate[0] = P_RUNNING;
    ppid[0] = 0;
    pml4_id[0] = 0;

    pstate[1] = P_RUNNING;
    ppid[1] = 0;
    pml4_id[1] = 1;
    pml4_ref[1] = 2;

    pstate[2] = P_UNUSED;
    pml4_id[2] = 0;

    pstate[3] = P_RUNNING;
    ppid[3] = 1;
    pml4_id[3] = 1;

    run cpu0_fork();
    run cpu1_exit_isr()
}

/* Safety: BKL prevents concurrent proc table modification */
#define mutex_ok (in_modify <= 1)
ltl no_concurrent_modify { [] mutex_ok }
