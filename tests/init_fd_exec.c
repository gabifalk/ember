/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * FD close-on-exec across exec tests: verifies that O_CLOEXEC file descriptors
 * are closed and non-CLOEXEC descriptors survive across execve().
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read            0
#define __NR_write           1
#define __NR_open            2
#define __NR_close           3
#define __NR_dup             32
#define __NR_dup2            33
#define __NR_fork            57
#define __NR_execve          59
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_fcntl           72
#define __NR_pipe2           293

/* Fcntl commands. */
#define F_GETFD  1
#define F_SETFD  2

/* Flags. */
#define FD_CLOEXEC   1
#define O_RDONLY     0
#define O_CLOEXEC    02000000

/* Raw syscall wrappers. */
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
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "S"(a2), "D"(a1):"rcx",
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
do_open(const char *path, long flags)
{
	return sys2(__NR_open, (long)path, flags);
}

static long
do_close(long fd)
{
	return sys1(__NR_close, fd);
}

static long
do_dup2(long oldfd, long newfd)
{
	return sys2(__NR_dup2, oldfd, newfd);
}

static long
do_fcntl(long fd, long cmd, long arg)
{
	return sys3(__NR_fcntl, fd, cmd, arg);
}

static long
do_pipe2(int pipefd[2], long flags)
{
	return sys2(__NR_pipe2, (long)pipefd, flags);
}

static long
do_fork(void)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(__NR_fork):"rcx", "r11",
			  "memory");
	return ret;
}

static void
do_exit(long code)
{
	sys1(__NR_exit, code);
	__builtin_unreachable();
}

static long
do_wait4(long pid, int *status, long options)
{
	return sys4(__NR_wait4, pid, (long)status, options, 0);
}

static long
do_execve(const char *path, char *const argv[], char *const envp[])
{
	return sys3(__NR_execve, (long)path, (long)argv, (long)envp);
}

/*
 * Test: O_CLOEXEC FDs are closed across exec, non-CLOEXEC FDs survive
 *
 * Setup:
 *   FD 3 = pipe read end, non-CLOEXEC  (should survive exec)
 *   FD 4 = pipe read end, CLOEXEC      (should be closed by exec)
 *   FD 5 = dup of pipe write end, non-CLOEXEC (should survive exec)
 *
 * fd_checker reports open FDs as bitmask in exit code:
 *   bit 0: FD 3 open, bit 1: FD 4 open, bit 2: FD 5 open
 * Expected: 0b101 = 5.
 */

static void
test_cloexec_across_exec(void)
{
	int pipe_a[2], pipe_b[2];

	/* Create two pipes to get FDs we can place where we want. */
	if (do_pipe2(pipe_a, 0) < 0) {
		check(0, "cloexec exec (pipe_a failed)");
		return;
	}
	if (do_pipe2(pipe_b, 0) < 0) {
		check(0, "cloexec exec (pipe_b failed)");
		do_close(pipe_a[0]);
		do_close(pipe_a[1]);
		return;
	}
	/*
	 * Place FDs at known positions:
	 * FD 3: pipe_a read end, non-CLOEXEC.
	 */
	long fd3 = do_dup2(pipe_a[0], 3);
	if (fd3 < 0) {
		check(0, "cloexec exec (dup2 fd3 failed)");
		return;
	}
	/* FD 4: pipe_b read end, then set CLOEXEC. */
	long fd4 = do_dup2(pipe_b[0], 4);
	if (fd4 < 0) {
		check(0, "cloexec exec (dup2 fd4 failed)");
		return;
	}
	do_fcntl(4, F_SETFD, FD_CLOEXEC);

	/* FD 5: dup of pipe_a write end, non-CLOEXEC. */
	long fd5 = do_dup2(pipe_a[1], 5);
	if (fd5 < 0) {
		check(0, "cloexec exec (dup2 fd5 failed)");
		return;
	}
	/*
	 * Close original pipe FDs only if they don't overlap with our target FDs 3-5
	 * (pipe returns lowest available FDs, so pipe_a may be {3,4} and pipe_b {5,6})
	 */
	int origfds[] = { pipe_a[0], pipe_a[1], pipe_b[0], pipe_b[1] };
	for (int i = 0; i < 4; i++) {
		if (origfds[i] != 3 && origfds[i] != 4 && origfds[i] != 5)
			do_close(origfds[i]);
	}

	/* Verify our setup before exec. */
	long flags3 = do_fcntl(3, F_GETFD, 0);
	long flags4 = do_fcntl(4, F_GETFD, 0);
	long flags5 = do_fcntl(5, F_GETFD, 0);
	check(flags3 >= 0, "FD 3 open before exec");
	check(flags4 >= 0 && (flags4 & FD_CLOEXEC), "FD 4 CLOEXEC before exec");
	check(flags5 >= 0, "FD 5 open before exec");

	/* Fork + exec fd_checker. */
	long pid = do_fork();
	if (pid < 0) {
		check(0, "cloexec exec (fork failed)");
		return;
	}
	if (pid == 0) {
		/* Child: exec fd_checker. */
		char *argv[] = { "/fd_checker", (char *)0 };
		char *envp[] = { (char *)0 };
		do_execve("/fd_checker", argv, envp);
		/* If exec fails, exit with 0xFF to signal failure. */
		do_exit(0xFF);
	}
	/* Parent: wait for child and check exit code. */
	int status = 0;
	do_wait4(pid, &status, 0);
	int code = (status >> 8) & 0xff;

	if (code == 0xFF) {
		check(0, "fd_checker exec failed");
		return;
	}

	check((code & 1) != 0, "FD 3 (non-CLOEXEC) survived exec");
	check((code & 2) == 0, "FD 4 (CLOEXEC) closed by exec");
	check((code & 4) != 0, "FD 5 (non-CLOEXEC dup) survived exec");

	/* Clean up parent FDs. */
	do_close(3);
	do_close(4);		/* May already be closed if dup2 didn't reopen. */
	do_close(5);
}

int
main(void)
{
	msg("=== fd_exec tests ===\n");
	test_cloexec_across_exec();
	test_done();
	return 0;
}
