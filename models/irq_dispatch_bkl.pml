/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Device IRQ dispatch vs BKL: handler acquires BKL, dispatches, EOIs once,
 * releases; must not deadlock with a CPU spinning for the BKL. */

bit bkl = 0;            /* big kernel lock */
byte holder = 255;      /* cpu id holding bkl, 255 = none */
byte eoi_count = 0;     /* EOIs issued by the IRQ handler */
bit handler_ran = 0;

inline bkl_acquire(cpu) {
	atomic { bkl == 0 -> bkl = 1; holder = cpu }
}
inline bkl_release(cpu) {
	atomic { holder = 255; bkl = 0 }
}

/* CPU0: a normal kernel critical section that takes and drops the BKL. */
active proctype kernel_cpu0() {
	bkl_acquire(0);
	/* ... kernel work ... */
	bkl_release(0);
}

/* BSP device IRQ handler (interrupt entry path). */
active proctype irq_handler() {
	bkl_acquire(1);
	handler_ran = 1;
	eoi_count++;            /* lapic_eoi(), exactly once */
	bkl_release(1);
}

/* Safety: handler eventually runs, EOIs exactly once, lock left free. */
ltl runs   { <> (handler_ran == 1) }
ltl eoi_one { [] (eoi_count <= 1) }
ltl no_leak { <> [] (bkl == 0) }
