/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * MAP_SHARED visibility across fork and COW+brk interaction tests.
 * Verifies that Ember correctly isolates memory after fork (COW semantics)
 * and that brk operations in parent/child don't corrupt each other.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read      0
#define __NR_write     1
#define __NR_mmap      9
#define __NR_munmap    11
#define __NR_brk       12
#define __NR_nanosleep 35
#define __NR_fork      57
#define __NR_exit      60
#define __NR_wait4     61
#define __NR_pipe2     293

/* Mmap constants. */
#define PROT_READ      1
#define PROT_WRITE     2
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20

#define PAGE_SIZE 4096

struct timespec {
	long tv_sec;
	long tv_nsec;
};

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

static long
do_mmap(long addr, long size, long prot, long flags)
{
	return sys6(__NR_mmap, addr, size, prot, flags, -1, 0);
}

static void
do_nanosleep(long ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/*
 * ---------------------------------------------------------------
 * Test 1: MAP_SHARED|MAP_ANONYMOUS visibility across fork
 *
 * Ember treats MAP_SHARED as MAP_PRIVATE (single-address-space
 * limitation), so after fork+COW the child's write should NOT
 * be visible in the parent.
 * ---------------------------------------------------------------
 */
static void
test_shared_anon_across_fork(void)
{
	msg("test_shared_anon_across_fork\n");

	/* Map a "shared" anonymous page. */
	long page = do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_ANONYMOUS);
	check(page > 0 && (page & 0xFFF) == 0, "mmap shared anon");

	/* Write initial value. */
	volatile unsigned char *p = (volatile unsigned char *)page;
	*p = 0x42;

	/* Create pipe for child->parent communication. */
	int fds[2];
	long rc = sys2(__NR_pipe2, (long)fds, 0);
	check(rc == 0, "pipe2 for sync");

	long pid = sys0(__NR_fork);
	check(pid >= 0, "fork");

	if (pid == 0) {
		/* Child: write 0x99 to the "shared" page, then signal parent. */
		*p = 0x99;
		unsigned char done = 1;
		sys3(__NR_write, fds[1], (long)&done, 1);
		do_nanosleep(50);
		sys1(__NR_exit, 0);
	}
	/* Parent: wait for child's write to complete. */
	unsigned char buf;
	sys3(__NR_read, fds[0], (long)&buf, 1);

	/*
	 * Read the page -- if truly shared, we'd see 0x99.
	 * Ember uses COW, so we expect 0x42 (parent's original value).
	 */
	unsigned char parent_val = *p;

	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);

	/* Ember treats MAP_SHARED as private (COW): parent keeps 0x42. */
	check(parent_val == 0x42,
	      "shared anon across fork: COW isolation (parent unchanged)");

	sys2(__NR_munmap, page, PAGE_SIZE);
}

/*
 * ---------------------------------------------------------------
 * Test 2: MAP_PRIVATE|MAP_ANONYMOUS COW isolation
 *
 * After fork, child writes to a private page. Parent must still
 * see its original value.
 * ---------------------------------------------------------------
 */
static void
test_private_anon_cow_isolation(void)
{
	msg("test_private_anon_cow_isolation\n");

	long page = do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS);
	check(page > 0 && (page & 0xFFF) == 0, "mmap private anon");

	volatile unsigned char *p = (volatile unsigned char *)page;
	*p = 0x11;

	/* Pipe for synchronization. */
	int fds[2];
	long rc = sys2(__NR_pipe2, (long)fds, 0);
	check(rc == 0, "pipe2 for sync");

	long pid = sys0(__NR_fork);
	check(pid >= 0, "fork");

	if (pid == 0) {
		/* Child: overwrite the COW page. */
		*p = 0x22;
		unsigned char ok = 1;
		sys3(__NR_write, fds[1], (long)&ok, 1);
		do_nanosleep(50);
		sys1(__NR_exit, 0);
	}
	/* Parent: wait for child to finish writing. */
	unsigned char buf;
	sys3(__NR_read, fds[0], (long)&buf, 1);

	unsigned char parent_val = *p;

	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);

	check(parent_val == 0x11,
	      "COW: parent page unchanged after child write");

	sys2(__NR_munmap, page, PAGE_SIZE);
}

