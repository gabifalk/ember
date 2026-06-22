/*
 * Shared-segment-page protection model for Ember (VMA-authority redesign).
 *
 * Real binaries pack PT_LOAD segments so a read-only segment (text/rodata)
 * and the read-write data segment share one boundary page.  Two places decide
 * whether a write to that page is allowed:
 *
 *   1. the loader's PTE permissions for the page, and
 *   2. the VMA consulted by the page-fault handler (vma_addr_writable).
 *
 * BUGGY (Ember after Phase 1+3): the loader maps the shared page with the
 * FIRST segment's perms (read-only) and the writable segment skips the
 * already-present page; setup_image_vmas adds both segment VMAs and vma_find
 * returns the first (read-only) one.  A legitimate write to .data on the
 * shared page is denied -> SIGSEGV.
 *
 * FIXED: the loader takes the UNION of permissions for a shared page (writable
 * wins) and vma_addr_writable reports writable if ANY covering VMA permits it.
 * A write to a shared (code+data) page succeeds; a write to a page that only a
 * read-only segment covers still faults (W^X preserved).
 *
 * Verify (fixed -- must PASS):
 *   spin -a models/wx_shared_page.pml && gcc -O2 -o pan pan.c && ./pan
 * Verify (buggy -- must FAIL):
 *   spin -a -DBUGGY models/wx_shared_page.pml && gcc -O2 -o pan pan.c && ./pan
 */

/* Three pages: 0 = code-only (RO), 1 = shared code+data, 2 = data-only (RW). */
bool ro_covers[3];   /* a read-only segment covers this page */
bool rw_covers[3];   /* a read-write segment covers this page */

bool pte_writable[3];   /* loader's PTE write permission */
bool vma_writable[3];   /* what vma_addr_writable() reports */

bool wrote[3];       /* a user write to this page succeeded */
bool faulted[3];     /* a user write to this page took SIGSEGV */

inline load_image() {
    /* Segment coverage: page0 RO-only, page1 shared, page2 RW-only. */
    ro_covers[0] = true;  rw_covers[0] = false;
    ro_covers[1] = true;  rw_covers[1] = true;
    ro_covers[2] = false; rw_covers[2] = true;

    /* Loader maps the RO segment first, then the RW segment. */
    int p = 0;
    do
    :: p < 3 ->
        if
        :: ro_covers[p] -> pte_writable[p] = false   /* RO seg maps RO */
        :: else -> skip
        fi;
        if
        :: rw_covers[p] ->
            if
            :: ro_covers[p] ->
                /* Shared page already present from the RO segment. */
#ifdef BUGGY
                skip                       /* BUG: skip -> stays read-only */
#else
                pte_writable[p] = true     /* FIX: union -> writable wins */
#endif
            :: else -> pte_writable[p] = true   /* fresh RW page */
            fi
        :: else -> skip
        fi;
        p++
    :: p >= 3 -> break
    od;

    /* setup_image_vmas + vma_addr_writable. */
    p = 0;
    do
    :: p < 3 ->
#ifdef BUGGY
        /* vma_find returns the first covering VMA; RO segment added first. */
        if
        :: ro_covers[p] -> vma_writable[p] = false
        :: else -> vma_writable[p] = rw_covers[p]
        fi
#else
        /* Union: writable if ANY covering VMA permits writes. */
        vma_writable[p] = rw_covers[p]
#endif
        ;
        p++
    :: p >= 3 -> break
    od
}

/* A user write to page p: the VMA is authoritative, PTE must also allow it. */
inline write_page(p) {
    if
    :: vma_writable[p] && pte_writable[p] -> wrote[p] = true
    :: else -> faulted[p] = true
    fi
}

active proctype P() {
    load_image();

    write_page(1);   /* write to .data on the shared page -- must succeed */
    write_page(2);   /* write to data-only page          -- must succeed */
    write_page(0);   /* write to code-only page          -- must fault   */

    assert(wrote[1]);     /* shared-page data write must succeed */
    assert(wrote[2]);     /* data-only write must succeed */
    assert(faulted[0])    /* W^X: write to read-only code must fault */
}
