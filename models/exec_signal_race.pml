/*
 * Exec + signal delivery model for ember SMP.
 *
 * Models the actual code:
 *   do_execve (syscall_proc_exec.c): BKL held throughout.
 *     1. elf_load_user (no sleep — blkcache holds lock during I/O)
 *     2. cur->pml4_phys = new_pml4 (line 267)
 *     3. Clear sig_pending, sig_mask, reset handlers (lines 293-302)
 *     4. write_cr3(new_pml4) (line 311)
 *
 *   signal_deliver (syscall_sig.c): BKL held (syscall return).
 *     Reads sig_pending, sig_handlers, jumps to handler.
 *
 * BKL serializes exec and signal delivery — no race possible.
 *
 * Verify:
 *   spin -a models/exec_signal_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define SIG_DFL   0
#define SIG_IGN   1
#define VALID_HANDLER 100
#define INVALID_HANDLER 200

byte handler = VALID_HANDLER;   /* signal handler address */
bool sig_pending = false;
byte aspace = 1;                /* 1=old, 2=new */
bool delivered_invalid = false;

bool bkl = false;
byte bkl_cpu = 255;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}

/* CPU0: exec (BKL held throughout — no sleep) */
proctype cpu0_exec() {
    BKL_ACQ(0);

    /* elf_load_user (no BKL release — I/O is synchronous under lock) */
    skip;

    /* Switch address space (line 267) */
    aspace = 2;

    /* Clear signals and reset handlers (lines 293-302) */
    sig_pending = false;
    handler = SIG_DFL;

    /* write_cr3 (line 311) */
    skip;

    BKL_REL(0)
}

/* CPU1: another process sends signal, then target gets scheduled
 * and signal_deliver runs on return. All under BKL. */
proctype cpu1_signal() {
    /* Send signal (kill) */
    BKL_ACQ(1);
    sig_pending = true;
    BKL_REL(1);

    /* Later: target scheduled on CPU1, signal_deliver on return */
    BKL_ACQ(1);
    if
    :: sig_pending ->
        sig_pending = false;
        if
        :: handler > SIG_IGN ->
            /* Dispatch to handler.
             * If handler was invalidated by exec (old address in new
             * address space), this is a bug. */
            if
            :: aspace == 2 && handler == VALID_HANDLER ->
                /* Old handler in new address space = invalid */
                delivered_invalid = true
            :: else -> skip
            fi
        :: else -> skip   /* SIG_DFL or SIG_IGN */
        fi
    :: else -> skip
    fi;
    BKL_REL(1)
}

init {
    handler = VALID_HANDLER;
    sig_pending = false;
    aspace = 1;
    run cpu0_exec();
    run cpu1_signal()
}

/* Safety: never deliver to invalid handler */
ltl no_invalid_delivery { [] !delivered_invalid }
