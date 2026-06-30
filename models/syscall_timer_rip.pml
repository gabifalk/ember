/*
 * syscall_timer_rip.pml — kstack memory model for RIP corruption (v2).
 *
 * Bug: make -j4 on SMP (-smp 64) crashes a cp child:
 *   RIP=0x246 (RFLAGS), RCX=0x4265a6, R11=0x246.
 *   RIP≠RCX ⇒ iretq return (timer preempt), not sysretq.
 *
 * This model tracks the 5 qwords at kstack_top for each process,
 * and every code path that reads/writes them.  Includes:
 *   - Syscall entry/return (sysretq)
 *   - Timer preempt push/pop (iretq)
 *   - signal_deliver, rt_sigreturn
 *   - signal_deliver_isr (exception signal delivery)
 *   - Exception handler (isr_common: iretq return)
 *   - do_exit auto-reap: proc_free_slot + schedule on freed kstack
 *   - Slot reuse: proc_alloc + setup_child_kstack on recycled slot
 *   - schedule() idle path: context_switch(&prev->saved_ksp, idle)
 *   - exec: replaces frame rip/rsp during syscall
 *   - Syscall sleep/wake: process sleeps, woken by timer or peer
 *   - BKL serialization
 *
 * Slot layout per process kstack (5 qwords at kstack_top):
 *   [0] kstack_top - 8:   syscall orig_rax  / iretq SS
 *   [1] kstack_top - 16:  syscall rsp       / iretq RSP
 *   [2] kstack_top - 24:  syscall rflags    / iretq RFLAGS
 *   [3] kstack_top - 32:  syscall rip       / iretq CS
 *   [4] kstack_top - 40:  syscall rax       / iretq RIP  ← crash
 *
 * Code references:
 *   syscall_entry.S:15-76     frame build
 *   syscall_entry.S:95-99     syscall_return (sysretq)
 *   isr_entry.S:149-204       timer ISR user-mode path
 *   isr_entry.S:54-122        isr_common (exception ISR path)
 *   isr.c:50-306              isr_handler (exception dispatch)
 *   syscall_sig.c:149-281     signal_deliver
 *   syscall_sig.c:285-335     signal_deliver_isr
 *   syscall_sig.c:424-446     rt_sigreturn
 *   syscall_proc_exit.c:8-136 do_exit (auto-reap + zombie)
 *   syscall_proc_exec.c       do_exec (replaces frame)
 *   sched.c:113-267           schedule (idle + switch paths)
 *   proc.c:98-124             proc_alloc
 *   proc.c:153-159            proc_free_slot
 *   syscall_proc_fork.c:57-81 setup_child_kstack
 *
 * Verify:
 *   spin -a models/syscall_timer_rip.pml && \
 *   gcc -O2 -DMEMLIM=4096 -o pan pan.c && \
 *   ./pan -m500000
 */

#define NCPU  2
#define NPROC 2

/* Symbolic tags for kstack slot values */
#define V_ZERO     0
#define V_RFLAGS   1   /* 0x246 */
#define V_USERIP   2   /* valid user code address */
#define V_CS       3   /* 0x23 */
#define V_USERRSP  4
#define V_SS       5   /* 0x1b */
#define V_RAX      6   /* syscall return value */
#define V_ORIGAX   7   /* syscall number */
#define V_SIGHAND  8   /* signal handler address */
#define V_SIGRSP   9
#define V_GARBAGE 10   /* stale/freed/overwritten by unrelated code */
#define V_EXECIP  11   /* exec new entry point */
#define V_EXECRSP 12   /* exec new user stack */

/* Process states */
#define ST_UNUSED   0
#define ST_USER     1
#define ST_INSYS    2   /* in syscall between sti..cli */
#define ST_PREEMPT  3   /* timer-preempted */
#define ST_FORKRET  4   /* fork child awaiting first schedule */
#define ST_ZOMBIE   5
#define ST_DEAD     6   /* auto-reaped: UNUSED + slot freed */
#define ST_SLEEPING 7   /* sleeping in syscall (e.g., read on empty pipe) */

/* kstack top: 5 slots per process */
byte slot[NPROC * 5];

