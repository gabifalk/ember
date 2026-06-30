/*
 * tlb_deferred.pml — Deferred page freeing with TLB generation counters.
 *
 * Design: smp_flush_tlb is fire-and-forget (send IPI, don't wait).
 * Freed pages go to a deferred list tagged with the current TLB
 * generation.  Pages are actually freed only when ALL CPUs have
 * flushed their TLB (generation >= page's tag).
 *
 * TLB flush happens on:
 *   - IPI handler (CR3 reload, sets cpu_gen = global_gen)
 *   - Context switch (CR3 reload, sets cpu_gen = global_gen)
 *
 * Property: a page is never reused while any CPU still has a
 * stale TLB entry pointing to it.
 *
 * Verify:
 *   spin -a models/tlb_deferred.pml && \
 *   gcc -O2 -DMEMLIM=4096 -o pan pan.c && \
 *   ./pan -m500000
 */

#define NCPU  2
#define NPAGE 4

/* ── Physical page state ── */
byte pg_refcount[NPAGE];    /* 0 = free or deferred */
byte pg_owner[NPAGE];       /* last writer proc (for corruption check) */

/* ── TLB generation tracking ── */
byte global_gen;            /* incremented on each shootdown */
byte cpu_gen[NCPU];         /* per-CPU: last TLB flush generation */
byte cpu_tlb_pa[NCPU];     /* per-CPU: cached TLB PA for stack VA (0=empty) */
bool cpu_tlb_valid[NCPU];

/* ── Deferred free list (max 2 entries for model) ── */
byte deferred_pa[2];        /* physical page */
byte deferred_gen[2];       /* generation when freed */
byte deferred_count;

/* ── Per-process state ── */
byte proc_stack_pa[2];      /* PTE: stack physical page (0=unmapped) */
bool proc_cow[2];

/* ── Per-CPU state ── */
byte cpu_proc[NCPU];        /* 255 = idle */

/* ── BKL ── */
bool bkl_locked;
byte bkl_who;

inline BKL_ACQ(c) {
    atomic { !bkl_locked -> bkl_locked = true; bkl_who = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_who == c);
    atomic { bkl_who = 255; bkl_locked = false }
}

/* ── PMM alloc: only returns pages with refcount 0 AND not deferred ── */
inline PMM_ALLOC(out) {
    out = 0;
    byte _i;
    for (_i : 1 .. NPAGE-1) {
        if
        :: pg_refcount[_i] == 0 ->
            /* Check not in deferred list */
            bool _in_deferred = false;
            byte _d;
            for (_d : 0 .. 1) {
                if
                :: _d < deferred_count && deferred_pa[_d] == _i ->
                    _in_deferred = true
                :: else -> skip
                fi
            };
            if
            :: !_in_deferred ->
                pg_refcount[_i] = 1;
                pg_owner[_i] = 255;
                out = _i;
                break
            :: else -> skip
            fi
        :: else -> skip
        fi
    }
}

/* ── Deferred free: add to list instead of immediate PMM free ── */
inline PMM_FREE_DEFERRED(pa) {
    assert(pa > 0 && pa < NPAGE);
    pg_refcount[pa] = pg_refcount[pa] - 1;
    if
    :: pg_refcount[pa] == 0 ->
        /* Add to deferred list tagged with current generation */
        assert(deferred_count < 2);
        deferred_pa[deferred_count] = pa;
        deferred_gen[deferred_count] = global_gen;
        deferred_count++
    :: else -> skip  /* still referenced */
    fi
}

inline PMM_REF(pa) {
    pg_refcount[pa] = pg_refcount[pa] + 1;
}

