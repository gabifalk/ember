/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Device IRQ dispatch vs BKL, including the kernel-mode-without-BKL case:
 * the handler acquires the BKL iff not already held by this CPU and releases
 * iff it acquired. EOI exactly once. No leak. Both the nested case (IRQ taken
 * while this CPU already holds the BKL) and the contended case (another CPU
 * holds it) are reachable. */

bit bkl = 0;
byte holder = 255;
byte eoi_count = 0;
bit handler_ran = 0;

inline acquire(cpu) { atomic { bkl == 0 -> bkl = 1; holder = cpu } }
inline release(cpu) { atomic { assert(holder == cpu); holder = 255; bkl = 0 } }

/* Device IRQ entry on CPU `cpu`: acquire iff not already held by self, run the
 * handler, EOI once, release iff we acquired (matches isr_irq_common). */
inline irq_entry(cpu) {
	bit acq = 0;
	if
	:: (holder != cpu) -> acquire(cpu); acq = 1
	:: (holder == cpu) -> skip            /* nested: already held by self */
	fi;
	handler_ran = 1;
	eoi_count++;
	if
	:: acq -> release(cpu)
	:: else -> skip
	fi;
}

/* A second CPU doing ordinary kernel work, to create BKL contention. */
active proctype other_cpu() {
	acquire(1);
	release(1);
}

/* CPU 0 takes a device IRQ either nested in its own kernel critical section
 * (holder == 0) or while idle (holder != 0, possibly held by other_cpu). */
active proctype cpu0() {
	if
	:: acquire(0); irq_entry(0); release(0)   /* nested */
	:: irq_entry(0)                           /* non-nested / contended */
	fi
}

ltl runs    { <> (handler_ran == 1) }
ltl eoi_one { [] (eoi_count <= 1) }
ltl no_leak { <> [] (bkl == 0) }