byte pstate[NPROC];
byte kstack_cpu[NPROC];    /* 255 = free */

byte cpu_proc[NCPU];       /* 255 = idle */
byte cpu_tss[NCPU];        /* TSS.RSP0 → which proc's kstack (255 = idle) */

bool bkl;
byte bkl_who;

bool slot_on_freelist[NPROC]; /* true if proc_free_slot has been called */
byte fork_gen;                /* how many times slot 1 has been forked */

/* ── BKL ──────────────────────────────── */
inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_who = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_who == c);
    atomic { bkl_who = 255; bkl = false }
}

/* ── Valid user code address check ── */
/* RIP must be one of the valid user code addresses, never RFLAGS/CS/SS/etc */
#define VALID_RIP(v) ((v) == V_USERIP || (v) == V_SIGHAND || (v) == V_EXECIP)

/* ── Syscall entry: write frame at kstack_top ── */
inline SYSCALL_ENTRY(p) {
    atomic {
        slot[p*5+0] = V_ORIGAX;
        slot[p*5+1] = V_USERRSP;
        slot[p*5+2] = V_RFLAGS;
        slot[p*5+3] = V_USERIP;
        slot[p*5+4] = V_RAX
    }
}

/* ── Sysretq: check slot[3] (rip) is valid user address ── */
inline SYSRETQ_CHECK(p) {
    assert(VALID_RIP(slot[p*5+3]))
}

/* ── Timer preempt: CPU pushes iretq frame using TSS.RSP0 ── */
inline TIMER_PUSH(c, p) {
    if
    :: cpu_tss[c] == p ->
        /* Correct: frame pushed at p's kstack_top */
        atomic {
            slot[p*5+0] = V_SS;
            slot[p*5+1] = V_USERRSP;
            slot[p*5+2] = V_RFLAGS;
            slot[p*5+3] = V_CS;
            slot[p*5+4] = V_USERIP
        }
    :: cpu_tss[c] != p ->
        /* TSS mismatch: frame goes elsewhere, p's slots stay stale */
        skip
    fi
}

/* ── Timer iretq: read RIP from slot[4] ── */
inline TIMER_IRETQ(p) {
    assert(VALID_RIP(slot[p*5+4]))
}

/* ── Exception (isr_common): CPU pushes iretq frame using TSS.RSP0.
 *    Same 5 qwords at kstack_top as timer. The vector/error_code
 *    and 15 GPRs are below (not tracked — only top 5 matter). ── */
inline EXCEPTION_PUSH(c, p) {
    if
    :: cpu_tss[c] == p ->
        atomic {
            slot[p*5+0] = V_SS;
            slot[p*5+1] = V_USERRSP;
            slot[p*5+2] = V_RFLAGS;
            slot[p*5+3] = V_CS;
            slot[p*5+4] = V_USERIP
        }
    :: cpu_tss[c] != p ->
        skip
    fi
}

/* ── Exception iretq: same frame as timer ── */
inline EXCEPTION_IRETQ(p) {
    assert(VALID_RIP(slot[p*5+4]))
}

/* ── signal_deliver: modify rip, rsp (during syscall return) ── */
inline SIG_DELIVER(p) {
    atomic {
        slot[p*5+3] = V_SIGHAND;
        slot[p*5+1] = V_SIGRSP
    }
}

/* ── signal_deliver_isr: modify rip, rsp (during exception return).
 *    Modifies frame->rip (slot[4] = kstack_top-40) and
 *    frame->rsp (slot[1] = kstack_top-16). ── */
inline SIG_DELIVER_ISR(p) {
    atomic {
        slot[p*5+4] = V_SIGHAND;
        slot[p*5+1] = V_SIGRSP
    }
}

/* ── rt_sigreturn: kmemcpy restores entire frame ── */
inline RT_SIGRETURN(p) {
    atomic {
        slot[p*5+0] = V_ORIGAX;
        slot[p*5+1] = V_USERRSP;
        slot[p*5+2] = V_RFLAGS;
        slot[p*5+3] = V_USERIP;
        slot[p*5+4] = V_RAX
    }
}

