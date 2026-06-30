/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_PERCPU_H
#define EMBER_PERCPU_H

#include <ember/cpu.h>

struct proc;

extern struct proc *percpu_current[MAX_CPUS];

static inline struct proc *
get_current_proc(void)
{
	int cpu = this_cpu_id();
	if (cpu < 0 || cpu >= MAX_CPUS)
		return 0;
	return percpu_current[cpu];
}

static inline void
set_this_cpu_proc(struct proc *p)
{
	percpu_current[this_cpu_id()] = p;
}

/* Alias for early BSP init -- identical to set_this_cpu_proc. */
static inline void
set_bsp_current_proc(struct proc *p)
{
	percpu_current[0] = p;
}

/* Legacy compatibility -- existing code using current_proc keeps working. */
#define current_proc (get_current_proc())

#endif				/* EMBER_PERCPU_H. */
