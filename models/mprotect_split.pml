/*
 * mprotect VMA-split model for Ember (VMA-authority redesign).
 *
 * Every glibc/musl binary mprotects its GNU_RELRO sub-range to read-only at
 * startup.  That range is the FRONT of the read-write data segment, whose VMA
 * also covers .data/.bss behind RELRO.  do_mprotect must change the protection
 * of ONLY the requested sub-range, splitting the VMA -- the pages behind RELRO
 * stay writable.
 *
 * BUGGY (Ember): do_mprotect did `v = vma_find(addr); v->prot = prot`, setting
 * the ENTIRE covering VMA's prot.  RELRO's mprotect therefore marked the whole
 * data segment read-only; after fork the .bss COW write was denied -> SIGSEGV.
 *
 * FIXED: carve the sub-range out (vma_remove preserves the surrounding prot)
 * and re-add it with the new prot, so only [addr,addr+len) changes.
 *
 * Verify (fixed -- must PASS):
 *   spin -a models/mprotect_split.pml && gcc -O2 -o pan pan.c && ./pan
 * Verify (buggy -- must FAIL):
 *   spin -a -DBUGGY models/mprotect_split.pml && gcc -O2 -o pan pan.c && ./pan
 */

#define N 10            /* data-segment pages */
#define RELRO 4         /* mprotect(PROT_READ) covers pages [0, RELRO) */

bool vma_w[N];          /* VMA permits writes, per page */

inline mprotect_relro() {
    k = 0;
    do
    :: k < N ->
#ifdef BUGGY
        /* vma_find(addr) returns the whole data VMA; its prot is overwritten. */
        vma_w[k] = false
#else
        /* Split: only the requested sub-range changes. */
        if
        :: k < RELRO -> vma_w[k] = false
        :: else -> skip            /* pages behind RELRO keep write perm */
        fi
#endif
        ;
        k++
    :: k >= N -> break
    od
}

active proctype P() {
    /* Loader: the whole read-write data segment is writable. */
    int i = 0;
    int k = 0;
    do
    :: i < N -> vma_w[i] = true; i++
    :: i >= N -> break
    od;

    mprotect_relro();

    assert(!vma_w[2]);    /* a RELRO page must become read-only */
    assert(vma_w[7])      /* .bss behind RELRO must stay writable */
}
