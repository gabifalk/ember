/*
 * tlb_ipi_deadlock.pml — TLB shootdown IPI vs BKL spin deadlock.
 *
 * Scenario: CPU 0 holds BKL, sends TLB shootdown IPI, waits for acks.
 * CPUs 1,2 are spinning for BKL with interrupts disabled.
 * Without sti in spin loop: deadlock (IPIs never delivered).
 * With sti: IPIs delivered during spin, acks sent, sender proceeds.
 *
 * Verify buggy (invalid end state = deadlock):
 *   spin -a models/tlb_ipi_deadlock.pml && \
 *   gcc -O2 -o pan pan.c && ./pan
 *
 * Verify fixed (0 errors):
 *   spin -a -DFIX_STI models/tlb_ipi_deadlock.pml && \
 *   gcc -O2 -o pan pan.c && ./pan
 */

bool bkl_locked = true;  /* sender starts holding BKL */
bool ipi_pending[3];
byte tlb_acks;

/* ── Sender: holds BKL, sends IPI, waits for acks ── */
active proctype sender() {
    atomic {
        tlb_acks = 0;
        ipi_pending[1] = true;
        ipi_pending[2] = true;
    };

    /* Wait for acks — blocks here if deadlocked */
    (tlb_acks >= 2);

    bkl_locked = false;
}

/* ── Spinner: tries to acquire BKL.
 * Buggy: can only proceed when BKL is free (blocks on locked BKL).
 * Fixed: can also process pending IPI while waiting. ── */
proctype spinner(byte me) {
    if
    :: atomic { !bkl_locked -> bkl_locked = true };
       bkl_locked = false
#ifdef FIX_STI
    :: ipi_pending[me] ->
       ipi_pending[me] = false;
       tlb_acks++;
       /* Now wait for BKL normally */
       atomic { !bkl_locked -> bkl_locked = true };
       bkl_locked = false
#endif
    fi
}

init {
    run spinner(1);
    run spinner(2);
}
