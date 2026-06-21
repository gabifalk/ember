/*
 * COW + VMA-protection model for ember.
 *
 * Demonstrates the RO-at-fork -> mprotect(PROT_WRITE) isolation bug and
 * verifies the VMA-authoritative fix.
 *
 * Scenario:
 *   After fork(), parent (proc 0) and child (proc 1) share one physical
 *   page that was READ-ONLY at fork time: both PTEs are RO, the COW bit is
 *   clear, refcount == 2, both VMAs are PROT_READ.
 *
 *   The parent then mprotect(PROT_WRITE)s the page and writes to it.  POSIX
 *   fork semantics require the child to never observe that write.
 *
 *   BUGGY_MPROTECT (ember today): mprotect grants the writable bit directly
 *   because the COW bit is clear -- it keys off the PTE COW bit, not the
 *   refcount.  The parent then writes to the still-shared frame and the
 *   child sees it -> isolation violated.
 *
 *   FIXED (VMA-authoritative): the VMA prot is the source of truth.  mprotect
 *   never grants W directly on a shared frame; the write faults and the
 *   handler checks the VMA (writable?) and the refcount (shared? -> copy).
 *
 * TLB coherence is covered separately by tlb_cow.pml; this model isolates
 * the permission/sharing semantics.
 *
 * Verify (fixed -- must PASS):
 *   spin -a models/cow_vma_prot.pml && gcc -O2 -o pan pan.c && ./pan -m100000
 * Verify (buggy -- must FAIL with an assertion violation):
 *   spin -a -DBUGGY_MPROTECT models/cow_vma_prot.pml && \
 *       gcc -O2 -o pan pan.c && ./pan -m100000
 */

#define DATA_ORIG    10
#define DATA_PARENT  20

/* Physical pages: content. page[1]=shared original, page[2]=parent's copy. */
byte page[3];

/* Per-process PTE (0=parent, 1=child). */
byte pte_page[2];      /* physical page the PTE maps */
bool pte_writable[2];  /* hardware writable bit */
bool pte_cow[2];       /* COW bit (OS-available) */

/* Per-process VMA intended protection: is PROT_WRITE present? */
bool vma_writable[2];

/* Physical-page refcounts. */
byte refcount[3];

/* BKL: serialize kernel sections. */
bool bkl = false;

/* Progress flags. */
bool parent_done = false;
bool child_done = false;

/* Value the child observed on its read. */
byte child_observed = 0;

/*
 * mprotect(PROT_WRITE) on process p's shared page.  The VMA prot is updated
 * in both variants; only the PTE handling differs.
 */
inline mprotect_write(p) {
#ifdef BUGGY_MPROTECT
	/* ember today: preserve COW only if the bit is already set; otherwise
	 * grant the writable bit directly.  On a shared RO-at-fork page the
	 * COW bit is clear, so this hands out write access to a shared frame. */
	vma_writable[p] = true;
	if
	:: pte_cow[p] -> skip
	:: else -> pte_writable[p] = true
	fi
#else
	/* Fixed: never grant W on a shared frame.  Leave it RO (mark COW as a
	 * deferred-copy hint) so the next write faults into the handler. */
	vma_writable[p] = true;
	if
	:: refcount[pte_page[p]] > 1 -> pte_cow[p] = true
	:: else -> pte_writable[p] = true
	fi
#endif
}

/*
 * Write-fault handler -- VMA-authoritative.  Reached when a write hits a
 * non-writable present page.  'newpg' is the fresh page used if a copy is
 * needed.
 */
inline write_fault(p, newpg) {
	/* The VMA is the source of truth for "is this write legal?". */
	if
	:: !vma_writable[p] -> assert(false)   /* would be SIGSEGV; not this scenario */
	:: else -> skip
	fi;
	if
	:: refcount[pte_page[p]] == 1 ->
		pte_writable[p] = true;
		pte_cow[p] = false
	:: else ->
		page[newpg] = page[pte_page[p]];
		refcount[pte_page[p]] = refcount[pte_page[p]] - 1;
		refcount[newpg] = 1;
		pte_page[p] = newpg;
		pte_writable[p] = true;
		pte_cow[p] = false
	fi
}

/* ---- Parent (proc 0): mprotect, then write ---- */
proctype Parent() {
	atomic { !bkl -> bkl = true }
	mprotect_write(0);
	bkl = false;

	if
	:: pte_writable[0] ->
		skip    /* no fault -- this is the buggy direct-grant path */
	:: else ->
		atomic { !bkl -> bkl = true }
		write_fault(0, 2);
		bkl = false
	fi;

	assert(pte_writable[0]);
	page[pte_page[0]] = DATA_PARENT;
	parent_done = true;

	/* The shared original page must never carry the parent's write. */
	assert(page[1] == DATA_ORIG)
}

/* ---- Child (proc 1): read; must never see the parent's write ---- */
proctype Child() {
	child_observed = page[pte_page[1]];
	assert(child_observed == DATA_ORIG);
	child_done = true
}

init {
	page[1] = DATA_ORIG;
	page[2] = 0;

	/* After fork of a READ-ONLY page: shared, RO, no COW bit, refcount 2. */
	pte_page[0] = 1; pte_writable[0] = false; pte_cow[0] = false;
	pte_page[1] = 1; pte_writable[1] = false; pte_cow[1] = false;
	refcount[1] = 2;
	vma_writable[0] = false;   /* PROT_READ */
	vma_writable[1] = false;

	run Parent();
	run Child();

	parent_done && child_done;

	/* Final isolation invariants (hold in the fixed model). */
	assert(page[1] == DATA_ORIG);
	assert(pte_page[0] == 2);    /* parent got a private copy */
	assert(pte_page[1] == 1);    /* child still on the shared page */
	assert(refcount[1] == 1)     /* parent dropped its ref to the shared page */
}
