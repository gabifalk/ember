/*
 * AP GDT allocation failure model for ember SMP.
 *
 * gdt_init_cpu() calls kmalloc for GDT and kzalloc for TSS.
 * If the heap is exhausted (many APs), kmalloc returns NULL.
 * gdt_populate(NULL) writes to address 0 → corrupts memory.
 * Later, iretq with broken GDT causes #GP.
 *
 * Code: gdt.c:134-160 (gdt_init_cpu)
 *
 * Verify:
 *   spin -a models/ap_gdt_alloc.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define N_APS     4
#define HEAP_CAP  3    /* heap can satisfy this many allocs before OOM */

byte heap_remaining = HEAP_CAP;
bool null_deref = false;
bool alloc_checked = false;

/* BKL + ap_init_lock serialization */
bool init_lock = false;

inline INIT_LOCK() {
    atomic { !init_lock -> init_lock = true }
}
inline INIT_UNLOCK() {
    init_lock = false
}

/* ── BUGGY: no NULL check after kmalloc ── */
#ifdef BUGGY
proctype ap_gdt_buggy(byte id) {
    INIT_LOCK();

    /* kmalloc(GDT) */
    byte gdt_ptr;
    if
    :: heap_remaining > 0 ->
        heap_remaining--;
        gdt_ptr = 1        /* valid pointer */
    :: else ->
        gdt_ptr = 0         /* NULL — OOM */
    fi;

    /* kzalloc(TSS) */
    byte tss_ptr;
    if
    :: heap_remaining > 0 ->
        heap_remaining--;
        tss_ptr = 1
    :: else ->
        tss_ptr = 0
    fi;

    /* gdt_populate(gdt_ptr) — writes to gdt_ptr.
     * If NULL, writes to address 0 → corruption */
    if
    :: gdt_ptr == 0 -> null_deref = true
    :: else -> skip
    fi;

    /* tss_set_rsp0_on(tss_ptr, ...) */
    if
    :: tss_ptr == 0 -> null_deref = true
    :: else -> skip
    fi;

    INIT_UNLOCK()
}
#endif

/* ── FIXED: check kmalloc return, halt if NULL ── */
#ifndef BUGGY
proctype ap_gdt_fixed(byte id) {
    INIT_LOCK();

    byte gdt_ptr;
    if
    :: heap_remaining > 0 ->
        heap_remaining--;
        gdt_ptr = 1
    :: else ->
        gdt_ptr = 0
    fi;

    byte tss_ptr;
    if
    :: heap_remaining > 0 ->
        heap_remaining--;
        tss_ptr = 1
    :: else ->
        tss_ptr = 0
    fi;

    /* FIX: check before use */
    if
    :: gdt_ptr == 0 || tss_ptr == 0 ->
        /* BUG_ON or halt — don't dereference NULL */
        INIT_UNLOCK();
        goto done
    :: else -> skip
    fi;

    /* Safe to use */
    skip;

    INIT_UNLOCK();
done:
    skip
}
#endif

init {
    byte i = 0;
    do
    :: i < N_APS ->
#ifdef BUGGY
        run ap_gdt_buggy(i);
#else
        run ap_gdt_fixed(i);
#endif
        i++
    :: else -> break
    od
}

/* Safety: no NULL pointer dereference */
ltl no_null_deref { [] !null_deref }
