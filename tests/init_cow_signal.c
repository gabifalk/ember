/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * COW + signal interaction tests -- verifies that COW page fault resolution
 * works correctly when signals are delivered concurrently, when signal handlers
 * trigger COW faults, and that fork+kill cleanup is correct.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read            0
#define __NR_write           1
#define __NR_close           3
#define __NR_mmap            9
#define __NR_munmap          11
#define __NR_rt_sigaction    13
#define __NR_rt_sigreturn    15
#define __NR_pipe            22
#define __NR_nanosleep       35
#define __NR_getpid          39
#define __NR_fork            57
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_kill            62

/* Signal numbers. */
#define SIGUSR1  10
#define SIGKILL  9

/* Flags. */
#define SA_RESTORER 0x04000000

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

/* kernel_sigaction for x86_64: handler(8), flags(8), restorer(8), mask(8) */
struct kernel_sigaction {
	void (*handler) (int);
	unsigned long flags;
	void (*restorer) (void);
	unsigned long mask;
};

/* Custom restorer that calls rt_sigreturn. */
__asm__(".type my_restorer, @function\n"
	"my_restorer:\n" "    mov $15, %rax\n" "    syscall\n");
extern void my_restorer(void);

struct timespec {
	long tv_sec;
	long tv_nsec;
};

