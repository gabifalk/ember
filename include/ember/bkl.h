/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_BKL_H
#define EMBER_BKL_H

#include <ember/spinlock.h>
#include <ember/cpu.h>
#include <ember/vectors.h>

/*
 * Big Kernel Lock -- serializes all kernel entry.
 * Matches the verified Promela model in models/bkl.pml.
 *
 * Acquire: on kernel entry (syscall, interrupt, exception)
 * Release: on return to userspace or entering idle spin.
 */

extern atomic_spinlock_t bkl;
extern volatile int bkl_holder_cpu;

static inline void
bkl_acquire(void)
{
	while (1) {
		int old;
		__asm__ __volatile__("xchgl %0, %1":"=r"(old), "+m"(bkl.locked)
				     :"0"(1)
				     :"memory");
		if (!old)
			break;
		/*
		 * Halt until interrupt -- sti;hlt is atomic on x86 (interrupt
		 * delivery deferred until hlt), so no lost-wakeup.
		 * bkl_release sends a kick IPI to wake us immediately.
		 */
		__asm__ __volatile__("sti; hlt; cli":::"memory");
	}
	bkl_holder_cpu = this_cpu_id();
}

static inline int
bkl_tryacquire(void)
{
	if (atomic_spin_trylock(&bkl)) {
		bkl_holder_cpu = this_cpu_id();
		return 1;
	}
	return 0;
}

static inline void
bkl_release(void)
{
	bkl_holder_cpu = -1;
	atomic_spin_unlock(&bkl);
	/* Wake CPUs halted in bkl_acquire so they can try again. */
	extern int cpu_count;
	extern volatile int lapic_enabled;
	extern void lapic_send_ipi_all_excl_self(uint8_t vector);
	if (cpu_count > 1 && lapic_enabled)
		lapic_send_ipi_all_excl_self(VEC_SCHED_KICK);
}

static inline int
bkl_held_by_this_cpu(void)
{
	return bkl_holder_cpu == this_cpu_id();
}

#endif				/* EMBER_BKL_H. */
