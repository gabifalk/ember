/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * mprotect + SIGSEGV test -- exercises mprotect page protection changes
 * and verifies SIGSEGV delivery on protection violations.
 */

#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_mmap           9
#define __NR_mprotect       10
#define __NR_munmap         11
#define __NR_rt_sigaction   13
#define __NR_rt_sigreturn   15
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62

/* Mmap/mprotect constants. */
#define PROT_NONE    0
#define PROT_READ    1
#define PROT_WRITE   2
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x20

#define PAGE_SIZE 4096

/* Signal constants. */
#define SIGSEGV     11
#define SA_RESTORER 0x04000000

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

/* kernel_sigaction for x86_64. */
struct kernel_sigaction {
	void (*handler) (int);
	unsigned long flags;
	void (*restorer) (void);
	unsigned long mask;
};

/* Custom restorer (naked asm to avoid prologue corruption) */
__asm__(".type my_restorer, @function\n"
	"my_restorer:\n" "    mov $15, %rax\n" "    syscall\n");
extern void my_restorer(void);

/* Helper: mmap anonymous RW page(s) */
static long
do_mmap(long size, int prot)
{
	return sys6(__NR_mmap, 0, size, prot,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/*
 * Helper: fork a child that performs an action expected to SIGSEGV.
 * Returns 1 if child was killed by SIGSEGV, 0 otherwise.
 */
static int
expect_sigsegv_in_child(void (*child_action) (long), long arg)
{
	long pid = sys0(__NR_fork);
	if (pid < 0)
		return 0;

	if (pid == 0) {
		/*
		 * Child: perform the faulting action.
		 * If it doesn't fault, exit with code 42 to signal "no crash".
		 */
		child_action(arg);
		sys1(__NR_exit, 42);
		__builtin_unreachable();
	}
	/* Parent: wait for child. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);

	/*
	 * Check if child was killed by signal 11 (SIGSEGV)
	 * WIFSIGNALED: (status & 0x7f) != 0 && (status & 0x7f) != 0x7f
	 * WTERMSIG: status & 0x7f.
	 */
	int termsig = status & 0x7f;
	return (termsig == SIGSEGV);
}

/* ---- Child action functions (called in forked child) ---- */

static void
child_write_to_addr(long addr)
{
	volatile int *p = (volatile int *)addr;
	*p = 0xDEAD;
}

static void
child_read_from_addr(long addr)
{
	volatile int *p = (volatile int *)addr;
	volatile int v = *p;
	(void)v;
}

/* ---- Test 1: Anonymous mmap RW ---- */
static long test_page;		/* Shared across tests. */

static void
test_mmap_rw(void)
{
	test_page = do_mmap(PAGE_SIZE, PROT_READ | PROT_WRITE);
	check(test_page > 0
	      && (test_page & 0xfff) == 0, "mmap: anon RW returned valid addr");

	/* Write and read back. */
	volatile int *p = (volatile int *)test_page;
	*p = 0xCAFEBABE;
	check(*p == (int)0xCAFEBABE, "mmap: write then read back");
}

/* ---- Test 2: mprotect to read-only ---- */
static void
test_mprotect_readonly(void)
{
	long r = sys3(__NR_mprotect, test_page, PAGE_SIZE, PROT_READ);
	check(r == 0, "mprotect: set PROT_READ returns 0");

	/* Read should still work. */
	volatile int *p = (volatile int *)test_page;
	check(*p == (int)0xCAFEBABE,
	      "mprotect: read still works after PROT_READ");
}

/* ---- Test 3: SIGSEGV on write to read-only page ---- */
static void
test_sigsegv_write_readonly(void)
{
	/*
	 * Page is still PROT_READ from test 2.
	 * Fork a child that writes to it -- should get SIGSEGV.
	 */
	int got_segv = expect_sigsegv_in_child(child_write_to_addr, test_page);
	check(got_segv, "sigsegv: child killed by SIGSEGV on write to RO page");
}

/* ---- Test 4: mprotect back to RW ---- */
static void
test_mprotect_rw_again(void)
{
	long r =
	    sys3(__NR_mprotect, test_page, PAGE_SIZE, PROT_READ | PROT_WRITE);
	check(r == 0, "mprotect: set PROT_READ|PROT_WRITE returns 0");

	/* Write should work again. */
	volatile int *p = (volatile int *)test_page;
	*p = 0x12345678;
	check(*p == 0x12345678, "mprotect: write works after restoring RW");
}

/* ---- Test 5: munmap ---- */
static void
test_munmap(void)
{
	long r = sys2(__NR_munmap, test_page, PAGE_SIZE);
	check(r == 0, "munmap: returns 0");
}

/* ---- Test 6: PROT_NONE -- any access should SIGSEGV ---- */
static void
test_prot_none(void)
{
	long addr = do_mmap(PAGE_SIZE, PROT_NONE);
	check(addr > 0 && (addr & 0xfff) == 0, "prot_none: mmap PROT_NONE ok");
	if (addr <= 0)
		return;

	/* Read should SIGSEGV. */
	int segv_read = expect_sigsegv_in_child(child_read_from_addr, addr);
	check(segv_read, "prot_none: SIGSEGV on read");

	/* Write should SIGSEGV. */
	int segv_write = expect_sigsegv_in_child(child_write_to_addr, addr);
	check(segv_write, "prot_none: SIGSEGV on write");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

int
main(void)
{
	msg("=== mprotect tests ===\n");

	test_mmap_rw();
	test_mprotect_readonly();
	test_sigsegv_write_readonly();
	test_mprotect_rw_again();
	test_munmap();
	test_prot_none();

	test_done();
	return 0;
}