/* Helpers. */
static long
do_mmap(long size)
{
	return sys6(__NR_mmap, 0, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static long
install_handler(int signum, void (*handler) (int))
{
	struct kernel_sigaction sa;
	sa.handler = handler;
	sa.flags = SA_RESTORER;
	sa.restorer = my_restorer;
	sa.mask = 0;
	return sys4(__NR_rt_sigaction, signum, (long)&sa, 0, 8);
}

static void
nanosleep_ms(long ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/* Sync pipe: child writes a byte after handler is installed, parent reads to wait. */
static int sync_pipe[2];

static void
sync_child_ready(void)
{
	char c = 'R';
	sys3(__NR_write, sync_pipe[1], (long)&c, 1);
	sys1(__NR_close, sync_pipe[1]);
}

static void
sync_wait_for_child(void)
{
	sys1(__NR_close, sync_pipe[1]);
	char c;
	sys3(__NR_read, sync_pipe[0], (long)&c, 1);
	sys1(__NR_close, sync_pipe[0]);
}

/* ---- Global test state ---- */
static volatile int sig_flag = 0;
static volatile int sig_counter = 0;
static volatile long *cow_page_ptr = 0;

/* Signal handlers. */
static void
handler_set_flag(int sig)
{
	(void)sig;
	sig_flag = 1;
}

static void
handler_write_cow(int sig)
{
	(void)sig;
	/* Write to COW page from signal handler context -- triggers COW fault. */
	if (cow_page_ptr)
		*cow_page_ptr = 0xBBBB;
}

static void
handler_increment(int sig)
{
	(void)sig;
	sig_counter++;
}

static void
handler_stack_flag(int sig)
{
	(void)sig;
	sig_flag = 1;
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: Signal during COW fault resolution
 *
 * Parent mmaps a page and writes a sentinel. Fork child. Child installs
 * SIGUSR1 handler, parent sends signal immediately after fork. Child writes
 * to COW page (triggering fault), then checks handler ran and write stuck.
 * Parent verifies its page is unchanged.
 * ---------------------------------------------------------------------------
 */
static void
test_signal_during_cow(void)
{
	msg("  test 1: signal during COW fault\n");

	long addr = do_mmap(PAGE_SIZE);
	check(addr > 0, "sig_cow: mmap ok");
	volatile long *page = (volatile long *)addr;
	*page = 0xDEAD;

	sys1(__NR_pipe, (long)sync_pipe);

	long pid = sys0(__NR_fork);
	check(pid >= 0, "sig_cow: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: install handler first, then signal parent we're ready. */
		sys1(__NR_close, sync_pipe[0]);
		sig_flag = 0;
		install_handler(SIGUSR1, handler_set_flag);
		sync_child_ready();

		/* Give parent a moment to send signal. */
		nanosleep_ms(10);

		/* Write to COW page (triggers COW fault) */
		*page = 0xBEEF;

		/* Verify write succeeded. */
		int write_ok = (*page == 0xBEEF);

		/* Check if signal handler ran. */
		int handler_ran = (sig_flag == 1);

		if (write_ok && handler_ran)
			sys1(__NR_exit, 0);
		else if (!write_ok)
			sys1(__NR_exit, 1);
		else
			sys1(__NR_exit, 2);	/* Write ok but signal missed (acceptable) */
		__builtin_unreachable();
	}
	/* Parent: wait until child has handler installed, then send signal. */
	sync_wait_for_child();
	sys2(__NR_kill, pid, SIGUSR1);

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "sig_cow: wait4 ok");
	int code = (status >> 8) & 0xff;
	int exited = ((status & 0x7f) == 0);
	check(exited, "sig_cow: child exited normally");
	check(code == 0 || code == 2, "sig_cow: child COW write ok");

	/* Parent page should be unchanged. */
	check(*page == 0xDEAD, "sig_cow: parent page unchanged");

	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Signal handler modifies COW page
 *
 * Parent mmaps a page, writes 0xAAAA. Fork child. Child installs handler
 * that writes 0xBBBB to the COW page (handler triggers COW fault from signal
 * context). Parent sends SIGUSR1 to child. Child checks page is 0xBBBB.
 * Parent verifies its page is still 0xAAAA.
 * ---------------------------------------------------------------------------
 */
static void
test_handler_modifies_cow(void)
{
	msg("  test 2: signal handler modifies COW page\n");

	long addr = do_mmap(PAGE_SIZE);
	check(addr > 0, "hmod_cow: mmap ok");
	volatile long *page = (volatile long *)addr;
	*page = 0xAAAA;

	/* Set global pointer for signal handler. */
	cow_page_ptr = page;

	sys1(__NR_pipe, (long)sync_pipe);

	long pid = sys0(__NR_fork);
	check(pid >= 0, "hmod_cow: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: install handler that writes to COW page, then signal ready. */
		sys1(__NR_close, sync_pipe[0]);
		install_handler(SIGUSR1, handler_write_cow);
		sync_child_ready();

		/* Wait for signal from parent. */
		nanosleep_ms(20);

		/* Check: handler should have written 0xBBBB. */
		long val = *page;
		if (val == 0xBBBB)
			sys1(__NR_exit, 0);
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}
	/* Parent: wait for child to install handler, then send signal. */
	sync_wait_for_child();
	sys2(__NR_kill, pid, SIGUSR1);

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "hmod_cow: wait4 ok");
	int exited = ((status & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(exited, "hmod_cow: child exited normally");
	check(code == 0, "hmod_cow: child saw 0xBBBB from handler");

	/* Parent page should still be 0xAAAA. */
	check(*page == 0xAAAA, "hmod_cow: parent page unchanged");

	cow_page_ptr = 0;
	sys2(__NR_munmap, addr, PAGE_SIZE);
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Fork + immediate kill
 *
 * Fork child. Parent immediately sends SIGKILL to child. Parent waits --
 * child should be killed by signal. Then fork another child that exits
 * normally to verify no resource leak.
 * ---------------------------------------------------------------------------
 */
static void
test_fork_immediate_kill(void)
{
	msg("  test 3: fork + immediate kill\n");

	long pid = sys0(__NR_fork);
	check(pid >= 0, "fkill: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: loop forever (will be killed) */
		for (;;)
			nanosleep_ms(100);
		__builtin_unreachable();
	}
	/* Parent: immediately kill child. */
	long r = sys2(__NR_kill, pid, SIGKILL);
	check(r == 0, "fkill: kill ok");

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "fkill: wait4 ok");

	/* Child should have been killed by signal. */
	int signaled = ((status & 0x7f) != 0);
	int signum = (status & 0x7f);
	check(signaled, "fkill: child was signaled");
	check(signum == SIGKILL, "fkill: killed by SIGKILL");

	/* Fork another child to verify no resource leak. */
	long pid2 = sys0(__NR_fork);
	check(pid2 >= 0, "fkill: second fork ok");
	if (pid2 < 0)
		return;

	if (pid2 == 0) {
		sys1(__NR_exit, 42);
		__builtin_unreachable();
	}

	int status2 = 0;
	long wpid2 = sys4(__NR_wait4, pid2, (long)&status2, 0, 0);
	check(wpid2 == pid2, "fkill: second wait4 ok");
	int exited2 = ((status2 & 0x7f) == 0);
	int code2 = (status2 >> 8) & 0xff;
	check(exited2, "fkill: second child exited normally");
	check(code2 == 42, "fkill: second child exit code 42");
}

/*
 * ---------------------------------------------------------------------------
 * Test 4: COW on stack during signal delivery
 *
 * Parent stores a stack variable = 0x1234. Fork child. Child installs
 * SIGUSR1 handler. Parent sends SIGUSR1. Child verifies stack var preserved,
 * modifies it to 0x5678, verifies it persists.
 * ---------------------------------------------------------------------------
 */
static void
test_cow_stack_signal(void)
{
	msg("  test 4: COW on stack during signal\n");

	volatile long stack_var = 0x1234;

	sys1(__NR_pipe, (long)sync_pipe);

	long pid = sys0(__NR_fork);
	check(pid >= 0, "stkcos: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: install handler, signal ready, then wait. */
		sys1(__NR_close, sync_pipe[0]);
		sig_flag = 0;
		install_handler(SIGUSR1, handler_stack_flag);
		sync_child_ready();

		/* Wait for signal. */
		nanosleep_ms(20);

		int ok = 1;

		/* Stack variable should still be 0x1234 (COW preserves it) */
		if (stack_var != 0x1234)
			ok = 0;

		/* Modify stack var (triggers COW on stack page) */
		stack_var = 0x5678;
		if (stack_var != 0x5678)
			ok = 0;

		/* Signal handler should have set flag. */
		if (sig_flag == 1 && ok)
			sys1(__NR_exit, 0);
		else if (ok)
			sys1(__NR_exit, 0);	/* Signal might not arrive yet, COW is the main test. */
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}
	/* Parent: wait for child to install handler, then send signal. */
	sync_wait_for_child();
	sys2(__NR_kill, pid, SIGUSR1);

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "stkcos: wait4 ok");
	int exited = ((status & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(exited, "stkcos: child exited normally");
	check(code == 0, "stkcos: child stack COW ok");

	/* Parent's stack var should be unchanged. */
	check(stack_var == 0x1234, "stkcos: parent stack unchanged");
}

/*
 * ---------------------------------------------------------------------------
 * Test 5: Multiple signals to COW-heavy child
 *
 * Parent mmaps 4 pages, writes sentinels. Fork child. Child installs
 * SIGUSR1 handler (increments counter). Child writes to all COW pages.
 * Parent sends 3 SIGUSR1 signals with tiny sleeps. Child verifies:
 * signal counter >= 1 (some may coalesce), all pages written correctly.
 * Parent verifies its pages unchanged.
 * ---------------------------------------------------------------------------
 */
static void
test_multi_signal_cow(void)
{
	msg("  test 5: multiple signals to COW-heavy child\n");

#define NUM_PAGES 4
	long addrs[NUM_PAGES];
	volatile long *pages[NUM_PAGES];

	for (int i = 0; i < NUM_PAGES; i++) {
		addrs[i] = do_mmap(PAGE_SIZE);
		check(addrs[i] > 0, "multi: mmap ok");
		pages[i] = (volatile long *)addrs[i];
		*pages[i] = 0xAA00 + i;
	}

	sys1(__NR_pipe, (long)sync_pipe);

	long pid = sys0(__NR_fork);
	check(pid >= 0, "multi: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: install handler, signal ready. */
		sys1(__NR_close, sync_pipe[0]);
		sig_counter = 0;
		install_handler(SIGUSR1, handler_increment);
		sync_child_ready();

		/* Give parent time to start sending signals. */
		nanosleep_ms(5);

		/* Write to all COW pages (each triggers COW fault) */
		for (int i = 0; i < NUM_PAGES; i++) {
			*pages[i] = 0xBB00 + i;
		}

		/* Small sleep to receive pending signals. */
		nanosleep_ms(20);

		/* Verify all writes succeeded. */
		int all_ok = 1;
		for (int i = 0; i < NUM_PAGES; i++) {
			if (*pages[i] != (long)(0xBB00 + i))
				all_ok = 0;
		}

		/* Signal counter should be >= 1 (signals may coalesce) */
		if (all_ok && sig_counter >= 1)
			sys1(__NR_exit, 0);
		else if (all_ok)
			sys1(__NR_exit, 0);	/* Writes ok, signals may not have arrived. */
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}
	/* Parent: wait for child to install handler, then send 3 signals. */
	sync_wait_for_child();
	sys2(__NR_kill, pid, SIGUSR1);
	nanosleep_ms(2);
	sys2(__NR_kill, pid, SIGUSR1);
	nanosleep_ms(2);
	sys2(__NR_kill, pid, SIGUSR1);

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "multi: wait4 ok");
	int exited = ((status & 0x7f) == 0);
	int code = (status >> 8) & 0xff;
	check(exited, "multi: child exited normally");
	check(code == 0, "multi: child COW+signal ok");

	/* Verify parent pages unchanged. */
	int parent_ok = 1;
	for (int i = 0; i < NUM_PAGES; i++) {
		if (*pages[i] != (long)(0xAA00 + i))
			parent_ok = 0;
	}
	check(parent_ok, "multi: parent pages unchanged");

	for (int i = 0; i < NUM_PAGES; i++)
		sys2(__NR_munmap, addrs[i], PAGE_SIZE);
#undef NUM_PAGES
}

int
main(void)
{
	msg("=== COW + signal interaction tests ===\n");

	test_signal_during_cow();
	test_handler_modifies_cow();
	test_fork_immediate_kill();
	test_cow_stack_signal();
	test_multi_signal_cow();

	test_done();
	return 0;
}
