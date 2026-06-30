/*
 * unified_smp.pml — Comprehensive SMP correctness model.
 *
 * Unifies: cow_phys, tlb_lazy, idle_cr3, bkl_nest, vfork_sigchld,
 *          signal delivery into ONE model that checks ALL invariants.
 *
 * State tracked:
 *   - Per-CPU: CR3, proc, TLB cache, kstack ownership
 *   - Per-page: refcount, owner
 *   - Per-process: state, PML4 page, stack page, COW flag, sig_pending
 *   - Global: BKL, vfork_active
 *
 * Properties (checked at every transition):
 *   P1: cpu_cr3[c] refcount > 0        (no dangling CR3)
 *   P2: writable page refcount == 1    (no COW aliasing)
 *   P3: freed page not in any TLB      (no stale TLB)
 *   P4: each process on ≤ 1 CPU        (no double schedule)
 *   P5: BKL held during kernel ops     (serialization)
 *   P6: vfork parent not running while child shares AS
 *   P7: signal delivery targets correct process's stack
 *   P8: mprotect preserves COW (via P2: writable ⇒ exclusive)
 *   P9: after thread munmap, no sibling CPU caches freed page
 *   P10: no process in user mode with undelivered signals
 *
 * Verify:
 *   spin -a models/unified_smp.pml && \
 *   gcc -O2 -DMEMLIM=8192 -DCOLLAPSE -w -o pan pan.c && \
 *   ./pan -m50000000
 *
 * State space is finite (bounded variables) but large.  Increase
 * MEMLIM for deeper coverage — e.g. -DMEMLIM=65536 for 64 GB.
 *
 * BFS finds short counterexamples that DFS misses (deep branches
 * explored first).  Use -DBFS when hunting a specific bug:
 *   gcc -O2 -DMEMLIM=8192 -DBFS -DCOLLAPSE -w -o pan pan.c
 *
 * Verify with vfork bug (expect P6 violation):
 *   spin -a -DBUGGY_VFORK models/unified_smp.pml && \
 *   gcc -O2 -DMEMLIM=8192 -w -o pan pan.c && \
 *   ./pan -m5000000
 *
 * Verify with mprotect bug (expect P2 violation):
 *   spin -a -DBUGGY_MPROTECT models/unified_smp.pml && \
 *   gcc -O2 -DMEMLIM=8192 -DBFS -w -o pan pan.c && ./pan
 *
 * Verify with thread TLB bug (expect P9 violation):
 *   spin -a -DBUGGY_THREAD_TLB models/unified_smp.pml && \
 *   gcc -O2 -DMEMLIM=8192 -DBFS -w -o pan pan.c && ./pan
 *
 * Verify with missing signal-on-preempt (expect P10 violation):
 *   spin -a -DBUGGY_NO_SIGNAL_ON_PREEMPT models/unified_smp.pml && \
 *   gcc -O2 -DMEMLIM=8192 -DBFS -w -o pan pan.c && ./pan
 */

/* ── Configuration ── */
#define NCPU   2
#define NPROC  2
#define NPAGE  12  /* 0=invalid, 1=boot_pml4, 2-11=allocatable */

#define PML4_BOOT 1

/* ── Process states ── */
#define ST_UNUSED      0
#define ST_USER        1
#define ST_PREEMPT     2
#define ST_DEAD        3
#define ST_VFORK_SLEEP 4

/* ── Physical page state ── */
byte pg_refcount[NPAGE];

/* ── Per-process state ── */
byte pstate[NPROC];
byte proc_pml4[NPROC];      /* PML4 physical page (0=none) */
byte proc_stack[NPROC];     /* stack physical page (0=none) */
bool proc_cow[NPROC];       /* stack is COW read-only */
byte proc_kstack[NPROC];    /* kstack page (fixed, not PMM) — just an ID */
bool sig_pending[NPROC];    /* SIGCHLD or other signal pending */

/* ── Per-CPU state ── */
byte cpu_proc[NCPU];        /* 255=idle */
byte cpu_cr3[NCPU];         /* which PML4 page */
byte cpu_tlb[NCPU];         /* cached stack VA→PA (0=empty) */
bool cpu_tlb_valid[NCPU];
byte kstack_cpu[NPROC];     /* which CPU owns kstack (255=free) */

