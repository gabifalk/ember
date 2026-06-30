/*
 * sched_kick_eoi.pml — Comprehensive model of LAPIC EOI paths.
 *
 * Models the full boot sequence, AP initialization, IPI delivery,
 * and three distinct EOI code paths to find ordering or state bugs.
 *
 * CRASH (observed, original code):
 *   cr2=00000000fee000b0 rax=00000000fee00000 rdx=0 rcx=1b
 *   lapic_base global = ffff8000fee00000 (correct)
 *   RFLAGS IF=0 (interrupt gate)
 *   Crash RIP is inside isr_sched_kick (TCC build, isr_entry.o)
 *
 * Three EOI paths in the kernel:
 *   P1: timer_eoi_kernel → lapic_eoi → lapic_base[EOI/4] = 0  (C code)
 *   P2: isr_sched_kick   → inline asm EOI                     (asm)
 *   P3: isr_tlb_shootdown → CR3 reload + inline asm EOI       (asm)
 *
 * Original asm EOI (P2/P3):
 *   rdmsr 0x1B → rax = LAPIC_PHYS
 *   movq $HHDM_BASE, %rdx    ← 10-byte imm64 encoding
 *   add %rdx, %rax            → rax = HHDM + LAPIC_PHYS
 *   movl $0, 0xB0(%rax)       → EOI write
 *
 * Fixed asm EOI (P2/P3):
 *   mov lapic_base(%rip), %rax   ← RIP-relative from global
 *   movl $0, 0xB0(%rax)          → EOI write
 *
 * Verified: spin -a sched_kick_eoi.pml && cc -o pan pan.c && ./pan -a
 */

/* ================================================================
 * State encoding
 * ================================================================ */

mtype = { ZERO, LAPIC_PHYS, LAPIC_HHDM };

/* ---- Global kernel state ---- */
mtype lapic_base_global = ZERO;     /* proc.c:18 — set by BSP lapic_init */
bool  lapic_hhdm_mapped = false;    /* LAPIC MMIO page in HHDM page tables */
bool  identity_map_present = true;  /* removed early in kmain */
bool  idt_installed = false;        /* after idt_init on BSP */
bool  aps_started = false;          /* after smp_init sends SIPI */

/* ---- Per-AP state ---- */
bool  ap_idt_loaded[2];            /* AP loaded shared IDT */
bool  ap_lapic_inited[2];          /* AP called lapic_init */
bool  ap_interrupts_enabled[2];    /* AP did sti */
bool  ap_timer_started[2];         /* AP started LAPIC timer */
bool  ap_in_idle[2];               /* AP in sti;hlt idle loop */

/* ---- Scheduling state ---- */
bool  bsp_scheduling = false;       /* BSP has called schedule() */

/* ---- Observable ---- */
bool  eoi_fault = false;

ltl p_no_eoi_fault { [] !eoi_fault }

/* ================================================================
 * Helper: perform EOI write and check address validity
 * ================================================================ */
inline do_eoi_write(addr) {
    if
    :: addr == LAPIC_HHDM ->
        /* HHDM address: always mapped (BSP mapped it before APs start) */
        if
        :: lapic_hhdm_mapped -> skip       /* OK */
        :: !lapic_hhdm_mapped ->
            eoi_fault = true               /* HHDM mapping gone?! */
        fi
    :: addr == LAPIC_PHYS ->
        /* Identity-mapped address: only valid if identity map present */
        if
        :: identity_map_present -> skip    /* OK (but shouldn't happen) */
        :: !identity_map_present ->
            eoi_fault = true               /* PAGE FAULT — matches crash */
        fi
    :: addr == ZERO ->
        eoi_fault = true                   /* NULL pointer */
    fi
}

/* ================================================================
 * Helper: original MSR-based EOI (P2/P3 before fix)
 *
 * Models the instruction sequence faithfully.
 * The movq is a single x86 instruction — it either loads the
 * correct value or doesn't.  We don't assume WHY it might fail;
 * we model the possibility nondeterministically.
 * ================================================================ */
inline eoi_via_msr() {
    mtype rax;
    mtype rdx;

    /* rdmsr 0x1B: read LAPIC base MSR → edx:eax */
    /* On x86: clears upper 32 bits of both RAX and RDX */
    /* LAPIC base is always < 4GB, so edx = 0 */
    rax = LAPIC_PHYS;
    rdx = ZERO;

    /* and $0xFFFFF000, %eax — page align, clear rax[63:32] */
    /* shl $32, %rdx; or %rdx, %rax — combine (rdx=0, no effect) */
    /* rax = LAPIC_PHYS, rdx = ZERO */

    /* movq $0xffff800000000000, %rdx — 10-byte imm64 */
    /* This instruction SHOULD set rdx to HHDM_BASE.
     * We model the possibility that it doesn't work correctly. */
    if
    :: true -> rdx = LAPIC_HHDM  /* correct: rdx = HHDM_BASE (nonzero) */
    :: true -> rdx = ZERO        /* broken: rdx stays 0 */
    fi;

    /* add %rdx, %rax */
    if
    :: rdx == ZERO ->
        skip  /* rax unchanged = LAPIC_PHYS */
    :: rdx != ZERO ->
        rax = LAPIC_HHDM   /* rax = LAPIC_PHYS + HHDM_BASE */
    fi;

    /* movl $0, 0xB0(%rax) — EOI write */
    do_eoi_write(rax)
}