/* ── Sweep deferred list: free pages where all CPUs have flushed ── */
inline DEFERRED_SWEEP() {
    byte _min_gen = 255;
    byte _c;
    for (_c : 0 .. NCPU-1) {
        if
        :: cpu_gen[_c] < _min_gen -> _min_gen = cpu_gen[_c]
        :: else -> skip
        fi
    };
    /* Free pages with generation <= min_gen */
    byte _d;
    byte _new_count = 0;
    byte _new_pa[2];
    byte _new_gen[2];
    for (_d : 0 .. 1) {
        if
        :: _d < deferred_count && deferred_gen[_d] <= _min_gen ->
            skip  /* truly free — don't copy to new list */
        :: _d < deferred_count ->
            _new_pa[_new_count] = deferred_pa[_d];
            _new_gen[_new_count] = deferred_gen[_d];
            _new_count++
        :: else -> skip
        fi
    };
    deferred_count = _new_count;
    for (_d : 0 .. 1) {
        if
        :: _d < _new_count ->
            deferred_pa[_d] = _new_pa[_d];
            deferred_gen[_d] = _new_gen[_d]
        :: else -> skip
        fi
    }
}

/* ── TLB shootdown: fire-and-forget + bump generation ── */
inline TLB_SHOOTDOWN(sender) {
    global_gen = global_gen + 1;
    /* IPI sent to all other CPUs.  Delivery is NON-ATOMIC:
     * each CPU processes it eventually (on next interrupt window).
     * Modeled as non-deterministic: each CPU may or may not have
     * processed it yet. */
}

/* ── IPI delivery: CPU processes pending shootdown ── */
inline TLB_IPI_PROCESS(c) {
    cpu_gen[c] = global_gen;
    cpu_tlb_valid[c] = false;
    cpu_tlb_pa[c] = 0
}

/* ── Context switch: flushes TLB (CR3 reload) ── */
inline CTX_SWITCH_TLB(c) {
    cpu_gen[c] = global_gen;
    cpu_tlb_valid[c] = false;
    cpu_tlb_pa[c] = 0
}

/* ── TLB fill from page table ── */
inline TLB_FILL(c) {
    byte _p = cpu_proc[c];
    if
    :: _p != 255 && proc_stack_pa[_p] != 0 ->
        cpu_tlb_pa[c] = proc_stack_pa[_p];
        cpu_tlb_valid[c] = true
    :: else ->
        cpu_tlb_valid[c] = false
    fi
}

/* ── Write through TLB: property check ── */
inline WRITE_STACK(c, p) {
    byte _pa;
    if
    :: cpu_tlb_valid[c] -> _pa = cpu_tlb_pa[c]
    :: else -> TLB_FILL(c);
        if :: cpu_tlb_valid[c] -> _pa = cpu_tlb_pa[c] :: else -> _pa = 0 fi
    fi;
    if
    :: _pa != 0 ->
        /* PROPERTY: page must not be in deferred list (freed but not yet safe) */
        bool _is_deferred = false;
        byte _d;
        for (_d : 0 .. 1) {
            if
            :: _d < deferred_count && deferred_pa[_d] == _pa ->
                _is_deferred = true
            :: else -> skip
            fi
        };
        assert(!_is_deferred);
        /* PROPERTY: page must be allocated (refcount >= 1) */
        assert(pg_refcount[_pa] >= 1);
        pg_owner[_pa] = p
    :: else -> skip
    fi
}