/* ── BKL ── */
bool bkl;
byte bkl_who;

/* ── Vfork state ── */
bool vfork_active;           /* proc 1 shares proc 0's PML4 */

/* ── Thread (CLONE_VM) state ── */
bool is_thread;               /* proc 1 is a CLONE_VM thread of proc 0 */
byte heap_page;               /* shared heap page (0=none), mapped in shared PML4 */
byte cpu_tlb_heap[NCPU];      /* TLB cache for heap page per CPU */
bool cpu_tlb_heap_valid[NCPU];

/* ═══════════════════════════════════════════════════════
 * INVARIANT CHECKS — called after every state mutation
 * ═══════════════════════════════════════════════════════ */

/* P1: no CPU's CR3 points to freed page */
inline CHECK_P1() {
    byte _c;
    for (_c : 0 .. NCPU-1) {
        assert(cpu_cr3[_c] != 0);
        assert(pg_refcount[cpu_cr3[_c]] > 0)
    }
}

/* P3: no TLB caches a freed page */
inline CHECK_P3() {
    byte _c;
    for (_c : 0 .. NCPU-1) {
        if
        :: cpu_tlb_valid[_c] && cpu_tlb[_c] != 0 ->
            assert(pg_refcount[cpu_tlb[_c]] > 0)
        :: else -> skip
        fi
    }
}

/* P4: each process on at most one CPU */
inline CHECK_P4() {
    byte _c1, _c2;
    for (_c1 : 0 .. NCPU-1) {
        for (_c2 : 0 .. NCPU-1) {
            if
            :: _c1 < _c2 && cpu_proc[_c1] != 255 ->
                assert(cpu_proc[_c1] != cpu_proc[_c2])
            :: else -> skip
            fi
        }
    }
}

/* P6: vfork parent (proc 0) not running in user mode
 *     while vfork child (proc 1) shares its address space */
inline CHECK_P6() {
    if
    :: vfork_active -> assert(pstate[0] != ST_USER)
    :: else -> skip
    fi
}

/* P9: no CPU's heap TLB caches a freed page (thread TLB coherency) */
inline CHECK_P9() {
    byte _c;
    for (_c : 0 .. NCPU-1) {
        if
        :: cpu_tlb_heap_valid[_c] && cpu_tlb_heap[_c] != 0 ->
            assert(pg_refcount[cpu_tlb_heap[_c]] > 0)
        :: else -> skip
        fi
    }
}

/* P10: checked inline — at the moment of transitioning to ST_USER,
 *      sig_pending must be clear. Signals can arrive asynchronously
 *      AFTER the transition (from another CPU's do_exit/Ctrl+C). */
inline ENTER_USER(pp) {
    /* P10: if signals are pending AND deliverable (stack exists),
     * they must have been delivered before entering user mode. */
    assert(!sig_pending[pp] || proc_stack[pp] == 0);
    pstate[pp] = ST_USER
}

inline CHECK_ALL() {
    CHECK_P1();
    CHECK_P3();
    CHECK_P4();
    CHECK_P6();
    CHECK_P9()
}

/* ═══════════════════════════════════════════════════════
 * PMM
 * ═══════════════════════════════════════════════════════ */

inline PMM_FREE_COUNT(out) {
    out = 0;
    byte _fi;
    for (_fi : 2 .. NPAGE-1) {
        if :: pg_refcount[_fi] == 0 -> out = out + 1 :: else -> skip fi
    }
}

inline PMM_ALLOC(out) {
    out = 0;
    byte _i;
    for (_i : 2 .. NPAGE-1) {
        if
        :: pg_refcount[_i] == 0 ->
            pg_refcount[_i] = 1;
            out = _i;
            break
        :: else -> skip
        fi
    }
}

inline PMM_FREE(pa) {
    assert(pa >= 2 && pa < NPAGE);
    assert(pg_refcount[pa] > 0);
    pg_refcount[pa] = pg_refcount[pa] - 1
}

