/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * COW (copy-on-write) fork stress test -- exercises COW page fault resolution
 * across mmap'd regions, brk heap, and concurrent multi-child scenarios.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_mmap    9
#define __NR_munmap  11
#define __NR_brk     12
#define __NR_fork    57
#define __NR_exit    60
#define __NR_wait4   61

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

/* ---- Test 1: Basic single-page COW ---- */

static void
test_basic_cow(void)
{
	long addr = do_mmap(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "cow: mmap");
	if (addr <= 0)
		return;

	volatile int *p = (volatile int *)addr;
	*p = 0xCAFE;

	long pid = do_fork();
	check(pid >= 0, "cow: fork");
	if (pid < 0) {
		do_munmap(addr, PAGE_SIZE);
		return;
	}

	if (pid == 0) {
		/* Child: write a different value, triggering COW fault. */
		*p = 0xBEEF;
		/* Verify child sees its own copy. */
		do_exit(*p == 0xBEEF ? 0 : 1);
	}
	/* Parent: wait for child, then verify our page is untouched. */
	long code = do_wait(pid);
	check(code == 0, "cow: child wrote ok");
	check(*p == 0xCAFE, "cow: parent intact");

	do_munmap(addr, PAGE_SIZE);
}

/* ---- Test 2: Multi-page COW ---- */

static void
test_multipage_cow(void)
{
	long size = 4 * PAGE_SIZE;
	long addr = do_mmap(size);
	check(addr > 0, "cow-multi: mmap");
	if (addr <= 0)
		return;

	volatile int *pages[4];
	for (int i = 0; i < 4; i++) {
		pages[i] = (volatile int *)(addr + i * PAGE_SIZE);
		*pages[i] = 0xA000 + i;
	}

	long pid = do_fork();
	check(pid >= 0, "cow-multi: fork");
	if (pid < 0) {
		do_munmap(addr, size);
		return;
	}

	if (pid == 0) {
		/* Child modifies all 4 pages. */
		int ok = 1;
		for (int i = 0; i < 4; i++) {
			/* Verify child initially sees parent's values. */
			if (*pages[i] != 0xA000 + i)
				ok = 0;
			/* Write new value (triggers COW) */
			*pages[i] = 0xB000 + i;
			if (*pages[i] != 0xB000 + i)
				ok = 0;
		}
		do_exit(ok ? 0 : 1);
	}

	long code = do_wait(pid);
	check(code == 0, "cow-multi: child ok");

	/* Parent verifies all original values intact. */
	int intact = 1;
	for (int i = 0; i < 4; i++) {
		if (*pages[i] != 0xA000 + i)
			intact = 0;
	}
	check(intact, "cow-multi: parent intact");

	do_munmap(addr, size);
}

/* ---- Test 3: COW with brk ---- */

static void
test_brk_cow(void)
{
	long base = sys1(__NR_brk, 0);
	check(base > 0, "cow-brk: query");
	if (base <= 0)
		return;

	/* Extend brk by 2 pages. */
	long new_brk = sys1(__NR_brk, base + 2 * PAGE_SIZE);
	check(new_brk >= base + 2 * PAGE_SIZE, "cow-brk: extend");
	if (new_brk < base + 2 * PAGE_SIZE)
		return;

	volatile int *p1 = (volatile int *)base;
	volatile int *p2 = (volatile int *)(base + PAGE_SIZE);
	*p1 = 0x1111;
	*p2 = 0x2222;

	long pid = do_fork();
	check(pid >= 0, "cow-brk: fork");
	if (pid < 0) {
		sys1(__NR_brk, base);
		return;
	}

	if (pid == 0) {
		/* Child: extend brk further and write. */
		long child_brk = sys1(__NR_brk, base + 4 * PAGE_SIZE);
		if (child_brk < base + 4 * PAGE_SIZE)
			do_exit(2);

		/* Write to original pages (COW) and new pages. */
		*p1 = 0x3333;
		*p2 = 0x4444;
		volatile int *p3 = (volatile int *)(base + 2 * PAGE_SIZE);
		*p3 = 0x5555;

		int ok = (*p1 == 0x3333 && *p2 == 0x4444 && *p3 == 0x5555);
		do_exit(ok ? 0 : 1);
	}

	long code = do_wait(pid);
	check(code == 0, "cow-brk: child ok");
	check(*p1 == 0x1111, "cow-brk: parent p1");
	check(*p2 == 0x2222, "cow-brk: parent p2");

	/* Restore brk. */
	sys1(__NR_brk, base);
}

/* ---- Test 4: Concurrent COW (4 children) ---- */

static void
test_concurrent_cow(void)
{
	long size = 4 * PAGE_SIZE;
	long addr = do_mmap(size);
	check(addr > 0, "cow-conc: mmap");
	if (addr <= 0)
		return;

	/* Parent writes sentinel values to each page. */
	for (int i = 0; i < 4; i++) {
		volatile int *p = (volatile int *)(addr + i * PAGE_SIZE);
		*p = 0xD000 + i;
	}

	long pids[4];
	for (int i = 0; i < 4; i++) {
		long pid = do_fork();
		check(pid >= 0, "cow-conc: fork");
		if (pid < 0) {
			/* Wait for any already-forked children before bailing. */
			for (int j = 0; j < i; j++)
				do_wait(pids[j]);
			do_munmap(addr, size);
			return;
		}
		if (pid == 0) {
			/* Each child writes to its own page offset (triggers COW) */
			volatile int *my_page =
			    (volatile int *)(addr + i * PAGE_SIZE);
			*my_page = 0xE000 + i;
			/* Also read all pages to stress sharing. */
			int ok = (*my_page == 0xE000 + i);
			do_exit(ok ? 0 : 1);
		}
		pids[i] = pid;
	}

	/* Parent waits for all children. */
	int all_ok = 1;
	for (int i = 0; i < 4; i++) {
		long code = do_wait(pids[i]);
		if (code != 0)
			all_ok = 0;
	}
	check(all_ok, "cow-conc: children ok");

	/* Verify parent's pages are all untouched. */
	int intact = 1;
	for (int i = 0; i < 4; i++) {
		volatile int *p = (volatile int *)(addr + i * PAGE_SIZE);
		if (*p != 0xD000 + i)
			intact = 0;
	}
	check(intact, "cow-conc: parent intact");

	do_munmap(addr, size);
}

int
main(void)
{
	msg("=== cow tests ===\n");

	test_basic_cow();
	test_multipage_cow();
	test_brk_cow();
	test_concurrent_cow();

	test_done();
	return 0;
}