/* ── exec: replace frame rip/rsp during syscall ── */
inline DO_EXEC(p) {
    atomic {
        slot[p*5+3] = V_EXECIP;
        slot[p*5+1] = V_EXECRSP
        /* rflags, rax, orig_rax unchanged (still from syscall entry) */
    }
}

/* ── do_exit auto-reap: marks slot UNUSED, adds to free list.
 *    Then schedule() runs on the freed kstack.
 *    schedule() idle path writes prev->saved_ksp (stale write).
 *    Then context_switch to idle. BKL released in idle loop. ── */
inline DO_EXIT_AUTOREAP(c, p) {
    /* cur->state = PROC_UNUSED; proc_free_slot(idx) */
    atomic {
        pstate[p] = ST_DEAD;
        slot_on_freelist[p] = true
    };

    /* schedule() called on freed kstack.
     * No READY process found (or maybe one found).
     * Idle path: kstack_cpu[prev] = -1, context_switch to idle.
     * The context_switch(&prev->saved_ksp, idle) writes to the
     * freed slot's saved_ksp field — use-after-free, but safe
     * because BKL is held and proc_alloc will overwrite it. */

    /* Mark kstack free (schedule idle path line 166) */
    kstack_cpu[p] = 255;
    /* TSS/gs → idle (schedule idle path lines 171-173) */
    cpu_tss[c] = 255;
    /* context_switch saves RSP to prev->saved_ksp — stale write.
     * Model: the kstack slots are NOT changed by context_switch
     * (it only saves/restores RSP, not the frame data above it).
     * Switch to idle. */
    cpu_proc[c] = 255
    /* BKL still held until idle loop releases it. */
}

/* ── proc_alloc + setup_child_kstack: reuse a freed slot ── */
inline FORK_REUSE(p, parent) {
    /* proc_alloc: zeros saved_ksp, sets SLEEPING, etc. */
    /* setup_child_kstack: copies parent frame, sets rax=0 */
    atomic {
        pstate[p] = ST_FORKRET;
        kstack_cpu[p] = 255;
        slot_on_freelist[p] = false;
        /* Copy parent's frame (parent is in syscall, frame on kstack) */
        slot[p*5+0] = slot[parent*5+0];
        slot[p*5+1] = slot[parent*5+1];
        slot[p*5+2] = slot[parent*5+2];
        slot[p*5+3] = slot[parent*5+3];
        slot[p*5+4] = V_RAX;  /* child rax = 0 */
        fork_gen = fork_gen + 1
    }
}

/* ── schedule ── */
inline DO_SCHED(c, out) {
    byte _prev;
    _prev = cpu_proc[c];
    out = 255;

    if
    :: _prev != 255 -> kstack_cpu[_prev] = 255
    :: else -> skip
    fi;

    if
    :: (pstate[0] == ST_PREEMPT || pstate[0] == ST_FORKRET || pstate[0] == ST_SLEEPING) && kstack_cpu[0] == 255 -> out = 0
    :: (pstate[1] == ST_PREEMPT || pstate[1] == ST_FORKRET || pstate[1] == ST_SLEEPING) && kstack_cpu[1] == 255 -> out = 1
    :: else -> skip
    fi;

    if
    :: out != 255 ->
        atomic {
            kstack_cpu[out] = c;
            cpu_proc[c] = out;
            cpu_tss[c] = out
        }
    :: else ->
        atomic {
            cpu_proc[c] = 255;
            cpu_tss[c] = 255
        }
    fi
}

/* ── Resume a scheduled process ── */
inline RESUME(c, p) {
    if
    :: pstate[p] == ST_FORKRET ->
        /* fork_child_return → syscall_return → sysretq */
        SYSRETQ_CHECK(p);
        atomic { pstate[p] = ST_USER }
    :: pstate[p] == ST_PREEMPT ->
        /* Timer ISR return: pop GPRs, iretq */
        TIMER_IRETQ(p);
        atomic { pstate[p] = ST_USER }
    :: pstate[p] == ST_SLEEPING ->
        /* Wake from sleep: resume in syscall path → sysretq.
         * The process resumes from schedule() inside the syscall,
         * finishes the syscall, runs signal_deliver, then sysretq.
         * Frame was written by SYSCALL_ENTRY (or modified by SIG_DELIVER). */
        SYSRETQ_CHECK(p);
        atomic { pstate[p] = ST_USER }
    :: else -> skip
    fi
}