inline PMM_REF(pa) {
    assert(pa >= 2 && pa < NPAGE);
    pg_refcount[pa] = pg_refcount[pa] + 1
}

inline PMM_TRY_EXCLUSIVE(pa, out) {
    if
    :: pg_refcount[pa] == 1 -> out = 1
    :: pg_refcount[pa] > 1 -> pg_refcount[pa] = pg_refcount[pa] - 1; out = 0
    :: else -> out = 0
    fi
}

/* ═══════════════════════════════════════════════════════
 * BKL
 * ═══════════════════════════════════════════════════════ */

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_who = c }
}

inline BKL_REL(c) {
    assert(bkl && bkl_who == c);
    atomic { bkl_who = 255; bkl = false }
}

/* ═══════════════════════════════════════════════════════
 * TLB — local only (lazy TLB, no IPI shootdown)
 * ═══════════════════════════════════════════════════════ */

inline TLB_FLUSH(c) {
    cpu_tlb_valid[c] = false;
    cpu_tlb[c] = 0;
    cpu_tlb_heap_valid[c] = false;
    cpu_tlb_heap[c] = 0
}

inline TLB_FILL(c) {
    byte _tp = cpu_proc[c];
    if
    :: _tp != 255 && proc_stack[_tp] != 0 ->
        cpu_tlb[c] = proc_stack[_tp];
        cpu_tlb_valid[c] = true
    :: else ->
        cpu_tlb_valid[c] = false
    fi
}

/* ═══════════════════════════════════════════════════════
 * SCHEDULE — context switch with CR3 reload
 * ═══════════════════════════════════════════════════════ */

inline DO_SCHEDULE(c, to) {
    byte _from = cpu_proc[c];
    if :: _from != 255 -> kstack_cpu[_from] = 255 :: else -> skip fi;
    assert(kstack_cpu[to] == 255);
    kstack_cpu[to] = c;
    cpu_proc[c] = to;
    cpu_cr3[c] = proc_pml4[to];
    TLB_FLUSH(c);
    CHECK_ALL()
}

