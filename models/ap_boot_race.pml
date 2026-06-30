/*
 * AP boot initialization race model for ember SMP.
 *
 * Models concurrent AP startup (smp.c:98-175).  Multiple APs execute
 * ap_entry_64() simultaneously WITHOUT the BKL.  They call:
 *   1. gdt_init_cpu()    — kmalloc/kzalloc (heap + PMM, cli/sti spinlock)
 *   2. sched_init_idle() — per-CPU array (safe, indexed by cpu_id)
 *   3. lapic_init()      — BSP-only mapping (fixed), per-CPU LAPIC config
 *
 * The bug: spinlock_t is cli/sti only (not SMP-safe).  Multiple APs
 * calling kmalloc concurrently corrupt the heap free list and PMM bitmap.
 *
 * Shared resources:
 *   - heap_free_list: linked list of free blocks, protected by cli/sti
 *   - pmm_bitmap: page allocation bitmap, protected by cli/sti
 *   Both are global — cli/sti doesn't prevent concurrent access from
 *   other CPUs.
 *
 * Code references:
 *   smp.c:126       — gdt_init_cpu(my_cpu, kstack_top) calls kmalloc
 *   gdt.c:136-137   — kmalloc(GDT_ENTRIES*8), kzalloc(128)
 *   heap.c          — heap_alloc uses spin_lock (cli/sti only)
 *   pmm.c           — pmm_alloc_page uses spin_lock (cli/sti only)
 *   spinlock.h:12   — spin_lock = pushfq; cli (no atomic lock!)
 *
 * Verify:
 *   spin -a models/ap_boot_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

#define N_APS  3     /* model 3 APs for tractability (real: up to 15) */

/* Heap state: simplified as a single counter.
 * Concurrent alloc without real lock = torn read/write. */
byte heap_free_blocks = 10;
byte heap_allocs = 0;

/* Track concurrent access to detect races */
byte in_heap = 0;
bool race_detected = false;

/* Per-AP completion flag */
bool ap_done[N_APS];

/* Atomic spinlock protecting gdt_init_cpu (smp.c:ap_init_lock).
 * This is an xchg-based spinlock, SMP-safe. */
bool ap_init_locked = false;

inline AP_INIT_LOCK() {
    atomic { !ap_init_locked -> ap_init_locked = true }
}
inline AP_INIT_UNLOCK() {
    ap_init_locked = false
}

inline HEAP_ALLOC(cpu, count) {
    /* heap's spin_lock is cli/sti only — not SMP-safe.
     * But the caller (gdt_init_cpu) is now wrapped in ap_init_lock
     * which IS SMP-safe, so only one AP is here at a time. */
    in_heap++;

    if
    :: in_heap > 1 -> race_detected = true
    :: else -> skip
    fi;

    byte avail;
    avail = heap_free_blocks;
    if
    :: avail >= count ->
        heap_free_blocks = avail - count;
        heap_allocs = heap_allocs + count
    :: else -> skip
    fi;

    in_heap--;
}

/* AP boot: models ap_entry_64 shared resource access */
proctype ap_boot(byte id) {
    /* gdt_init_cpu: kmalloc(GDT) + kzalloc(TSS)
     * Wrapped in ap_init_lock (atomic spinlock, SMP-safe).
     * Verified: models/ap_boot_race.pml */
    AP_INIT_LOCK();
    HEAP_ALLOC(id, 1);    /* kmalloc for GDT */
    HEAP_ALLOC(id, 1);    /* kzalloc for TSS */
    AP_INIT_UNLOCK();

    /* sched_init_idle: per-CPU array write (safe, no shared state) */
    skip;

    /* lapic_init: BSP-only mapping now (fixed), just LAPIC register config */
    skip;

    ap_done[id] = true
}

init {
    byte i = 0;
    do
    :: i < N_APS ->
        ap_done[i] = false;
        i++
    :: else -> break
    od;

    /* Launch all APs concurrently (models wake_flag release) */
    byte j = 0;
    do
    :: j < N_APS ->
        run ap_boot(j);
        j++
    :: else -> break
    od
}

/* Safety: no concurrent access to heap or PMM */
ltl no_race { [] !race_detected }