/* ── CPU thread ── */
proctype cpu_thread(byte me) {
    byte p;
    byte sr;

end_idle:
    do
    :: true ->
        p = cpu_proc[me];

        if
        /* ── IDLE ── */
        :: p == 255 ->
            BKL_ACQ(me);
            DO_SCHED(me, sr);
            if
            :: sr != 255 -> RESUME(me, sr); BKL_REL(me)
            :: else -> BKL_REL(me)
            fi

        /* ── Process in user mode ── */
        :: p != 255 && pstate[p] == ST_USER ->
            if
            /* ── SYSCALL ── */
            :: true ->
                SYSCALL_ENTRY(p);

                /* Non-deterministic syscall type */
                if
                /* ── fork: reuse freed slot 1 ── */
                :: p == 0 && pstate[1] == ST_DEAD && slot_on_freelist[1] && fork_gen < 3 ->
                    FORK_REUSE(1, 0)

                /* ── fork: first child in slot 1 ── */
                :: p == 0 && pstate[1] == ST_UNUSED && fork_gen == 0 ->
                    atomic {
                        pstate[1] = ST_FORKRET;
                        kstack_cpu[1] = 255;
                        slot[1*5+0] = slot[0*5+0];
                        slot[1*5+1] = slot[0*5+1];
                        slot[1*5+2] = slot[0*5+2];
                        slot[1*5+3] = slot[0*5+3];
                        slot[1*5+4] = V_RAX;
                        fork_gen = 1
                    }

                /* ── exec: replace entry point ── */
                :: true ->
                    DO_EXEC(p)

                /* ── sleep (read/write/wait on pipe) ── */
                :: true ->
                    pstate[p] = ST_SLEEPING;
                    BKL_ACQ(me);
                    DO_SCHED(me, sr);
                    if
                    :: sr != 255 && sr != p ->
                        /* Switched away: this CPU now runs sr, not p.
                         * In real code, context_switch diverges control flow.
                         * p will be resumed by another CPU later. */
                        RESUME(me, sr);
                        BKL_REL(me)
                        /* Skip signal_deliver — p is no longer on this CPU */
                    :: sr == p ->
                        /* Woken immediately (race: wakeup before context_switch).
                         * Continue with signal_deliver below. */
                        BKL_REL(me);
                        goto syscall_epilogue
                    :: sr == 255 ->
                        /* Idle: p is sleeping, CPU goes idle.
                         * p will be woken by another CPU. */
                        BKL_REL(me)
                        /* Skip signal_deliver — p is sleeping, not on this CPU */
                    fi

                /* ── other syscall (no special effect) ── */
                :: true ->
                    goto syscall_epilogue
                fi;
                /* Sleep paths that switched away skip to here (no epilogue) */
                goto skip_epilogue;

syscall_epilogue:
                /* signal_deliver (non-deterministic, after syscall) */
                if
                :: true ->
                    SIG_DELIVER(p);
                    /* sysretq to signal handler */
                    SYSRETQ_CHECK(p);
                    pstate[p] = ST_USER;
                    /* handler eventually calls rt_sigreturn */
                    if
                    :: true ->
                        SYSCALL_ENTRY(p);
                        RT_SIGRETURN(p);
                        SYSRETQ_CHECK(p);
                        pstate[p] = ST_USER
                    :: true -> skip  /* handler still running */
                    fi
                :: true ->
                    /* no signal: normal sysretq */
                    SYSRETQ_CHECK(p);
                    pstate[p] = ST_USER
                fi;
skip_epilogue:

            /* ── EXIT (child only) ── */
            :: p == 1 && fork_gen > 0 ->
                SYSCALL_ENTRY(p);
                /* syscall_entry.S: bkl_acquire_entry (line 44) */
                BKL_ACQ(me);
                /* do_exit auto-reap */
                DO_EXIT_AUTOREAP(me, p);
                /* Now on idle stack. BKL still held from syscall entry.
                 * sched_idle_loop releases it. */
                BKL_REL(me)
                /* CPU is idle, p is ST_DEAD */

            /* ── TIMER PREEMPT ── */
            :: true ->
                TIMER_PUSH(me, p);
                pstate[p] = ST_PREEMPT;

                BKL_ACQ(me);
                DO_SCHED(me, sr);
                if
                :: sr == p ->
                    TIMER_IRETQ(p);
                    pstate[p] = ST_USER;
                    BKL_REL(me)
                :: sr != 255 && sr != p ->
                    RESUME(me, sr);
                    BKL_REL(me)
                :: sr == 255 ->
                    BKL_REL(me)
                fi

            /* ── EXCEPTION (user-mode fault, e.g. CoW #PF) ── */
            :: true ->
                EXCEPTION_PUSH(me, p);
                /* isr_common: bkl_acquire, isr_handler */
                BKL_ACQ(me);

                if
                /* ── Non-fatal exception (CoW handled): iretq resume ── */
                :: true ->
                    EXCEPTION_IRETQ(p);
                    BKL_REL(me)
                    /* pstate stays ST_USER — exception was transparent */

                /* ── Fatal exception with signal handler ── */
                :: true ->
                    SIG_DELIVER_ISR(p);
                    /* iretq to signal handler */
                    EXCEPTION_IRETQ(p);
                    BKL_REL(me)
                    /* pstate stays ST_USER — running handler now */

                /* ── Fatal exception, process killed (SIG_DFL) ── */
                :: p == 1 && fork_gen > 0 ->
                    /* do_exit_from_isr → do_exit auto-reap */
                    DO_EXIT_AUTOREAP(me, p);
                    BKL_REL(me)
                fi
            fi

        :: else -> skip
        fi
    od
}