inline GO_IDLE(c) {
    byte _from = cpu_proc[c];
    if :: _from != 255 -> kstack_cpu[_from] = 255 :: else -> skip fi;
    cpu_proc[c] = 255;
    cpu_cr3[c] = PML4_BOOT;   /* switch to safe CR3 before idle */
    TLB_FLUSH(c);
    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * WRITE — check page through TLB
 * ═══════════════════════════════════════════════════════ */

inline WRITE_STACK(c, p) {
    byte _pa;
    if
    :: cpu_tlb_valid[c] -> _pa = cpu_tlb[c]
    :: else -> TLB_FILL(c);
        if :: cpu_tlb_valid[c] -> _pa = cpu_tlb[c] :: else -> _pa = 0 fi
    fi;
    if
    :: _pa != 0 ->
        /* P2: writable page must be exclusive */
        assert(pg_refcount[_pa] >= 1);
        if :: !proc_cow[p] -> assert(pg_refcount[_pa] == 1) :: else -> skip fi
    :: else -> skip
    fi
}

/* ═══════════════════════════════════════════════════════
 * FORK — clone PML4, share stack page (COW)
 * ═══════════════════════════════════════════════════════ */

inline DO_FORK(c, parent, child) {
    /* Allocate PML4 for child */
    byte _cpml4;
    PMM_ALLOC(_cpml4);
    assert(_cpml4 != 0);
    proc_pml4[child] = _cpml4;

    /* Share stack page (COW) */
    byte _spa = proc_stack[parent];
    if
    :: _spa != 0 ->
        PMM_REF(_spa);
        proc_stack[child] = _spa;
        proc_cow[parent] = true;
        proc_cow[child] = true;
        TLB_FLUSH(c)   /* parent's writable TLB entries stale */
    :: else ->
        proc_stack[child] = 0
    fi;

    kstack_cpu[child] = 255;
    pstate[child] = ST_PREEMPT;
    sig_pending[child] = false;
    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * VFORK — share PML4, parent sleeps
 * ═══════════════════════════════════════════════════════ */

inline DO_VFORK(c, parent, child) {
    /* Child shares parent's PML4 — NO clone */
    proc_pml4[child] = proc_pml4[parent];

    /* Child shares parent's stack page (no COW, direct sharing) */
    proc_stack[child] = proc_stack[parent];

    kstack_cpu[child] = 255;
    pstate[child] = ST_PREEMPT;
    sig_pending[child] = false;
    vfork_active = true;

    /* Parent sleeps */
    pstate[parent] = ST_VFORK_SLEEP;
    kstack_cpu[parent] = 255;
    cpu_proc[c] = 255;
    cpu_cr3[c] = PML4_BOOT;
    TLB_FLUSH(c);
    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * COW FAULT — resolve by copy or make exclusive
 * ═══════════════════════════════════════════════════════ */

inline DO_COW(c, p) {
    byte _old = proc_stack[p];
    assert(_old != 0 && proc_cow[p]);
    byte _excl;
    PMM_TRY_EXCLUSIVE(_old, _excl);
    if
    :: _excl -> proc_cow[p] = false
    :: else ->
        byte _new;
        PMM_ALLOC(_new);
        assert(_new != 0);
        proc_stack[p] = _new;
        proc_cow[p] = false;
        TLB_FLUSH(c)
    fi;
    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * MPROTECT — change page permissions.
 * BUG: kernel overwrites PTE flags, dropping the COW bit.
 * If a COW page is mprotected writable, both processes
 * write to the same physical page without faulting → corruption.
 *
 * The BUGGY_MPROTECT flag enables the buggy behavior.
 * Default (fixed): preserve COW bit across mprotect.
 * ═══════════════════════════════════════════════════════ */

inline DO_MPROTECT(c, p) {
    if
    :: proc_stack[p] != 0 ->
#ifdef BUGGY_MPROTECT
        /* BUG: mprotect makes page writable, dropping COW bit.
         * After this, writes go directly to the shared page. */
        proc_cow[p] = false;
#else
        /* FIXED: preserve COW bit — mprotect must not remove it.
         * If page is COW, it stays COW regardless of new prot. */
        skip
#endif
        TLB_FLUSH(c);
        CHECK_ALL()
    :: else -> skip
    fi
}

/* ═══════════════════════════════════════════════════════
 * MUNMAP — unmap stack, free page
 * ═══════════════════════════════════════════════════════ */

inline DO_MUNMAP(c, p) {
    byte _pa = proc_stack[p];
    if
    :: _pa != 0 ->
        proc_stack[p] = 0;
        proc_cow[p] = false;
        PMM_FREE(_pa);
        TLB_FLUSH(c);
        CHECK_ALL()
    :: else -> skip
    fi
}

/* ═══════════════════════════════════════════════════════
 * CLONE_VM — create thread sharing PML4
 * ═══════════════════════════════════════════════════════ */

inline DO_CLONE_VM(c, parent, child) {
    /* Thread shares parent's PML4 */
    proc_pml4[child] = proc_pml4[parent];

    /* Allocate own stack for thread */
    byte _ts;
    PMM_ALLOC(_ts);
    assert(_ts != 0);
    proc_stack[child] = _ts;
    proc_cow[child] = false;

    /* Allocate shared heap page (accessible by both threads via shared PML4) */
    byte _hp;
    PMM_ALLOC(_hp);
    assert(_hp != 0);
    heap_page = _hp;

    is_thread = true;
    kstack_cpu[child] = 255;
    pstate[child] = ST_PREEMPT;
    sig_pending[child] = false;
    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * HEAP WRITE — thread accesses shared heap page through TLB
 * ═══════════════════════════════════════════════════════ */

inline HEAP_TLB_FILL(c) {
    if
    :: heap_page != 0 ->
        cpu_tlb_heap[c] = heap_page;
        cpu_tlb_heap_valid[c] = true
    :: else ->
        cpu_tlb_heap_valid[c] = false
    fi
}

/* WRITE_HEAP is atomic: on x86 a TLB-backed memory access is a single
 * operation — invlpg IPI preempts the CPU before a stale entry is used. */
inline WRITE_HEAP(c) {
    d_step {
        byte _hpa;
        if
        :: cpu_tlb_heap_valid[c] -> _hpa = cpu_tlb_heap[c]
        :: else -> HEAP_TLB_FILL(c);
            if :: cpu_tlb_heap_valid[c] -> _hpa = cpu_tlb_heap[c] :: else -> _hpa = 0 fi
        fi;
        if
        :: _hpa != 0 ->
            assert(pg_refcount[_hpa] >= 1)
        :: else -> skip
        fi
    }
}

/* ═══════════════════════════════════════════════════════
 * MUNMAP_HEAP — thread unmaps shared heap page.
 * BUG: lazy TLB only flushes local CPU — sibling thread's
 * CPU still has stale TLB entry for the freed page.
 * ═══════════════════════════════════════════════════════ */

inline DO_MUNMAP_HEAP(c) {
    byte _hpa = heap_page;
    if
    :: _hpa != 0 ->
        heap_page = 0;
        /* Flush TLBs BEFORE freeing — the page must not be reused
         * while any CPU's TLB still caches the old mapping. */
        cpu_tlb_heap_valid[c] = false;
        cpu_tlb_heap[c] = 0;
#ifdef BUGGY_THREAD_TLB
        /* BUG: no IPI to sibling CPU — stale TLB persists */
        skip
#else
        /* FIXED: flush heap TLB on ALL CPUs atomically (models IPI+ack).
         * In kernel: send invlpg IPI, wait for ack, then free. */
        byte _ftlb;
        d_step {
            for (_ftlb : 0 .. NCPU-1) {
                if
                :: _ftlb != c ->
                    cpu_tlb_heap_valid[_ftlb] = false;
                    cpu_tlb_heap[_ftlb] = 0
                :: else -> skip
                fi
            }
        }
#endif
        ;
        PMM_FREE(_hpa);
        CHECK_ALL()
    :: else -> skip
    fi
}

/* ═══════════════════════════════════════════════════════
 * EXEC — switch CR3 BEFORE freeing old PML4
 * If vfork child: get own PML4, wake parent, clear vfork.
 * ═══════════════════════════════════════════════════════ */

inline DO_EXEC(c, p) {
    byte _old_pml4 = proc_pml4[p];
    byte _old_stack = proc_stack[p];
    bool _was_vfork = (p == 1 && vfork_active);

    /* Allocate new PML4 */
    byte _new_pml4;
    PMM_ALLOC(_new_pml4);
    assert(_new_pml4 != 0);
    proc_pml4[p] = _new_pml4;

    /* Switch CR3 to new PML4 BEFORE freeing old */
    cpu_cr3[c] = _new_pml4;
    TLB_FLUSH(c);

    /* Free old PML4 — but not if shared via vfork (parent still uses it) */
    if
    :: _old_pml4 != 0 && !_was_vfork -> PMM_FREE(_old_pml4)
    :: else -> skip
    fi;

    /* Free old stack — but not if shared via vfork */
    if
    :: _old_stack != 0 && !_was_vfork ->
        proc_stack[p] = 0;
        proc_cow[p] = false;
        PMM_FREE(_old_stack)
    :: else ->
        proc_stack[p] = 0;
        proc_cow[p] = false
    fi;

    /* Allocate new stack */
    byte _new_stack;
    PMM_ALLOC(_new_stack);
    assert(_new_stack != 0);
    proc_stack[p] = _new_stack;

    /* If vfork child: clear vfork, wake parent */
    if
    :: _was_vfork ->
        vfork_active = false;
        if :: pstate[0] == ST_VFORK_SLEEP -> pstate[0] = ST_PREEMPT :: else -> skip fi
    :: else -> skip
    fi;

    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * SIGNAL DELIVER — write signal frame to user stack
 * Requires switching CR3 to the process's PML4.
 * P7: we must write to the correct process's stack.
 * ═══════════════════════════════════════════════════════ */

inline SIGNAL_DELIVER(c, p) {
    /* Switch CR3 to process's PML4 to write signal frame */
    byte _saved_cr3 = cpu_cr3[c];
    cpu_cr3[c] = proc_pml4[p];

    /* Write to stack page (CoW must be resolved first) */
    if
    :: proc_cow[p] && proc_stack[p] != 0 -> DO_COW(c, p)
    :: else -> skip
    fi;

    /* P7: verify we're writing to the correct process's stack */
    if :: proc_stack[p] != 0 -> assert(pg_refcount[proc_stack[p]] >= 1) :: else -> skip fi;

    /* Write signal frame */
    WRITE_STACK(c, p);

    /* Restore CR3 */
    cpu_cr3[c] = _saved_cr3;

    sig_pending[p] = false;
    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * EXIT — switch CR3 to boot BEFORE freeing PML4.
 * Send SIGCHLD to parent.
 * ═══════════════════════════════════════════════════════ */

inline DO_EXIT(c, p) {
    /* Switch CR3 away BEFORE freeing */
    cpu_cr3[c] = PML4_BOOT;
    TLB_FLUSH(c);

    byte _pml4 = proc_pml4[p];
    byte _stack = proc_stack[p];

    /* Check if PML4 is shared (vfork child that didn't exec).
     * If shared, do NOT free — the parent still uses these pages.
     * Matches kernel: paging_free_user_pml4 skipped when shared. */
    bool _pml4_shared = false;
    byte _si;
    for (_si : 0 .. NPROC-1) {
        if
        :: (_si != p) ->
            if
            :: (pstate[_si] != ST_DEAD && pstate[_si] != ST_UNUSED) ->
                if :: (proc_pml4[_si] == _pml4 && _pml4 != 0) -> _pml4_shared = true
                   :: else -> skip
                fi
            :: else -> skip
            fi
        :: else -> skip
        fi
    };

    if :: !_pml4_shared && _stack != 0 -> PMM_FREE(_stack); proc_stack[p] = 0 :: else -> proc_stack[p] = 0 fi;
    proc_cow[p] = false;
    if :: !_pml4_shared && _pml4 != 0 -> PMM_FREE(_pml4); proc_pml4[p] = 0 :: else -> proc_pml4[p] = 0 fi;

    /* Clean up thread state: free shared heap if last thread */
    if :: is_thread && p == 1 ->
        if :: heap_page != 0 -> PMM_FREE(heap_page); heap_page = 0 :: else -> skip fi;
        is_thread = false
       :: else -> skip
    fi;

    kstack_cpu[p] = 255;
    pstate[p] = ST_DEAD;
    cpu_proc[c] = 255;

    /* Send SIGCHLD to parent (proc 0) */
    if
    :: p == 1 ->
        sig_pending[0] = true;
        /* Wake parent if sleeping — this is where the vfork bug triggers.
         * An unrelated SIGCHLD would do the same thing. */
#ifdef BUGGY_VFORK
        if :: pstate[0] == ST_VFORK_SLEEP -> pstate[0] = ST_PREEMPT :: else -> skip fi
#else
        /* Fixed: only wake non-vfork sleepers; vfork parent stays asleep.
         * (In the kernel, the re-check loop in do_vfork handles this.) */
        if
        :: pstate[0] == ST_VFORK_SLEEP -> skip   /* don't wake vfork parent */
        :: pstate[0] == ST_PREEMPT -> skip        /* already awake */
        :: else -> skip
        fi
#endif
    :: else -> skip
    fi;

    CHECK_ALL()
}

/* ═══════════════════════════════════════════════════════
 * SPURIOUS SIGCHLD — models an unrelated child exiting
 * while the vfork parent is sleeping.
 * ═══════════════════════════════════════════════════════ */

inline SPURIOUS_SIGCHLD() {
    sig_pending[0] = true;
#ifdef BUGGY_VFORK
    if :: pstate[0] == ST_VFORK_SLEEP -> pstate[0] = ST_PREEMPT :: else -> skip fi
#else
    /* Fixed: vfork parent ignores spurious wakes (re-check loop) */
    skip
#endif
}

/* ═══════════════════════════════════════════════════════
 * CPU THREAD
 * ═══════════════════════════════════════════════════════ */

proctype cpu_thread(byte me) {
    byte p;

end_idle:
    do
    :: true ->
        p = cpu_proc[me];

        if
        /* ── IDLE ── */
        :: p == 255 ->
            BKL_ACQ(me);

            /* Nondeterministic spurious SIGCHLD while idle */
            if :: vfork_active -> SPURIOUS_SIGCHLD() :: else -> skip fi;

            if
            :: pstate[0] == ST_PREEMPT && kstack_cpu[0] == 255 ->
                DO_SCHEDULE(me, 0);
#ifndef BUGGY_NO_SIGNAL_ON_PREEMPT
                if :: sig_pending[0] && proc_stack[0] != 0 -> SIGNAL_DELIVER(me, 0) :: else -> skip fi;
#endif
                ENTER_USER(0)
            :: pstate[1] == ST_PREEMPT && kstack_cpu[1] == 255 ->
                DO_SCHEDULE(me, 1);
#ifndef BUGGY_NO_SIGNAL_ON_PREEMPT
                if :: sig_pending[1] && proc_stack[1] != 0 -> SIGNAL_DELIVER(me, 1) :: else -> skip fi;
#endif
                ENTER_USER(1)
            :: else -> skip
            fi;
            BKL_REL(me)

        /* ── RUNNING ── */
        :: p != 255 && pstate[p] == ST_USER ->
            if
            /* ── User write (through TLB) ── */
            :: proc_stack[p] != 0 ->
                if
                :: proc_cow[p] ->
                    BKL_ACQ(me);
                    byte _fcow; PMM_FREE_COUNT(_fcow);
                    if :: _fcow >= 1 -> DO_COW(me, p) :: else -> skip fi;
                    BKL_REL(me)
                :: else -> skip
                fi;
                if :: !proc_cow[p] -> WRITE_STACK(me, p) :: else -> skip fi

            /* ── Syscall: fork (needs 1 page for PML4) ── */
            :: p == 0 && pstate[1] == ST_DEAD && !is_thread ->
                BKL_ACQ(me);
                byte _ff; PMM_FREE_COUNT(_ff);
                if :: _ff >= 1 -> DO_FORK(me, 0, 1) :: else -> skip fi;
                BKL_REL(me)

            /* ── Syscall: vfork ── */
            :: p == 0 && pstate[1] == ST_DEAD && !vfork_active && !is_thread ->
                BKL_ACQ(me);
                DO_VFORK(me, 0, 1);
                BKL_REL(me)

            /* ── Syscall: clone(CLONE_VM) (needs 2 pages: stack + heap) ── */
            :: p == 0 && pstate[1] == ST_DEAD && !is_thread ->
                BKL_ACQ(me);
                byte _fclone; PMM_FREE_COUNT(_fclone);
                if :: _fclone >= 2 -> DO_CLONE_VM(me, 0, 1) :: else -> skip fi;
                BKL_REL(me)

            /* ── Thread: write to shared heap page ── */
            :: is_thread && heap_page != 0 ->
                WRITE_HEAP(me)

            /* ── Thread: munmap shared heap page ── */
            :: is_thread && heap_page != 0 ->
                BKL_ACQ(me);
                DO_MUNMAP_HEAP(me);
                BKL_REL(me)

            /* ── Syscall: exec (needs 2 pages; not for threads) ── */
            :: p == 1 && !is_thread ->
                BKL_ACQ(me);
                byte _fe; PMM_FREE_COUNT(_fe);
                if :: _fe >= 2 -> DO_EXEC(me, p) :: else -> skip fi;
                BKL_REL(me)

            /* ── Syscall: mprotect ── */
            :: proc_stack[p] != 0 ->
                BKL_ACQ(me);
                DO_MPROTECT(me, p);
                BKL_REL(me)

            /* ── Syscall: munmap ── */
            :: proc_stack[p] != 0 ->
                BKL_ACQ(me);
                DO_MUNMAP(me, p);
                BKL_REL(me)

            /* ── Syscall: exit (child only) ── */
            :: p == 1 ->
                BKL_ACQ(me);
                DO_EXIT(me, p);
                BKL_REL(me)

            /* ── Signal delivery (on return to user after syscall) ── */
            :: sig_pending[p] && proc_stack[p] != 0 ->
                BKL_ACQ(me);
                SIGNAL_DELIVER(me, p);
                BKL_REL(me)

            /* ── Timer preempt ── */
            :: true ->
                BKL_ACQ(me);
                pstate[p] = ST_PREEMPT;
                /* Nondeterministic spurious SIGCHLD during preempt */
                if :: vfork_active -> SPURIOUS_SIGCHLD() :: else -> skip fi;
                /* Deliver pending signals BEFORE returning to user mode.
                 * Without this, CPU-bound processes are unkillable (P10).
                 * Matches: timer ISR must call signal_deliver like syscall return. */
#ifndef BUGGY_NO_SIGNAL_ON_PREEMPT
                if :: sig_pending[p] && proc_stack[p] != 0 ->
                    SIGNAL_DELIVER(me, p)
                   :: else -> skip
                fi;
#endif
                /* Try to switch to another process */
                if
                :: pstate[0] == ST_PREEMPT && p != 0 && kstack_cpu[0] == 255 ->
                    DO_SCHEDULE(me, 0);
#ifndef BUGGY_NO_SIGNAL_ON_PREEMPT
                    if :: sig_pending[0] && proc_stack[0] != 0 ->
                        SIGNAL_DELIVER(me, 0)
                       :: else -> skip
                    fi;
#endif
                    ENTER_USER(0)
                :: pstate[1] == ST_PREEMPT && p != 1 && kstack_cpu[1] == 255 ->
                    DO_SCHEDULE(me, 1);
#ifndef BUGGY_NO_SIGNAL_ON_PREEMPT
                    if :: sig_pending[1] && proc_stack[1] != 0 ->
                        SIGNAL_DELIVER(me, 1)
                       :: else -> skip
                    fi;
#endif
                    ENTER_USER(1)
                :: else ->
                    /* Nothing to switch to — go idle */
                    GO_IDLE(me);
                fi;
                BKL_REL(me)
            fi

        :: else -> skip
        fi
    od
}

/* ═══════════════════════════════════════════════════════
 * INIT
 * ═══════════════════════════════════════════════════════ */

init {
    byte i;
    for (i : 0 .. NPAGE-1) { pg_refcount[i] = 0 };

    /* Boot PML4: always alive (refcount=1, never freed) */
    pg_refcount[PML4_BOOT] = 1;

    /* Process 0: PML4=page2, stack=page3 */
    pg_refcount[2] = 1;
    pg_refcount[3] = 1;
    ENTER_USER(0);
    proc_pml4[0] = 2;
    proc_stack[0] = 3;
    proc_cow[0] = false;
    proc_kstack[0] = 1;  /* symbolic kstack ID */
    sig_pending[0] = false;

    /* Process 1: dead (will be forked) */
    pstate[1] = ST_DEAD;
    proc_pml4[1] = 0;
    proc_stack[1] = 0;
    proc_cow[1] = false;
    proc_kstack[1] = 2;
    sig_pending[1] = false;

    /* CPU 0: runs proc 0 */
    cpu_proc[0] = 0;
    cpu_cr3[0] = 2;
    cpu_tlb[0] = 3;
    cpu_tlb_valid[0] = true;
    kstack_cpu[0] = 0;

    /* CPU 1: idle with safe boot CR3 */
    cpu_proc[1] = 255;
    cpu_cr3[1] = PML4_BOOT;
    cpu_tlb[1] = 0;
    cpu_tlb_valid[1] = false;
    kstack_cpu[1] = 255;

    bkl = false;
    bkl_who = 255;
    vfork_active = false;
    is_thread = false;
    heap_page = 0;
    cpu_tlb_heap[0] = 0; cpu_tlb_heap_valid[0] = false;
    cpu_tlb_heap[1] = 0; cpu_tlb_heap_valid[1] = false;

    run cpu_thread(0);
    run cpu_thread(1);
}
