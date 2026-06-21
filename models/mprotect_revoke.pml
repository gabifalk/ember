/*
 * mprotect(PROT_READ) revocation model for ember.
 *
 * A COW page (writable-at-fork, shared, RO+COW in hardware) is mprotect'd
 * read-only.  A subsequent write MUST fault to SIGSEGV -- it must never be
 * silently copied and granted write access.
 *
 * BUGGY_MPROTECT (ember today): do_mprotect's COW-preserve branch keeps the
 * COW bit even when revoking write, and the fault path keys off the COW bit
 * alone.  A later write hits the COW handler, which copies the page and
 * grants write -- the write wrongly succeeds on a read-only region.
 *
 * FIXED (VMA-authoritative): the fault handler derives writability from the
 * VMA.  With the VMA read-only, the write is a protection violation -> SIGSEGV.
 *
 * Verify (fixed -- must PASS):
 *   spin -a models/mprotect_revoke.pml && gcc -O2 -o pan pan.c && ./pan
 * Verify (buggy -- must FAIL):
 *   spin -a -DBUGGY_MPROTECT models/mprotect_revoke.pml && \
 *       gcc -O2 -o pan pan.c && ./pan
 */

bool vma_writable = true;   /* VMA intended prot: starts read-write */
bool pte_writable = false;  /* COW page is read-only in hardware */
bool pte_cow = true;        /* COW bit set (page was writable at fork) */

bool segfault = false;

/* mprotect(PROT_READ): revoke write on the page. */
inline mprotect_ro() {
	vma_writable = false;
#ifdef BUGGY_MPROTECT
	/* ember today: the COW-preserve branch keeps the COW bit set. */
	pte_writable = false
#else
	/* Fixed: revoking write clears COW so a later write cannot be copied. */
	pte_writable = false;
	pte_cow = false
#endif
}

/* Write fault on a non-writable present page. */
inline write_fault() {
#ifdef BUGGY_MPROTECT
	/* ember today: the fault path keys off the COW bit only. */
	if
	:: pte_cow -> pte_writable = true; pte_cow = false   /* wrongly grants write */
	:: else -> segfault = true
	fi
#else
	/* VMA-authoritative: writability comes from the VMA, not the COW bit. */
	if
	:: !vma_writable -> segfault = true                   /* read-only -> SIGSEGV */
	:: else -> skip                                       /* would COW (not this case) */
	fi
#endif
}

active proctype P() {
	mprotect_ro();

	/* The program writes to the now-read-only page. */
	if
	:: pte_writable -> skip          /* no fault */
	:: else -> write_fault()
	fi;

	/* A write to a read-only region MUST have faulted. */
	assert(segfault)
}