/* ── Wake sleeping processes ──
 * Models sched_wakeup called by timer tick or by another process's syscall.
 * Can transition ST_SLEEPING → ST_SLEEPING (no change, re-schedulable).
 * In the real kernel, PROC_SLEEPING → PROC_READY.
 * In the model, schedule already picks up ST_SLEEPING processes. */

init {
    byte i;
    for (i : 0 .. NPROC-1) {
        pstate[i] = ST_UNUSED;
        kstack_cpu[i] = 255;
        slot_on_freelist[i] = false;
    };
    for (i : 0 .. NPROC*5-1) { slot[i] = V_ZERO };
    for (i : 0 .. NCPU-1) {
        cpu_proc[i] = 255;
        cpu_tss[i] = 255;
    };
    bkl = false; bkl_who = 255;
    fork_gen = 0;

    /* Boot: proc 0 on CPU 0 */
    atomic {
        pstate[0] = ST_USER;
        cpu_proc[0] = 0;
        cpu_tss[0] = 0;
        kstack_cpu[0] = 0;
    };

    run cpu_thread(0);
    run cpu_thread(1);

#ifdef INJECT_CORRUPTION
    run corruptor()
#endif
}

#ifdef INJECT_CORRUPTION
proctype corruptor() {
    do
    :: true ->
        if
        :: slot[0*5+4] == V_USERIP -> slot[0*5+4] = V_RFLAGS
        :: slot[1*5+4] == V_USERIP -> slot[1*5+4] = V_RFLAGS
        :: slot[0*5+3] == V_USERIP -> slot[0*5+3] = V_RFLAGS
        :: slot[1*5+3] == V_USERIP -> slot[1*5+3] = V_RFLAGS
        :: true -> skip
        fi
    od
}
#endif

/*
 * Properties (inline asserts):
 *   TIMER_IRETQ:     slot[p*5+4] is valid user RIP at every timer iretq.
 *   EXCEPTION_IRETQ: slot[p*5+4] is valid user RIP at every exception iretq.
 *   SYSRETQ_CHECK:   slot[p*5+3] is valid user RIP at every sysretq.
 *
 * Valid user RIP: V_USERIP, V_SIGHAND, or V_EXECIP.
 *
 * Model covers: syscall entry/return, exec, sleep/wake, signal_deliver,
 * signal_deliver_isr, rt_sigreturn, timer preempt push/iretq,
 * exception push/iretq, do_exit auto-reap with slot reuse,
 * fork setup_child_kstack, schedule idle path, BKL.
 */