/*
 * ---------------------------------------------------------------
 * Test 3: COW + brk in both parent and child
 *
 * Both sides grow the heap independently after fork. Verify no
 * crash and independent heap contents.
 * ---------------------------------------------------------------
 */
static void
test_cow_brk_parent_child(void)
{
	msg("test_cow_brk_parent_child\n");

	long brk0 = sys1(__NR_brk, 0);
	check(brk0 > 0, "initial brk");

	long pid = sys0(__NR_fork);
	check(pid >= 0, "fork");

	if (pid == 0) {
		/* Child: grow brk by 4096. */
		long new_brk = sys1(__NR_brk, brk0 + 4096);
		if (new_brk < brk0 + 4096) {
			sys1(__NR_exit, 1);
		}
		/* Write pattern to new memory. */
		volatile unsigned char *heap = (volatile unsigned char *)brk0;
		for (int i = 0; i < 4096; i++)
			heap[i] = 0xCC;
		/* Verify pattern. */
		for (int i = 0; i < 4096; i++) {
			if (heap[i] != 0xCC)
				sys1(__NR_exit, 2);
		}
		sys1(__NR_exit, 0);
	}
	/* Parent: grow brk by 8192. */
	long new_brk = sys1(__NR_brk, brk0 + 8192);
	check(new_brk >= brk0 + 8192, "parent brk grow 8192");

	/* Write pattern to new memory. */
	volatile unsigned char *heap = (volatile unsigned char *)brk0;
	for (int i = 0; i < 8192; i++)
		heap[i] = 0xDD;

	/* Wait for child. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	/* WIFEXITED(s) && WEXITSTATUS(s)==0  =>  (status & 0xff)==0 means exited, (status>>8)&0xff == exit code. */
	int exited_ok = ((status & 0x7f) == 0) && (((status >> 8) & 0xff) == 0);
	check(exited_ok, "child brk grow + write succeeded");

	/* Verify parent's pattern is intact. */
	int parent_ok = 1;
	for (int i = 0; i < 8192; i++) {
		if (heap[i] != 0xDD) {
			parent_ok = 0;
			break;
		}
	}
	check(parent_ok, "parent heap pattern intact after child brk");
}

/*
 * ---------------------------------------------------------------
 * Test 4: COW + brk -- child writes don't corrupt parent heap
 *
 * Parent grows brk, writes a pattern, forks. Child grows brk
 * further and overwrites everything. Parent's original data must
 * survive.
 * ---------------------------------------------------------------
 */
static void
test_cow_brk_no_corruption(void)
{
	msg("test_cow_brk_no_corruption\n");

	long brk0 = sys1(__NR_brk, 0);
	check(brk0 > 0, "initial brk");

	/* Grow brk by one page and fill with 0xEE. */
	long new_brk = sys1(__NR_brk, brk0 + 4096);
	check(new_brk >= brk0 + 4096, "brk grow 4096");

	volatile unsigned char *heap = (volatile unsigned char *)brk0;
	for (int i = 0; i < 4096; i++)
		heap[i] = 0xEE;

	long pid = sys0(__NR_fork);
	check(pid >= 0, "fork");

	if (pid == 0) {
		/* Child: grow brk by another page. */
		sys1(__NR_brk, brk0 + 8192);
		/* Overwrite both pages with 0xFF. */
		volatile unsigned char *ch = (volatile unsigned char *)brk0;
		for (int i = 0; i < 8192; i++)
			ch[i] = 0xFF;
		sys1(__NR_exit, 0);
	}
	/* Parent: wait for child to finish. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);

	/* Verify original pattern is untouched (COW isolation) */
	int ok = 1;
	for (int i = 0; i < 4096; i++) {
		if (heap[i] != 0xEE) {
			ok = 0;
			break;
		}
	}
	check(ok, "cow brk: parent heap unchanged after child overwrites");
}

int
main(void)
{
	msg("=== mmap_cross tests ===\n");

	test_shared_anon_across_fork();
	test_private_anon_cow_isolation();
	test_cow_brk_parent_child();
	test_cow_brk_no_corruption();

	test_done();
}
