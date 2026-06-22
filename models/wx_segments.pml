/*
 * W^X segment-protection model for ember.
 *
 * The ELF loader must map each PT_LOAD segment with its real p_flags: a code
 * segment is read-execute (NOT writable), a data segment is read-write.  With
 * the VMA authoritative for protection, a user write to a code page must then
 * fault to SIGSEGV, while a write to a data page succeeds.
 *
 * BUGGY_LOADER (ember today): the loader maps every segment RWX, ignoring
 * p_flags.  The code segment's VMA is writable, so a write to code wrongly
 * succeeds (W^X violation -- a classic exploitation primitive).
 *
 * FIXED: the loader honors p_flags; the code VMA is read-only (+exec), so a
 * write to code is denied by the VMA-authoritative fault path.
 *
 * Verify (fixed -- must PASS):
 *   spin -a models/wx_segments.pml && gcc -O2 -o pan pan.c && ./pan
 * Verify (buggy -- must FAIL):
 *   spin -a -DBUGGY_LOADER models/wx_segments.pml && \
 *       gcc -O2 -o pan pan.c && ./pan
 */

/* Two segments: 0 = code, 1 = data. */
bool vma_w[2];        /* VMA grants write */
bool seg_wrote[2];    /* a write to this segment succeeded */
bool seg_fault[2];    /* a write to this segment faulted (SIGSEGV) */

inline load_segments() {
#ifdef BUGGY_LOADER
	/* ember today: everything mapped RW, p_flags ignored. */
	vma_w[0] = true;    /* code: WRONGLY writable */
	vma_w[1] = true     /* data */
#else
	/* Fixed: honor p_flags. */
	vma_w[0] = false;   /* code: read-execute, not writable */
	vma_w[1] = true     /* data: read-write */
#endif
}

/* A user write to segment s -- the VMA is authoritative. */
inline write_seg(s) {
	if
	:: vma_w[s] -> seg_wrote[s] = true
	:: else -> seg_fault[s] = true
	fi
}

active proctype P() {
	load_segments();
	write_seg(0);   /* write to code */
	write_seg(1);   /* write to data */

	assert(seg_fault[0]);    /* a write to code must SIGSEGV */
	assert(seg_wrote[1])     /* a write to data must succeed */
}
