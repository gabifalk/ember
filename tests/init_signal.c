/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Signal handler return test -- exercises signal delivery, SA_RESTORER,
 * signal masking/pending, SIG_IGN, and cross-process signal delivery.
 */

#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_rt_sigaction   13
#define __NR_rt_sigprocmask 14
#define __NR_rt_sigreturn   15
#define __NR_rt_sigpending  127
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62
#define __NR_nanosleep      35

/* Signal numbers. */
#define SIGUSR1 10
#define SIGUSR2 12

/* SA_RESTORER flag. */
#define SA_RESTORER 0x04000000

/* SIG_IGN. */
#define SIG_IGN ((void (*)(int))1)

/* Sigprocmask how values. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

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

static volatile int usr1_count;
static volatile int usr2_count;
static volatile int restorer_handler_ran;

static void
handler_usr1(int sig)
{
	(void)sig;
	usr1_count++;
}

static void
handler_usr2(int sig)
{
	(void)sig;
	usr2_count++;
}

static void
handler_with_restorer(int sig)
{
	(void)sig;
	restorer_handler_ran = 1;
}

/* Small delay to let signals propagate. */
static void
tiny_sleep(void)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	0, 5000000};		/* 5Ms. */
	sys2(__NR_nanosleep, (long)&ts, 0);
}

/* ---- Test 1: Basic signal delivery ---- */
static void
test_basic_delivery(void)
{
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = handler_usr1;
	sa.flags = 0;

	long r = sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
	check(r == 0, "basic: rt_sigaction install");

	usr1_count = 0;
	long pid = sys0(__NR_getpid);
	r = sys2(__NR_kill, pid, SIGUSR1);
	check(r == 0, "basic: kill ret");
	check(usr1_count == 1, "basic: handler ran");
}

/* ---- Test 2: SA_RESTORER ---- */
static void
test_sa_restorer(void)
{
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = handler_with_restorer;
	sa.flags = SA_RESTORER;
	sa.restorer = my_restorer;

	long r = sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
	check(r == 0, "restorer: rt_sigaction install");

	restorer_handler_ran = 0;
	long pid = sys0(__NR_getpid);
	r = sys2(__NR_kill, pid, SIGUSR1);
	check(r == 0, "restorer: kill ret");
	check(restorer_handler_ran == 1, "restorer: handler ran and returned");
}

/* ---- Test 3: Signal masking ---- */
static void
test_signal_masking(void)
{
	/* Install handler for SIGUSR2. */
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = handler_usr2;
	sa.flags = 0;
	sys4(__NR_rt_sigaction, SIGUSR2, (long)&sa, 0, 8);

	/* Block SIGUSR2. */
	unsigned long mask = (1UL << SIGUSR2);
	unsigned long old_mask = 0;
	long r =
	    sys4(__NR_rt_sigprocmask, SIG_BLOCK, (long)&mask, (long)&old_mask,
		 8);
	check(r == 0, "mask: sigprocmask block");

	/* Send SIGUSR2 while blocked. */
	usr2_count = 0;
	long pid = sys0(__NR_getpid);
	r = sys2(__NR_kill, pid, SIGUSR2);
	check(r == 0, "mask: kill while blocked");
	check(usr2_count == 0, "mask: handler did not run while blocked");

	/* Check it's pending. */
	unsigned long pending = 0;
	r = sys2(__NR_rt_sigpending, (long)&pending, 8);
	check(r == 0, "mask: rt_sigpending ret");
	check((pending & mask) != 0, "mask: signal is pending");

	/* Unblock SIGUSR2 -- pending signal should be delivered. */
	r = sys4(__NR_rt_sigprocmask, SIG_UNBLOCK, (long)&mask, 0, 8);
	check(r == 0, "mask: sigprocmask unblock");
	check(usr2_count == 1, "mask: handler ran after unblock");
}

/* ---- Test 4: SIG_IGN ---- */
static void
test_sig_ign(void)
{
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = SIG_IGN;
	sa.flags = 0;

	long r = sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);
	check(r == 0, "ign: rt_sigaction SIG_IGN");

	usr1_count = 0;
	long pid = sys0(__NR_getpid);
	r = sys2(__NR_kill, pid, SIGUSR1);
	check(r == 0, "ign: kill ret");
	check(usr1_count == 0, "ign: handler did not run");
}

/* ---- Test 5: Child sends signal to parent ---- */
static void
test_child_signal(void)
{
	/* Re-install real handler for SIGUSR1. */
	struct kernel_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = handler_usr1;
	sa.flags = 0;
	sys4(__NR_rt_sigaction, SIGUSR1, (long)&sa, 0, 8);

	usr1_count = 0;
	long parent_pid = sys0(__NR_getpid);

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "child: fork");
		return;
	}
	if (pid == 0) {
		/* Child: send SIGUSR1 to parent, then exit. */
		tiny_sleep();	/* Small delay so parent is ready. */
		sys2(__NR_kill, parent_pid, SIGUSR1);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}
	/* Parent: wait for child. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "child: exit status");
	check(usr1_count == 1, "child: parent received signal");
}

int
main(void)
{
	msg("=== signal tests ===\n");

	test_basic_delivery();
	test_sa_restorer();
	test_signal_masking();
	test_sig_ign();
	test_child_signal();

	test_done();
	return 0;
}
