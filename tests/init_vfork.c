/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * vfork tests -- verifies vfork blocks parent until child exits or execs.
 */

#include "test_common.h"

#define __NR_write    1
#define __NR_vfork   58
#define __NR_execve  59
#define __NR_exit    60
#define __NR_wait4   61

static long
sys1(long nr, long a1)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1):"rcx", "r11",
			  "memory");
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

/*
 * Direct inline vfork + exit(code). Returns child pid to parent.
 * Uses inline asm to avoid C stack corruption from shared address space.
 */
static long
vfork_and_exit(long exit_code)
{
	long pid;
	register long r_code __asm__("rbx") = exit_code;
	__asm__ volatile ("mov $58, %%rax\n\t"	/* __NR_vfork. */
			  "syscall\n\t" "test %%rax, %%rax\n\t" "jnz 1f\n\t"
			  /* Child: call _exit(exit_code) */
			  "mov %%rbx, %%rdi\n\t" "mov $60, %%rax\n\t"	/* __NR_exit. */
			  "syscall\n\t" "1:\n\t":"=a" (pid)
			  :"r"(r_code)
			  :"rcx", "r11", "rdi", "rsi", "rdx", "memory");
	return pid;
}

/* Direct inline vfork + execve. Returns child pid to parent. */
static long
vfork_and_exec(const char *path, const char **argv, const char **envp)
{
	long pid;
	/* Use callee-saved registers so they survive the vfork syscall. */
	register long r_path __asm__("r12") = (long)path;
	register long r_argv __asm__("r13") = (long)argv;
	register long r_envp __asm__("r14") = (long)envp;
	__asm__ volatile ("mov $58, %%rax\n\t"	/* __NR_vfork. */
			  "syscall\n\t" "test %%rax, %%rax\n\t" "jnz 1f\n\t"
			  /* Child: execve(path, argv, envp) */
			  "mov %%r12, %%rdi\n\t" "mov %%r13, %%rsi\n\t" "mov %%r14, %%rdx\n\t" "mov $59, %%rax\n\t"	/* __NR_execve. */
			  "syscall\n\t"
			  /* If execve fails, _exit(99) */
			  "mov $99, %%rdi\n\t"
			  "mov $60, %%rax\n\t" "syscall\n\t" "1:\n\t":"=a" (pid)
			  :"r"(r_path), "r"(r_argv), "r"(r_envp)
			  :"rcx", "r11", "rdi", "rsi", "rdx", "memory");
	return pid;
}

/* Test 1: vfork + _exit. */
static void
test_vfork_exit(void)
{
	long pid = vfork_and_exit(42);
	check(pid > 0, "vfork_exit: vfork returned child pid");

	int status = 0;
	long rpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(rpid == pid, "vfork_exit: wait4 returned correct pid");

	int code = (status >> 8) & 0xff;
	check(code == 42, "vfork_exit: child exit status is 42");
}

/* Test 2: vfork + execve. */
static void
test_vfork_exec(void)
{
	const char *argv[] = { "/hello", (void *)0 };
	const char *envp[] = { (void *)0 };

	long pid = vfork_and_exec("/hello", argv, envp);
	check(pid > 0, "vfork_exec: vfork returned child pid");

	int status = 0;
	long rpid = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(rpid == pid, "vfork_exec: wait4 returned correct pid");

	int code = (status >> 8) & 0xff;
	check(code == 0, "vfork_exec: child exited successfully after exec");
}

/* Test 3: Multiple sequential vforks. */
static void
test_multi_vfork(void)
{
	long pid1 = vfork_and_exit(10);
	check(pid1 > 0, "multi_vfork: first vfork returned child pid");
	int status1 = 0;
	long rpid1 = sys4(__NR_wait4, pid1, (long)&status1, 0, 0);
	check(rpid1 == pid1, "multi_vfork: first wait4 returned correct pid");
	check(((status1 >> 8) & 0xff) == 10,
	      "multi_vfork: first child exit status is 10");

	long pid2 = vfork_and_exit(20);
	check(pid2 > 0, "multi_vfork: second vfork returned child pid");
	int status2 = 0;
	long rpid2 = sys4(__NR_wait4, pid2, (long)&status2, 0, 0);
	check(rpid2 == pid2, "multi_vfork: second wait4 returned correct pid");
	check(((status2 >> 8) & 0xff) == 20,
	      "multi_vfork: second child exit status is 20");
}

int
main(void)
{
	msg("=== vfork tests ===\n");

	test_vfork_exit();
	test_vfork_exec();
	test_multi_vfork();

	test_done();
	return 0;
}
