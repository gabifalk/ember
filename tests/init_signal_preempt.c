/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Signal delivery on timer preempt tests -- verifies that signals are
 * delivered to processes spinning in userspace without making syscalls.
 * Covers both custom handler delivery and default-action (SIGTERM) kill.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI). */
#define __NR_read            0
#define __NR_write           1
#define __NR_close           3
#define __NR_rt_sigaction    13
#define __NR_rt_sigreturn    15
#define __NR_pipe            22
#define __NR_nanosleep       35
#define __NR_fork            57
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_kill            62

/* Signal numbers. */
#define SIGUSR1  10
#define SIGTERM  15

/* Flags. */
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

/* kernel_sigaction for x86_64: handler(8), flags(8), restorer(8), mask(8). */
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

/* Sync pipe: child writes a byte after setup, parent reads to wait. */
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

/* Global signal flag for handler. */
static volatile int got_signal = 0;

/* Signal handler that sets the flag. */
static void
handler_set_flag(int sig)
{
	(void)sig;
	got_signal = 1;
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: Custom signal handler delivered during busy loop
 *
 * Child installs SIGUSR1 handler, signals ready via pipe, then enters a
 * tight busy loop with NO syscalls. Parent sends SIGUSR1 after a short
 * delay. The timer preempt path must deliver the signal to the child.
 * ---------------------------------------------------------------------------
 */
static void
test_signal_preempt_handler(void)
{
	msg("  test 1: custom handler on timer preempt\n");

	sys1(__NR_pipe, (long)sync_pipe);

	long pid = sys0(__NR_fork);
	check(pid >= 0, "preempt_h: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: install handler, signal ready, then busy-loop. */
		sys1(__NR_close, sync_pipe[0]);
		got_signal = 0;
		install_handler(SIGUSR1, handler_set_flag);
		sync_child_ready();

		/* Busy loop -- NO syscalls. Signal must arrive via timer preempt. */
		for (volatile long i = 0; i < 500000000L; i++) {
			if (got_signal)
				break;
		}

		if (got_signal)
			sys1(__NR_exit, 0);
		else
			sys1(__NR_exit, 1);
		__builtin_unreachable();
	}

	/* Parent: wait for child to be ready, then send signal after delay. */
	sync_wait_for_child();
	nanosleep_ms(10);
	sys2(__NR_kill, pid, SIGUSR1);

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "preempt_h: wait4 ok");
	int exited = ((status & 0x7f) == 0);
	int termsig = (status & 0x7f);
	int code = (status >> 8) & 0xff;
	check(exited, "preempt_h: child exited normally");
	check(termsig == 0, "preempt_h: child not killed by signal");
	check(code == 0, "preempt_h: child got signal in busy loop");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Default signal action delivered during busy loop
 *
 * Child enters infinite busy loop (no signal handler installed). Parent
 * sends SIGTERM after a short delay. The timer preempt path must deliver
 * the default-action kill to the child.
 * ---------------------------------------------------------------------------
 */
static void
test_signal_preempt_default(void)
{
	msg("  test 2: default action (SIGTERM) on timer preempt\n");

	long pid = sys0(__NR_fork);
	check(pid >= 0, "preempt_d: fork ok");
	if (pid < 0)
		return;

	if (pid == 0) {
		/* Child: infinite busy loop, no handler installed. */
		for (;;)
			;
		__builtin_unreachable();
	}

	/* Parent: give child time to start spinning, then send SIGTERM. */
	nanosleep_ms(20);
	sys2(__NR_kill, pid, SIGTERM);

	/* Wait for child. */
	int status = 0;
	long wpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(wpid == pid, "preempt_d: wait4 ok");
	int termsig = (status & 0x7f);
	check(termsig == SIGTERM, "preempt_d: child killed by SIGTERM");
}

int
main(void)
{
	msg("=== signal preempt tests (P10) ===\n");

	test_signal_preempt_handler();
	test_signal_preempt_default();

	test_done();
	return 0;
}
