/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * SMP COW stress test -- exercises COW page fault resolution under
 * concurrent CPU access (model property P2: writable page refcount == 1).
 * With 4 CPUs, parent and child can be scheduled on different cores and
 * both trigger COW faults on the same page simultaneously.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI). */
#define __NR_mmap    9
#define __NR_munmap  11
#define __NR_fork   57
#define __NR_exit   60
#define __NR_wait4  61

/* Mmap constants. */
#define PROT_READ    1
#define PROT_WRITE   2
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x20

#define PAGE_SIZE 4096

/* Raw syscall wrappers. */
static long
sys0(long nr)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr):"rcx", "r11", "memory");
	return ret;
}

static long
sys1(long nr, long a1)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1):"rcx", "r11",
			  "memory");
	return ret;
}

static long
sys2(long nr, long a1, long a2)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2):"rcx",
			  "r11", "memory");
	return ret;
}

static long
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10):"rcx", "r11", "memory");
	return ret;
}

static long
sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10), "r"(r8), "r"(r9):"rcx", "r11",
			  "memory");
	return ret;
}

static long
do_mmap(long size)
{
	return sys6(__NR_mmap, 0, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static void
do_munmap(long addr, long size)
{
	sys2(__NR_munmap, addr, size);
}

static long
do_fork(void)
{
	return sys0(__NR_fork);
}

static void
do_exit(long code)
{
	sys1(__NR_exit, code);
	__builtin_unreachable();
}

static long
do_wait(long pid)
{
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	if (r < 0)
		return r;
	return (status >> 8) & 0xff;
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: Single-page concurrent COW.
 *
 * Parent and child both write to the same COW-shared page immediately after
 * fork. On SMP they can race into COW resolution simultaneously. P2 requires
 * each ends up with its own physical page (refcount == 1 when writable).
 * ---------------------------------------------------------------------------
 */
static void
test_cow_concurrent_single(void)
{
	long addr = do_mmap(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "smp-cow-single: mmap");
	if (addr <= 0)
		return;

	volatile unsigned int *p = (volatile unsigned int *)addr;

	/* Pre-fault the page so fork shares it as COW. */
	*p = 0xAAAA;

	long pid = do_fork();
	check(pid >= 0, "smp-cow-single: fork");
	if (pid < 0) {
		do_munmap(addr, PAGE_SIZE);
		return;
	}

	if (pid == 0) {
		/* Child: immediately write, triggering COW fault. */
		*p = 0xCCCC0001;

		/* Spin to give parent time to also fault concurrently. */
		volatile int sink = 0;
		for (int i = 0; i < 200000; i++)
			sink += i;
		(void)sink;

		/* Verify child still sees its own value (no aliasing). */
		do_exit(*p == 0xCCCC0001 ? 0 : 1);
	}

	/* Parent: immediately write, triggering COW fault. */
	*p = 0xAABB0001;

	/* Wait for child. */
	long code = do_wait(pid);
	check(code == 0, "smp-cow-single: child value intact");
	check(*p == 0xAABB0001, "smp-cow-single: parent value intact");

	do_munmap(addr, PAGE_SIZE);
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Multi-page concurrent COW (8 pages).
 *
 * Same pattern across 8 pages. Both parent and child write distinct values
 * to every page and verify no cross-contamination.
 * ---------------------------------------------------------------------------
 */
static void
test_cow_concurrent_multi(void)
{
	int npages = 8;
	long size = npages * PAGE_SIZE;
	long addr = do_mmap(size);
	check(addr > 0, "smp-cow-multi: mmap");
	if (addr <= 0)
		return;

	volatile unsigned int *pages[8];
	for (int i = 0; i < npages; i++) {
		pages[i] = (volatile unsigned int *)(addr + i * PAGE_SIZE);
		/* Pre-fault all pages. */
		*pages[i] = 0xAA00 + i;
	}

	long pid = do_fork();
	check(pid >= 0, "smp-cow-multi: fork");
	if (pid < 0) {
		do_munmap(addr, size);
		return;
	}

	if (pid == 0) {
		/* Child writes distinct values to all pages. */
		for (int i = 0; i < npages; i++)
			*pages[i] = 0xCC00 + i;

		/* Verify all pages have child values. */
		int ok = 1;
		for (int i = 0; i < npages; i++) {
			if (*pages[i] != (unsigned int)(0xCC00 + i))
				ok = 0;
		}
		do_exit(ok ? 0 : 1);
	}

	/* Parent writes distinct values to all pages. */
	for (int i = 0; i < npages; i++)
		*pages[i] = 0xAA00 + i;

	long code = do_wait(pid);
	check(code == 0, "smp-cow-multi: child values intact");

	/* Verify parent values survived. */
	int intact = 1;
	for (int i = 0; i < npages; i++) {
		if (*pages[i] != (unsigned int)(0xAA00 + i))
			intact = 0;
	}
	check(intact, "smp-cow-multi: parent values intact");

	do_munmap(addr, size);
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: COW stress (20 rounds).
 *
 * Repeated mmap-fork-write-verify-munmap cycles. Each round both parent and
 * child race to resolve COW on a freshly shared page.
 * ---------------------------------------------------------------------------
 */
static void
test_cow_stress(void)
{
	int ok = 1;

	for (int round = 0; round < 20; round++) {
		long addr = do_mmap(PAGE_SIZE);
		if (addr <= 0) {
			ok = 0;
			break;
		}

		volatile unsigned int *p = (volatile unsigned int *)addr;

		/* Pre-fault with round-specific value. */
		*p = 0xDD000000 + round;

		long pid = do_fork();
		if (pid < 0) {
			ok = 0;
			do_munmap(addr, PAGE_SIZE);
			break;
		}

		if (pid == 0) {
			/* Child writes its own value. */
			*p = 0xEE000000 + round;
			do_exit(*p == (unsigned int)(0xEE000000 + round) ? 0 : 1);
		}

		/* Parent writes its own value. */
		*p = 0xFF000000 + round;

		long code = do_wait(pid);
		if (code != 0)
			ok = 0;
		if (*p != (unsigned int)(0xFF000000 + round))
			ok = 0;

		do_munmap(addr, PAGE_SIZE);
	}

	check(ok, "smp-cow-stress: 20 rounds passed");
}

int
main(void)
{
	msg("=== SMP COW tests (P2) ===\n");

	test_cow_concurrent_single();
	test_cow_concurrent_multi();
	test_cow_stress();

	test_done();
	return 0;
}