/* ================================================================
 * Helper: fixed global-based EOI (P2/P3 after fix)
 * ================================================================ */
inline eoi_via_global() {
    mtype rax;

    /* mov lapic_base(%rip), %rax */
    rax = lapic_base_global;

    /* movl $0, 0xB0(%rax) — EOI write */
    do_eoi_write(rax)
}

/* ================================================================
 * Helper: C-based EOI (P1 — timer path, always uses global)
 * ================================================================ */
inline eoi_via_c() {
    /* lapic_eoi: if (lapic_base) lapic_write(LAPIC_EOI, 0) */
    if
    :: lapic_base_global != ZERO ->
        do_eoi_write(lapic_base_global)
    :: lapic_base_global == ZERO ->
        skip   /* lapic_eoi does nothing if base is NULL */
    fi
}

/* ================================================================
 * BSP boot sequence
 * ================================================================ */
active proctype BSP() {
    /* kmain early: remove identity map */
    identity_map_present = false;

    /* acpi_init → lapic_init (BSP) */
    /* paging_map_range: map LAPIC MMIO into HHDM */
    lapic_hhdm_mapped = true;
    /* lapic_base = phys_to_virt(acpi_lapic_base) */
    lapic_base_global = LAPIC_HHDM;

    /* idt_init: install handlers including vector 0x40 */
    idt_installed = true;

    /* smp_init: send SIPI to APs */
    aps_started = true;

    /* Wait for APs to be ready (simplified) */
    /* ... */

    /* lapic_timer_init: calibrate and start BSP timer */
    /* First schedule/fork happens, sends sched kick IPI */
    bsp_scheduling = true;

    /*
     * BSP also receives timer interrupts.
     * Timer ISR kernel path: timer_eoi_kernel → lapic_eoi (P1)
     */
    eoi_via_c()
}

/* ================================================================
 * AP boot and idle loop
 *
 * Key ordering in ap_entry_64:
 *   1. gdt_init_cpu     (needs ap_init_lock)
 *   2. lidt             (loads shared IDT — handlers now dispatchable)
 *   3. syscall_init_ap
 *   4. sched_init_idle
 *   5. lapic_init       (writes LAPIC SVR/LINT/TPR via lapic_base)
 *   6. signal BSP ready
 *   7. lapic_timer_init_count (starts LAPIC timer → timer IRQs begin)
 *   8. sti; hlt; cli    (idle loop — sched kick & timer IPIs arrive here)
 *
 * QUESTION: Between step 2 (IDT loaded) and step 8 (sti),
 * can any interrupt arrive?  IF is cleared until sti.
 * NMI can arrive anytime but uses a separate handler.
 *
 * QUESTION: Between step 7 (timer started) and step 8 (sti),
 * the timer is counting but IF=0.  The first timer IRQ is
 * latched and delivered when sti enables interrupts.
 * ================================================================ */
active [2] proctype AP() {
    byte me = _pid - 1;   /* AP index 0 or 1 */

    /* Wait for BSP to send SIPI */
    (aps_started);

    /* Step 1-2: GDT + IDT */
    ap_idt_loaded[me] = true;
    /* After IDT load, vector 0x40 handler is reachable IF interrupts enabled.
     * But IF=0 here (AP starts with cli). */

    /* Step 3-4: syscall + sched init */

    /* Step 5: lapic_init — AP path (skips mapping, just writes SVR etc.) */
    /* lapic_init uses lapic_base global (set by BSP) to write LAPIC regs.
     * If lapic_base == 0 here, lapic_init returns early (no LAPIC). */
    assert(lapic_base_global == LAPIC_HHDM);  /* BSP must have set it */
    ap_lapic_inited[me] = true;

    /* Step 6: signal ready */

    /* Step 7: start LAPIC timer */
    ap_timer_started[me] = true;
    /* Timer IRQs now being generated by LAPIC hardware.
     * But IF=0, so they are latched, not delivered. */

    /* Step 8: idle loop */
    ap_in_idle[me] = true;

    /* sti; hlt — now interrupts are enabled.
     * Two things can wake us:
     *   A) Timer IRQ (vector 32) → .Ltimer_kernel → timer_eoi_kernel
     *   B) Sched kick IPI (vector 0x40) → isr_sched_kick
     *   C) TLB shootdown IPI (vector 0x41) → isr_tlb_shootdown
     */
    ap_interrupts_enabled[me] = true;

end_idle:
    do
    :: true ->
        /* A) Timer IRQ — uses C lapic_eoi (P1) */
        eoi_via_c()
    :: bsp_scheduling ->
        /* B) Sched kick IPI — uses asm EOI */
        eoi_via_global()    /* FIXED code */
        /* eoi_via_msr()    -- ORIGINAL code, uncomment to test */
    :: true ->
        /* C) TLB shootdown — CR3 reload + asm EOI */
        /* CR3 reload flushes TLB; page table walk must succeed for LAPIC */
        if
        :: lapic_hhdm_mapped -> skip   /* LAPIC page tables intact */
        :: !lapic_hhdm_mapped ->
            /* If LAPIC mapping was destroyed, CR3 reload + EOI faults */
            eoi_fault = true
        fi;
        eoi_via_global()    /* FIXED code */
        /* eoi_via_msr()    -- ORIGINAL code, uncomment to test */
    od
}
