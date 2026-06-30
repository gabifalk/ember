/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Signal/syscall cross-cutting interaction tests -- exercises EINTR from
 * pipe read, masked signal non-interruption, and rapid multi-signal delivery.
 */

#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read           0
#define __NR_write          1
#define __NR_close          3
#define __NR_rt_sigaction   13
#define __NR_rt_sigprocmask 14
#define __NR_rt_sigreturn   15
#define __NR_nanosleep      35
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62
#define __NR_pipe2          293

/* Signal numbers. */
#define SIGUSR1      10
#define SIGUSR1_MASK (1UL << 10)

/* Sigprocmask how values. */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1

/* Flags. */
#define SA_RESTORER 0x04000000

/* Error codes. */
#define EINTR        4
#define O_NONBLOCK   04000

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

/* ---- Test state ---- */
static volatile int handler_count;

static void
handler_usr1(int sig)
{
	(void)sig;
	handler_count++;
}

/* Small delay for synchronization. */
static void
msleep(int ms)
{
	struct timespec ts = { 0, ms * 1000000L };
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/* Install SIGUSR1 handler (no SA_RESTART) */
static long
install_sigusr1(void)
{
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = handler_usr1;
	sa.flags = SA_RESTORER;
	sa.restorer = my_restorer;
	return sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
}

/*
 * ---- Test 1: Signal interrupts blocking pipe read ----
 * Block on empty pipe read, child sends SIGUSR1 -> -EINTR.
 */
static void
test_signal_interrupts_read(void)
{
	long r = install_sigusr1();
	check(r == 0, "intr-read: install handler");

	int fds[2];
	r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "intr-read: pipe2");

	handler_count = 0;
	long ppid = sys0(__NR_getpid);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "intr-read: fork");
		return;
	}
	if (pid == 0) {
		/* Child: close read end, sleep 50ms, signal parent, then exit. */
		sys1(__NR_close, fds[0]);
		msleep(50);
		sys2(__NR_kill, ppid, SIGUSR1);
		/* Keep write end open briefly so parent doesn't get EOF. */
		msleep(200);
		sys1(__NR_close, fds[1]);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: close write end, block on read (pipe empty -> blocks) */
	sys1(__NR_close, fds[1]);
	char buf[16];
	r = sys3(__NR_read, fds[0], (long)buf, sizeof(buf));
	check(r == -EINTR, "intr-read: read returns -EINTR");
	check(handler_count > 0, "intr-read: signal handler ran");

	/* Clean up. */
	sys1(__NR_close, fds[0]);
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
}

/*
 * ---- Test 2: Masked signal does not interrupt pipe read ----
 * Block SIGUSR1, block on pipe read, child sends SIGUSR1 then writes data.
 * Parent read should return the data (not -EINTR) since signal was masked.
 */
static void
test_signal_does_not_interrupt_with_mask(void)
{
	long r = install_sigusr1();
	check(r == 0, "masked: install handler");

	/* Block SIGUSR1. */
	unsigned long mask = SIGUSR1_MASK;
	r = sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&mask, 0, 8);
	check(r == 0, "masked: block SIGUSR1");

	int fds[2];
	r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "masked: pipe2");

	handler_count = 0;
	long ppid = sys0(__NR_getpid);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "masked: fork");
		return;
	}
	if (pid == 0) {
		/* Child: close read end, sleep 50ms, signal parent, write data. */
		sys1(__NR_close, fds[0]);
		msleep(50);
		sys2(__NR_kill, ppid, SIGUSR1);
		msleep(50);
		sys3(__NR_write, fds[1], (long)"hi", 2);
		sys1(__NR_close, fds[1]);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/*
	 * Parent: close write end, block on read
	 * SIGUSR1 is masked, so it stays pending and does NOT interrupt read.
	 * Read should eventually return data written by child.
	 */
	sys1(__NR_close, fds[1]);
	char buf[16];
	memset(buf, 0, sizeof(buf));
	r = sys3(__NR_read, fds[0], (long)buf, sizeof(buf));
	check(r == 2, "masked: read returns 2 bytes (not -EINTR)");
	check(buf[0] == 'h' && buf[1] == 'i', "masked: read got 'hi'");
	check(handler_count == 0, "masked: handler did not run while masked");

	/* Unblock SIGUSR1 -- pending signal should now be delivered. */
	r = sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&mask, 0, 8);
	check(r == 0, "masked: unblock SIGUSR1");
	check(handler_count == 1, "masked: handler ran after unblock");

	/* Clean up. */
	sys1(__NR_close, fds[0]);
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
}

/*
 * ---- Test 3: Multiple signals during blocked syscall ----
 * Block on pipe read, child sends 3 SIGUSR1 signals rapidly.
 * Parent should get -EINTR and handler_count should be >= 1.
 */
static void
test_multiple_signals_during_syscall(void)
{
	long r = install_sigusr1();
	check(r == 0, "multi-sig: install handler");

	int fds[2];
	r = sys2(__NR_pipe2, (long)fds, 0);
	check(r == 0, "multi-sig: pipe2");

	handler_count = 0;
	long ppid = sys0(__NR_getpid);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "multi-sig: fork");
		return;
	}
	if (pid == 0) {
		/* Child: close read end, sleep 50ms, then send 3 signals rapidly. */
		sys1(__NR_close, fds[0]);
		msleep(50);
		sys2(__NR_kill, ppid, SIGUSR1);
		sys2(__NR_kill, ppid, SIGUSR1);
		sys2(__NR_kill, ppid, SIGUSR1);
		/* Keep write end open briefly. */
		msleep(200);
		sys1(__NR_close, fds[1]);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: close write end, block on read. */
	sys1(__NR_close, fds[1]);
	char buf[16];
	r = sys3(__NR_read, fds[0], (long)buf, sizeof(buf));
	check(r == -EINTR, "multi-sig: read returns -EINTR");
	check(handler_count >= 1, "multi-sig: handler ran at least once");

	/* Clean up. */
	sys1(__NR_close, fds[0]);
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
}

int
main(void)
{
	msg("=== signal_cross tests ===\n");

	test_signal_interrupts_read();
	test_signal_does_not_interrupt_with_mask();
	test_multiple_signals_during_syscall();

	test_done();
	return 0;
}
