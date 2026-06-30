/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/syscall.h"
#include "ember/console.h"
#include "ember/heap.h"

#define IA32_STAR         0xC0000081u
#define IA32_LSTAR        0xC0000082u
#define IA32_FMASK        0xC0000084u
#define IA32_EFER         0xC0000080u
#define IA32_GS_BASE        0xC0000101u
#define IA32_KERNEL_GS_BASE 0xC0000102u

#define KERNEL_CS 0x08u
/* STAR expects the user base selector (SS-8), so 0x13 yields CS=0x23, SS=0x1b. */
#define USER_CS_BASE 0x13u

struct cpu_local {
	uint64_t kstack_top;	/* Gs:0. */
	uint64_t user_rsp;	/* Gs:8. */
	uint64_t user_cr3;	/* Gs:16. */
	uint64_t cur_proc_ptr;	/* Gs:24 -- pointer to proc_t. */
	uint64_t resume_rsp;	/* Gs:32 -- kernel_resume_rsp equivalent. */
	int cpu_id;		/* Gs:40. */
	uint32_t _pad;		/* Gs:44 -- alignment padding. */
	uint64_t tss_ptr;	/* Gs:48 -- per-CPU TSS pointer. */
	uint64_t use_iretq;	/* Gs:56 -- force iretq return (rt_sigreturn). */
};

struct cpu_local cpu0;
static uint8_t kstack[16384] __attribute__ ((aligned(16)));

extern void syscall_entry(void);
extern uint64_t bsp_tss_addr(void);

/* Set per-CPU flag so syscall_return uses iretq instead of sysretq. */
void
syscall_force_iretq(void)
{
	__asm__ __volatile__("movq $1, %%gs:56"::: "memory");
}

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
	uint32_t lo = (uint32_t) val;
	uint32_t hi = (uint32_t) (val >> 32);
	__asm__ __volatile__("wrmsr"::"c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ __volatile__("rdmsr":"=a"(lo), "=d"(hi):"c"(msr));
	return ((uint64_t) hi << 32) | lo;
}

void
syscall_init(void)
{
	cpu0.kstack_top = (uint64_t) (uintptr_t) (kstack + sizeof(kstack));
	cpu0.user_rsp = 0;
	cpu0.user_cr3 = 0;
	cpu0.cur_proc_ptr = 0;
	cpu0.resume_rsp = 0;
	cpu0.cpu_id = 0;
	cpu0._pad = 0;
	cpu0.tss_ptr = bsp_tss_addr();

	uint64_t efer = rdmsr(IA32_EFER);
	efer |= 1u | (1u << 11);	/* SCE + NXE. */
	wrmsr(IA32_EFER, efer);

	wrmsr(IA32_GS_BASE, (uint64_t) (uintptr_t) & cpu0);
	wrmsr(IA32_KERNEL_GS_BASE, (uint64_t) (uintptr_t) & cpu0);
	wrmsr(IA32_LSTAR, (uint64_t) (uintptr_t) syscall_entry);
	wrmsr(IA32_FMASK, (1u << 9));	/* Clear IF on entry. */

	uint64_t star =
	    ((uint64_t) USER_CS_BASE << 48) | ((uint64_t) KERNEL_CS << 32);
	wrmsr(IA32_STAR, star);
}

void
syscall_init_ap(int cpu_id, uint64_t kstack_top)
{
	/*
	 * Allocate per-CPU cpu_local struct.
	 * OOM -> halt: without cpu_local, gs:0 is garbage and any
	 * syscall on this AP will triple-fault.
	 */
	struct cpu_local *cl =
	    (struct cpu_local *)kzalloc(sizeof(struct cpu_local));
	if (!cl) {
		for (;;)
			__asm__ __volatile__("cli; hlt");
	}
	cl->kstack_top = kstack_top;
	cl->cpu_id = cpu_id;

	/* Enable SYSCALL/SYSRET and NX. */
	uint64_t efer = rdmsr(IA32_EFER);
	efer |= 1u | (1u << 11);	/* SCE + NXE. */
	wrmsr(IA32_EFER, efer);

	/* Set GS base to this CPU's cpu_local struct. */
	wrmsr(IA32_GS_BASE, (uint64_t) (uintptr_t) cl);
	wrmsr(IA32_KERNEL_GS_BASE, (uint64_t) (uintptr_t) cl);

	/* LSTAR, FMASK, STAR are the same as BSP. */
	wrmsr(IA32_LSTAR, (uint64_t) (uintptr_t) syscall_entry);
	wrmsr(IA32_FMASK, (1u << 9));	/* Clear IF on entry. */

	uint64_t star =
	    ((uint64_t) USER_CS_BASE << 48) | ((uint64_t) KERNEL_CS << 32);
	wrmsr(IA32_STAR, star);
}

void
syscall_set_user_cr3(uint64_t cr3)
{
	__asm__ __volatile__("movq %0, %%gs:16"::"r"(cr3):"memory");
}

uint64_t
cpu0_user_cr3(void)
{
	return cpu0.user_cr3;
}

void
syscall_set_kstack(uint64_t kstack_top)
{
	__asm__ __volatile__("movq %0, %%gs:0"::"r"(kstack_top):"memory");
}

uint64_t
cpu0_kstack_top(void)
{
	return cpu0.kstack_top;
}
