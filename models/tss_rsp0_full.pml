/*
 * Complete TSS.RSP0 lifecycle model with TLB shootdown IPI.
 *
 * Tracks TSS.RSP0 across ALL paths including:
 * - schedule proc-to-proc (sets TSS.RSP0 = next)
 * - schedule going-idle (resets TSS.RSP0 = idle)
 * - sysretq return to user (TSS.RSP0 unchanged)
 * - TLB shootdown IPI in user mode (uses TSS.RSP0)
 *
 * The bug: after context_switch, CPU returns to user via iretq/sysretq.
 * TSS.RSP0 = process's kstack. Process migrates to another CPU.
 * Old CPU picks a different process, sets TSS.RSP0 = new process.
 * Old CPU's TSS.RSP0 no longer points to migrated process. Safe.
 *
 * But what about: process returns to user, TLB IPI fires on THIS CPU.
 * TSS.RSP0 = this process's kstack. IPI pushes frame on kstack.
 * Simultaneously, another CPU preempts this process (how? it's on THIS CPU).
 * No — a process runs on only one CPU at a time. Safe.
 *
 * New scenario: process P on CPU A returns to user. TSS.RSP0 = P's kstack.
 * CPU B does fork → smp_flush_tlb → sends IPI to CPU A.
 * IPI on CPU A: pushes frame on P's kstack (RSP from TSS.RSP0).
 * Frame is at kstack_top - 40 (5 regs: SS,RSP,RFLAGS,CS,RIP).
 * Our handler pushes 9 more regs (72 bytes). Total 112 bytes.
 * P's kstack already has the syscall frame (152 bytes from top).
 * IPI frame overlaps syscall frame! 112 + 152 = 264 > 256.
 * Wait — they're stacked, not overlapping. IPI frame is BELOW
 * the user-mode interrupt frame. The user-mode interrupt frame
 * starts at kstack_top (loaded from TSS.RSP0).
 *
 * Actually: when interrupted from user mode, hardware loads RSP
 * from TSS.RSP0 = kstack_top. Pushes SS,RSP,RFLAGS,CS,RIP at
 * kstack_top - 40. This OVERWRITES the first 40 bytes of any
 * existing data on the kstack!
 *
 * If the kstack has a syscall frame from a previous syscall that
 * hasn't been cleaned up... but sysretq restores user RSP from
 * the frame. After sysretq, the kstack data is "dead" — the
 * next syscall/interrupt starts fresh from kstack_top.
 *
 * So the IPI pushing at kstack_top should be safe — it overwrites
 * dead data. Unless the process was preempted MID-SYSCALL and its
 * kstack has LIVE data below kstack_top.
 *
 * But if the process is in user mode, it can't be mid-syscall.
 * The kstack is empty (all data was consumed by sysretq/iretq).
 *
 * CONCLUSION: the IPI on a user-mode CPU is safe. The kstack is
 * clean. The handler pushes/pops cleanly. No corruption.
 *
 * The remaining question: is there a path where TSS.RSP0 points
 * to a kstack whose OWNER is on a different CPU?
 */

#define N_CPUS  2
#define N_PROCS 2
#define NONE    255

byte running[N_CPUS];       /* which proc is running */
byte tss_rsp0[N_CPUS];      /* which proc's kstack TSS.RSP0 points to */
byte mode[N_CPUS];          /* 0=kernel, 1=user */

#define P_READY   1
#define P_RUNNING 2
byte pstate[N_PROCS];

/* Track if a TLB IPI hits a kstack owned by another CPU */
bool corruption = false;

inline SCHEDULE(cpu) {
    byte prev = running[cpu];
    byte next = NONE;

    if
    :: pstate[0] == P_READY -> next = 0
    :: pstate[1] == P_READY -> next = 1
    :: true -> next = NONE
    fi;

    if
    :: next != NONE && prev != NONE && next != prev ->
        /* proc-to-proc switch */
        pstate[prev] = P_READY;
        pstate[next] = P_RUNNING;
        running[cpu] = next;
        tss_rsp0[cpu] = next;    /* tss_update_rsp0 */
        mode[cpu] = 0;           /* kernel mode after context_switch */

    :: next != NONE && prev == NONE ->
        /* idle-to-proc */
        pstate[next] = P_RUNNING;
        running[cpu] = next;
        tss_rsp0[cpu] = next;
        mode[cpu] = 0;

    :: next == NONE && prev != NONE ->
        /* going idle */
        pstate[prev] = P_READY;
        running[cpu] = NONE;
        tss_rsp0[cpu] = NONE;    /* reset to idle */
        mode[cpu] = 0;

    :: next == NONE && prev == NONE ->
        skip

    :: next == prev ->
        skip
    fi
}

/* TLB IPI: can arrive at any time. If in user mode, hardware
 * pushes frame onto tss_rsp0[cpu]'s kstack. */
inline TLB_IPI(cpu) {
    if
    :: mode[cpu] == 1 ->
        /* User mode: hardware uses TSS.RSP0 */
        byte target = tss_rsp0[cpu];
        if
        :: target < N_PROCS ->
            /* Check: is this kstack owned by another CPU? */
            byte other = 1 - cpu;
            if
            :: running[other] == target ->
                /* Another CPU is running this process!
                 * We're pushing onto its kstack. CORRUPTION. */
                corruption = true
            :: else -> skip
            fi
        :: else -> skip  /* idle stack — safe */
        fi
    :: else ->
        /* Kernel mode: uses current RSP, not TSS.RSP0. Safe. */
        skip
    fi
}

bool bkl = false;

proctype cpu_loop(byte cpu) {
    byte iter = 0;
    do
    :: iter < 8 ->
        iter++;
        if
        :: true ->
            /* Schedule (under BKL) */
            atomic { !bkl -> bkl = true };
            SCHEDULE(cpu);
            bkl = false;
            /* Return to user mode */
            if
            :: running[cpu] != NONE -> mode[cpu] = 1
            :: else -> skip  /* idle */
            fi
        :: true ->
            /* TLB IPI (can happen WITHOUT BKL — that's the point) */
            TLB_IPI(cpu)
        fi
    od
}

init {
    running[0] = NONE; running[1] = NONE;
    tss_rsp0[0] = NONE; tss_rsp0[1] = NONE;
    mode[0] = 0; mode[1] = 0;
    pstate[0] = P_READY; pstate[1] = P_READY;
    run cpu_loop(0);
    run cpu_loop(1)
}

ltl no_corruption { [] !corruption }
