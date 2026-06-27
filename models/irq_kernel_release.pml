/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Demonstrates the pre-fix leak: kernel-mode device IRQ acquires the BKL but
 * does not release it. no_leak MUST fail here, which is what the fix removes. */
bit bkl = 0; byte holder = 255;
inline bkl_acquire(cpu) { atomic { bkl == 0 -> bkl = 1; holder = cpu } }
active proctype kernel_cpu0() { /* idle CPU released the BKL: starts unheld */ skip }
active proctype irq_handler() {
	if :: (holder != 1) -> bkl_acquire(1) :: else -> skip fi;
	/* BUG: no release on kernel-mode return */
}
ltl no_leak { <> [] (bkl == 0) }
