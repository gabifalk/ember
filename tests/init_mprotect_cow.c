/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * mprotect + COW regression test -- verifies that mprotect(RO) followed by
 * mprotect(RW) preserves COW semantics so forked parent and child do not
 * silently share a writable page.  Regression test for fix ea0939a.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI). */
#define __NR_mmap           9
#define __NR_mprotect       10
#define __NR_munmap         11
#define __NR_nanosleep      35
#define __NR_fork           57
#define __NR_exit           60
#define __NR_wait4          61

/* Mmap/mprotect constants. */
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
sys3(long nr, long a1, long a2, long a3)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3):"rcx", "r11", "memory");
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

struct timespec {
	long tv_sec;
	long tv_nsec;
};

/* Helper: mmap anonymous RW pages. */
static long
do_mmap(long size)
{
	return sys6(__NR_mmap, 0, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* Helper: busy-loop delay. */
static void
busy_wait(void)
{
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 50000000L;	/* 50 ms. */
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: mprotect(RO) then mprotect(RW) on a single COW page.
 *
 * Parent mmaps a page RW, writes 0xAAAA. Fork -- page is COW-shared.
 * Parent: mprotect(PROT_READ), then mprotect(PROT_READ|PROT_WRITE).
 * Parent: write 0xBBBB -- must trigger COW, giving parent its own copy.
 * Child: wait briefly, read page -- must still see 0xAAAA.
 * Parent waits for child, checks exit code 0 and that parent sees 0xBBBB.
 * ---------------------------------------------------------------------------
 */
static void
test_mprotect_cow_split(void)
{
	msg("  test 1: mprotect RO->RW preserves COW (single page)\n");

	long addr = do_mmap(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "split: mmap ok");
	volatile int *page = (volatile int *)addr;
	*page = 0xAAAA;

	long pid = sys0(__NR_fork);
	check(pid >= 0, "split: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: wait for parent to do mprotect + write. */
		busy_wait();

		/* Page should still contain original value. */
		int val = *page;
		if (val == 0xAAAA)
			sys1(__NR_exit, 0);
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}

	/* Parent: mprotect RO then back to RW. */
	long r = sys3(__NR_mprotect, addr, PAGE_SIZE, PROT_READ);
	check(r == 0, "split: mprotect PROT_READ ok");

	r = sys3(__NR_mprotect, addr, PAGE_SIZE, PROT_READ | PROT_WRITE);
	check(r == 0, "split: mprotect PROT_READ|PROT_WRITE ok");

	/* Write must trigger COW -- parent gets its own copy. */
	*page = 0xBBBB;
	check(*page == 0xBBBB, "split: parent sees 0xBBBB after write");

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "split: wait4 ok");

	int exited = ((status & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(exited, "split: child exited normally");
	check(code == 0, "split: child still sees 0xAAAA");

	/* Parent should still see its own written value. */
	check(*page == 0xBBBB, "split: parent page is 0xBBBB");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: mprotect(RO) then mprotect(RW) on 4 COW pages.
 *
 * Same pattern as test 1 but with a 4-page region.  mprotect covers the
 * whole range.  Each page must COW independently.
 * ---------------------------------------------------------------------------
 */
static void
test_mprotect_cow_multi(void)
{
	msg("  test 2: mprotect RO->RW preserves COW (4 pages)\n");

#define NUM_PAGES 4
	long addr = do_mmap(PAGE_SIZE * NUM_PAGES);
	check(addr > 0 && (addr & 0xfff) == 0, "multi: mmap ok");

	volatile int *pages[NUM_PAGES];
	for (int i = 0; i < NUM_PAGES; i++) {
		pages[i] = (volatile int *)(addr + i * PAGE_SIZE);
		*pages[i] = 0xAA00 + i;
	}

	long pid = sys0(__NR_fork);
	check(pid >= 0, "multi: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: wait for parent to do mprotect + writes. */
		busy_wait();

		/* Each page should still contain the original sentinel. */
		int ok = 1;
		for (int i = 0; i < NUM_PAGES; i++) {
			if (*pages[i] != 0xAA00 + i)
				ok = 0;
		}

		if (ok)
			sys1(__NR_exit, 0);
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}

	/* Parent: mprotect the whole range RO then back to RW. */
	long r = sys3(__NR_mprotect, addr, PAGE_SIZE * NUM_PAGES, PROT_READ);
	check(r == 0, "multi: mprotect PROT_READ ok");

	r = sys3(__NR_mprotect, addr, PAGE_SIZE * NUM_PAGES,
		 PROT_READ | PROT_WRITE);
	check(r == 0, "multi: mprotect PROT_READ|PROT_WRITE ok");

	/* Write each page -- must trigger COW independently. */
	for (int i = 0; i < NUM_PAGES; i++)
		*pages[i] = 0xBB00 + i;

	/* Verify parent sees its own writes. */
	int parent_ok = 1;
	for (int i = 0; i < NUM_PAGES; i++) {
		if (*pages[i] != 0xBB00 + i)
			parent_ok = 0;
	}
	check(parent_ok, "multi: parent sees 0xBB0x in all pages");

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "multi: wait4 ok");

	int exited = ((status & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(exited, "multi: child exited normally");
	check(code == 0, "multi: child still sees original sentinels");

	sys2(__NR_munmap, addr, PAGE_SIZE * NUM_PAGES);
#undef NUM_PAGES
}

int
main(void)
{
	msg("=== mprotect + COW tests (P8) ===\n");
	test_mprotect_cow_split();
	test_mprotect_cow_multi();
	test_done();
	return 0;
}
