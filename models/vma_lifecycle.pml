/*
 * VMA totality model for ember.
 *
 * Invariant: every present user page is covered by a VMA whose protection is
 * at least as permissive as the PTE (present => covered; pte_writable =>
 * vma_writable).  This is the PRECONDITION that makes a VMA-authoritative
 * page-fault gate safe: without it, a legitimate write to a present page that
 * has no covering VMA would be misclassified as a protection violation and
 * the process would be wrongly killed.
 *
 * NO_REGION_VMA (ember today): the stack, brk and ELF-segment mappers create
 * pages but never call vma_add.  A present writable page then has no covering
 * VMA -> totality violated.
 *
 * FIXED: every region mapper adds a VMA with matching prot, and aborts the
 * mapping when no VMA slot is free (rather than mapping a page with no VMA).
 *
 * Verify (fixed -- must PASS):
 *   spin -a models/vma_lifecycle.pml && gcc -O2 -o pan pan.c && ./pan
 * Verify (buggy -- must FAIL):
 *   spin -a -DNO_REGION_VMA models/vma_lifecycle.pml && \
 *       gcc -O2 -o pan pan.c && ./pan
 */

#define NREG    4      /* abstract page-regions: e.g. code, data, stack, brk */
#define MAX_VMA 3      /* fewer slots than regions -> exercises overflow */

bool present[NREG];    /* page mapped (PTE present) */
bool pte_w[NREG];      /* PTE writable */
bool vma_cover[NREG];  /* a VMA covers this region */
bool vma_w[NREG];      /* VMA grants write */
byte vma_used = 0;     /* VMA slots consumed */

/* Map region i with writability w, as a region mapper (stack/brk/segment). */
inline map_region(i, w) {
#ifdef NO_REGION_VMA
	/* ember today: create the page, no VMA. */
	present[i] = true;
	pte_w[i] = w
#else
	/* Fixed: reserve a VMA slot first; abort the mapping if none is free. */
	if
	:: vma_used < MAX_VMA ->
		vma_cover[i] = true;
		vma_w[i] = w;
		vma_used = vma_used + 1;
		present[i] = true;
		pte_w[i] = w
	:: else ->
		skip            /* no VMA slot -> ENOMEM, leave the page unmapped */
	fi
#endif
}

active proctype Setup() {
	byte i = 0;
	byte k;
	bool w;

	do
	:: i < NREG ->
		if
		:: w = true
		:: w = false
		fi;
		map_region(i, w);

		/* Totality + PTE <= VMA, checked after every region creation. */
		for (k : 0 .. NREG - 1) {
			assert(!present[k] || vma_cover[k]);
			assert(!pte_w[k] || vma_w[k])
		}
		i = i + 1
	:: i >= NREG ->
		break
	od
}
