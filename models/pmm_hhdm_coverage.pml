/*
 * PMM / HHDM coverage invariant model for ember.
 *
 * The PMM allocates physical pages.  All kernel code accesses them
 * via phys_to_virt(pa) = HHDM_BASE + pa.  If the HHDM page tables
 * don't map that virtual address, any access causes a page fault.
 *
 * Invariant: for every physical page in PMM's free pool, the
 * corresponding HHDM virtual address must be mapped.
 *
 * Investigation: both HHDM (efi_main.c:866-878) and PMM (pmm.c:69-74)
 * iterate the same EFI memory map with the same type filter.  So they
 * SHOULD cover the same regions.  The actual crash at cr2=0x100000040
 * is NOT an HHDM miss — it's a raw physical address used as a pointer
 * (no HHDM_BASE prefix), suggesting a missing phys_to_virt() call
 * somewhere in the kernel.
 *
 * This model verifies the HHDM coverage invariant and confirms both
 * FIX_HHDM and FIX_PMM strategies are correct if needed.
 *
 * Model: abstract physical memory as regions.  Each region is either
 * below or above a threshold (4GB).  PMM and HHDM each cover a set
 * of regions.  Allocation from PMM must only return pages from
 * HHDM-covered regions.
 *
 * Verify:
 *   spin -a models/pmm_hhdm_coverage.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

/* Physical memory regions (abstracted) */
#define REGION_LOW    0    /* below 4GB — always HHDM-mapped */
#define REGION_HIGH   1    /* above 4GB — may or may not be HHDM-mapped */
#define N_REGIONS     2

/* PMM: which regions have free pages */
bool pmm_has_pages[N_REGIONS];

/* HHDM: which regions are mapped */
bool hhdm_mapped[N_REGIONS];

/* Error flag */
bool unmapped_access = false;

/* Allocation count for liveness */
byte alloc_count = 0;

/* PMM allocator: returns a page from any region with free pages.
 * Models pmm_alloc_page() scanning bitmap. */
inline PMM_ALLOC(result_region) {
    if
    :: pmm_has_pages[REGION_LOW] ->
        result_region = REGION_LOW
    :: pmm_has_pages[REGION_HIGH] ->
        result_region = REGION_HIGH
    fi
}

/* Consumer: allocates a page and accesses it via HHDM */
proctype consumer() {
    byte region;

    PMM_ALLOC(region);

    /* phys_to_virt(pa): accesses HHDM_BASE + pa */
    if
    :: hhdm_mapped[region] ->
        skip  /* access succeeds */
    :: !hhdm_mapped[region] ->
        unmapped_access = true  /* PAGE FAULT */
    fi;

    alloc_count = alloc_count + 1
}

/* ── Select scenario ── */

#ifdef FIX_HHDM
/* FIX A: extend HHDM to cover all physical memory */
init {
    pmm_has_pages[REGION_LOW] = true;
    hhdm_mapped[REGION_LOW] = true;
    pmm_has_pages[REGION_HIGH] = true;
    hhdm_mapped[REGION_HIGH] = true;    /* FIX: map high memory in HHDM */
    run consumer(); run consumer(); run consumer()
}
#endif

#ifdef FIX_PMM
/* FIX B: PMM excludes pages not covered by HHDM */
init {
    pmm_has_pages[REGION_LOW] = true;
    hhdm_mapped[REGION_LOW] = true;
    pmm_has_pages[REGION_HIGH] = false;  /* FIX: don't add unmapped pages */
    hhdm_mapped[REGION_HIGH] = false;
    run consumer(); run consumer(); run consumer()
}
#endif

#if !defined(FIX_HHDM) && !defined(FIX_PMM)
/* BUGGY: PMM has high pages, HHDM doesn't cover them */
init {
    pmm_has_pages[REGION_LOW] = true;
    hhdm_mapped[REGION_LOW] = true;
    pmm_has_pages[REGION_HIGH] = true;
    hhdm_mapped[REGION_HIGH] = false;   /* BUG */
    run consumer(); run consumer(); run consumer()
}
#endif

/* Safety: no access to unmapped HHDM region */
ltl no_unmapped { [] !unmapped_access }