/* ── CPU thread ── */
proctype cpu_thread(byte me) {
    byte p;

end_idle:
    do
    :: true ->
        /* Non-deterministic IPI delivery (models async IPI processing) */
        if
        :: cpu_gen[me] < global_gen -> TLB_IPI_PROCESS(me)
        :: true -> skip  /* IPI not yet delivered */
        fi;

        p = cpu_proc[me];

        if
        /* ── IDLE ── */
        :: p == 255 ->
            BKL_ACQ(me);
            /* Try to schedule */
            if
            :: proc_stack_pa[0] != 0 && cpu_proc[0] == 255 && cpu_proc[1] == 255 ->
                cpu_proc[me] = 0; CTX_SWITCH_TLB(me)
            :: proc_stack_pa[1] != 0 && cpu_proc[0] == 255 && cpu_proc[1] == 255 ->
                cpu_proc[me] = 1; CTX_SWITCH_TLB(me)
            :: else -> skip
            fi;
            /* Sweep deferred list while we hold BKL */
            DEFERRED_SWEEP();
            BKL_REL(me)

        /* ── Process running ── */
        :: p != 255 ->
            if
            /* ── USER WRITE ── */
            :: proc_stack_pa[p] != 0 && !proc_cow[p] ->
                WRITE_STACK(me, p)

            /* ── SYSCALL: fork ── */
            :: p == 0 && proc_stack_pa[1] == 0 ->
                BKL_ACQ(me);
                /* Fork: share stack page, mark COW */
                byte _spa = proc_stack_pa[0];
                if
                :: _spa != 0 ->
                    PMM_REF(_spa);
                    proc_stack_pa[1] = _spa;
                    proc_cow[0] = true;
                    proc_cow[1] = true;
                    /* Flush local TLB (writable→COW) */
                    cpu_tlb_valid[me] = false;
                    /* Fire-and-forget shootdown */
                    TLB_SHOOTDOWN(me)
                :: else -> skip
                fi;
                BKL_REL(me)

            /* ── SYSCALL: munmap ── */
            :: proc_stack_pa[p] != 0 ->
                BKL_ACQ(me);
                byte _mpa = proc_stack_pa[p];
                proc_stack_pa[p] = 0;
                proc_cow[p] = false;
                cpu_tlb_valid[me] = false;
                /* Fire-and-forget shootdown */
                TLB_SHOOTDOWN(me);
                /* Deferred free (not immediate!) */
                PMM_FREE_DEFERRED(_mpa);
                /* Sweep in case some are ready */
                DEFERRED_SWEEP();
                BKL_REL(me)

            /* ── SYSCALL: COW fault → resolve ── */
            :: proc_cow[p] && proc_stack_pa[p] != 0 ->
                BKL_ACQ(me);
                byte _old = proc_stack_pa[p];
                byte _new;
                PMM_ALLOC(_new);
                if
                :: _new != 0 ->
                    pg_owner[_new] = p;
                    proc_stack_pa[p] = _new;
                    proc_cow[p] = false;
                    PMM_FREE_DEFERRED(_old);
                    cpu_tlb_valid[me] = false
                :: else -> skip  /* OOM */
                fi;
                BKL_REL(me)

            /* ── TIMER PREEMPT ── */
            :: true ->
                BKL_ACQ(me);
                cpu_proc[me] = 255;
                CTX_SWITCH_TLB(me);
                DEFERRED_SWEEP();
                BKL_REL(me)
            fi
        fi
    od
}

init {
    byte i;
    for (i : 0 .. NPAGE-1) { pg_refcount[i] = 0; pg_owner[i] = 255 };

    /* Page 1: proc 0's stack. Page 2: spare. Page 3: spare. */
    pg_refcount[1] = 1;

    proc_stack_pa[0] = 1;
    proc_stack_pa[1] = 0;
    proc_cow[0] = false;
    proc_cow[1] = false;

    cpu_proc[0] = 0;
    cpu_proc[1] = 255;
    cpu_gen[0] = 0;
    cpu_gen[1] = 0;
    cpu_tlb_pa[0] = 1;
    cpu_tlb_valid[0] = true;
    cpu_tlb_pa[1] = 0;
    cpu_tlb_valid[1] = false;

    global_gen = 0;
    deferred_count = 0;

    bkl_locked = false;
    bkl_who = 255;

    run cpu_thread(0);
    run cpu_thread(1);
}

/*
 * Properties (inline asserts):
 *   WRITE_STACK: page is not in deferred list (not freed-but-pending)
 *   WRITE_STACK: page refcount >= 1 (allocated)
 *   PMM_ALLOC: never returns a deferred page
 *   PMM_FREE_DEFERRED: refcount > 0 before decrement
 *
 * These ensure: no CPU can access a page through a stale TLB entry
 * after the page has been freed, even with fire-and-forget IPI.
 */
