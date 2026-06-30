/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * COW fork isolation tests -- verifies copy-on-write semantics for
 * mmap'd anonymous pages, stack variables, and multiple children.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_mmap    9
#define __NR_munmap  11
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
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10),
			  "r"(r8), "r"(r9)
			  :"rcx", "r11", "memory");
	return ret;
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
do_wait(long pid, int *status)
{
	return sys4(__NR_wait4, pid, (long)status, 0, 0);
}

static long
mmap_anon(long size)
{
	return sys6(__NR_mmap, 0, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: mmap COW isolation
 *
 * Allocate anonymous page, write 0xDEADBEEF, fork. Child overwrites with
 * 0xCAFEBABE and exits. Parent waits and verifies its value is untouched.
 * ---------------------------------------------------------------------------
 */
static void
test_mmap_cow(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "mmap_cow: mmap ok");
	if (addr <= 0)
		return;

	volatile unsigned int *p = (volatile unsigned int *)addr;
	*p = 0xDEADBEEF;

	long pid = do_fork();
	check(pid >= 0, "mmap_cow: fork ok");
	if (pid < 0) {
		sys2(__NR_munmap, addr, PAGE_SIZE);
		return;
	}

	if (pid == 0) {
		/* Child: overwrite the COW page. */
		volatile unsigned int *cp = (volatile unsigned int *)addr;
		*cp = 0xCAFEBABE;
		/* Verify child sees its own write. */
		if (*cp != 0xCAFEBABE)
			do_exit(1);
		do_exit(0);
	}
	/* Parent: wait for child. */
	int status = 0;
	long rpid = do_wait(pid, &status);
	check(rpid == pid, "mmap_cow: wait returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 0, "mmap_cow: child exited successfully");

	/* Verify parent's value is untouched (COW isolation) */
	check(*p == 0xDEADBEEF, "mmap_cow: parent value intact (0xDEADBEEF)");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Stack COW isolation
 *
 * Set a stack variable, fork. Child modifies it and exits. Parent waits
 * and verifies its stack variable is unchanged.
 * ---------------------------------------------------------------------------
 */
static void
test_stack_cow(void)
{
	volatile unsigned int stack_val = 0x12345678;

	long pid = do_fork();
	check(pid >= 0, "stack_cow: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: modify the stack variable (triggers COW on stack page) */
		stack_val = 0xAABBCCDD;
		/* Verify child sees its own write. */
		if (stack_val != 0xAABBCCDD)
			do_exit(1);
		do_exit(0);
	}
	/* Parent: wait for child. */
	int status = 0;
	long rpid = do_wait(pid, &status);
	check(rpid == pid, "stack_cow: wait returned correct pid");
	int code = (status >> 8) & 0xff;
	check(code == 0, "stack_cow: child exited successfully");

	/* Verify parent's stack variable is untouched. */
	check(stack_val == 0x12345678, "stack_cow: parent stack value intact");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Multiple children COW isolation
 *
 * Fork two children from parent, each writes a different value to the same
 * mmap'd page. Parent waits for both and verifies its page is still the
 * original value.
 * ---------------------------------------------------------------------------
 */
static void
test_multi_child_cow(void)
{
	long addr = mmap_anon(PAGE_SIZE);
	check(addr > 0 && (addr & 0xfff) == 0, "multi_cow: mmap ok");
	if (addr <= 0)
		return;

	volatile unsigned int *p = (volatile unsigned int *)addr;
	*p = 0xDEADBEEF;

	/* Fork child 1. */
	long pid1 = do_fork();
	check(pid1 >= 0, "multi_cow: fork child 1 ok");
	if (pid1 < 0) {
		sys2(__NR_munmap, addr, PAGE_SIZE);
		return;
	}

	if (pid1 == 0) {
		/* Child 1: write 0x11111111. */
		volatile unsigned int *cp = (volatile unsigned int *)addr;
		*cp = 0x11111111;
		if (*cp != 0x11111111)
			do_exit(1);
		do_exit(0);
	}
	/* Fork child 2. */
	long pid2 = do_fork();
	check(pid2 >= 0, "multi_cow: fork child 2 ok");
	if (pid2 < 0) {
		/* Wait for child 1 before returning. */
		int st = 0;
		do_wait(pid1, &st);
		sys2(__NR_munmap, addr, PAGE_SIZE);
		return;
	}

	if (pid2 == 0) {
		/* Child 2: write 0x22222222. */
		volatile unsigned int *cp = (volatile unsigned int *)addr;
		*cp = 0x22222222;
		if (*cp != 0x22222222)
			do_exit(1);
		do_exit(0);
	}
	/* Parent: wait for both children. */
	int status1 = 0, status2 = 0;
	long r1 = do_wait(pid1, &status1);
	long r2 = do_wait(pid2, &status2);

	check(r1 == pid1, "multi_cow: wait child 1 ok");
	check(r2 == pid2, "multi_cow: wait child 2 ok");

	int code1 = (status1 >> 8) & 0xff;
	int code2 = (status2 >> 8) & 0xff;
	check(code1 == 0, "multi_cow: child 1 exited successfully");
	check(code2 == 0, "multi_cow: child 2 exited successfully");

	/* Verify parent's page is still original value. */
	check(*p == 0xDEADBEEF,
	      "multi_cow: parent value intact after 2 children");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== cow-fork tests ===\n");

	test_mmap_cow();
	test_stack_cow();
	test_multi_child_cow();

	test_done();
	return 0;
}
