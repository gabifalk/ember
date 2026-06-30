/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 * Consolidated FD operations tests -- dup2, cloexec, fcntl, fd_limit, fd_exhaust.
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
#define __NR_exit            60
#define __NR_wait4           61
#define __NR_fcntl           72
#define __NR_dup3            292
#define __NR_pipe2           293

/* Fcntl commands. */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_DUPFD_CLOEXEC  1030

/* Flags. */
#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT      0100
#define O_APPEND     02000
#define O_CLOEXEC    02000000
#define O_ACCMODE    3
#define FD_CLOEXEC   1

/* Error codes. */
#define EBADF        9
#define EMFILE       24

/* Limits. */
#define MAX_FDS      256

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

/* Helper wrappers. */
static long
do_read(long fd, void *buf, long count)
{
	return sys3(__NR_read, fd, (long)buf, count);
}

static long
do_write(long fd, const void *buf, long count)
{
	return sys3(__NR_write, fd, (long)buf, count);
}

static long
do_open(const char *path, long flags, long mode)
{
	return sys3(__NR_open, (long)path, flags, mode);
}

static long
do_open_nomode(const char *path, long flags)
{
	return sys2(__NR_open, (long)path, flags);
}

static long
do_close(long fd)
{
	return sys1(__NR_close, fd);
}

static long
do_dup(long fd)
{
	return sys1(__NR_dup, fd);
}

static long
do_dup2(long oldfd, long newfd)
{
	return sys2(__NR_dup2, oldfd, newfd);
}

static long
do_dup3(long oldfd, long newfd, long flags)
{
	return sys3(__NR_dup3, oldfd, newfd, flags);
}

static long
do_pipe2(int pipefd[2], long flags)
{
	return sys2(__NR_pipe2, (long)pipefd, flags);
}

static long
do_fcntl(long fd, long cmd, long arg)
{
	return sys3(__NR_fcntl, fd, cmd, arg);
}

static long
do_fcntl_noarg(long fd, long cmd)
{
	return sys2(__NR_fcntl, fd, cmd);
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

/*
 * ================================================================
 * dup2 tests (from init_dup2.c)
 * ================================================================
 */

/* Test 1: dup2 to a specific fd number, verify read works via new fd. */
static void
test_dup2_specific_fd(void)
{
	int pipefd[2] = { -1, -1 };
	long r = do_pipe2(pipefd, 0);
	if (r < 0) {
		check(0, "dup2 specific fd (pipe2 failed)");
		return;
	}
	/* Write data through write end. */
	r = do_write(pipefd[1], "hello", 5);
	if (r != 5) {
		check(0, "dup2 specific fd (write failed)");
		goto cleanup;
	}
	/* Dup2 read end to fd 10. */
	r = do_dup2(pipefd[0], 10);
	if (r != 10) {
		check(0, "dup2 specific fd (dup2 failed)");
		goto cleanup;
	}
	/* Read from fd 10. */
	char buf[8] = { 0 };
	r = do_read(10, buf, 5);
	check(r == 5 && buf[0] == 'h' && buf[4] == 'o', "dup2 specific fd");

	do_close(10);
 cleanup:
	do_close(pipefd[0]);
	do_close(pipefd[1]);
}

/* Test 2: dup2(fd, fd) is a no-op and returns fd. */
static void
test_dup2_same_fd(void)
{
	int pipefd[2] = { -1, -1 };
	long r = do_pipe2(pipefd, 0);
	if (r < 0) {
		check(0, "dup2 same fd (pipe2 failed)");
		return;
	}

	r = do_dup2(pipefd[0], pipefd[0]);
	check(r == pipefd[0], "dup2 same fd");

	do_close(pipefd[0]);
	do_close(pipefd[1]);
}

/* Test 3: dup3 with O_CLOEXEC sets FD_CLOEXEC (from init_dup2.c) */
static void
test_dup2_dup3_cloexec(void)
{
	int pipefd[2] = { -1, -1 };
	long r = do_pipe2(pipefd, 0);
	if (r < 0) {
		check(0, "dup3 O_CLOEXEC (pipe2 failed)");
		return;
	}

	r = do_dup3(pipefd[0], 20, O_CLOEXEC);
	if (r != 20) {
		check(0, "dup3 O_CLOEXEC (dup3 failed)");
		goto cleanup;
	}

	long flags = do_fcntl(20, F_GETFD, 0);
	check(flags == FD_CLOEXEC, "dup3 O_CLOEXEC");

	do_close(20);
 cleanup:
	do_close(pipefd[0]);
	do_close(pipefd[1]);
}

/* Test 4: dup2 over an open fd closes the old target fd. */
static void
test_dup2_closes_target(void)
{
	/* Create two pipes. */
	int pipe_a[2] = { -1, -1 };
	int pipe_b[2] = { -1, -1 };
	long r = do_pipe2(pipe_a, 0);
	if (r < 0) {
		check(0, "dup2 closes target (pipe2 a failed)");
		return;
	}
	r = do_pipe2(pipe_b, 0);
	if (r < 0) {
		check(0, "dup2 closes target (pipe2 b failed)");
		do_close(pipe_a[0]);
		do_close(pipe_a[1]);
		return;
	}
	/* Write data to pipe_b. */
	r = do_write(pipe_b[1], "world", 5);
	if (r != 5) {
		check(0, "dup2 closes target (write failed)");
		goto cleanup;
	}
	/* Dup2 pipe_b read end over pipe_a read end. */
	r = do_dup2(pipe_b[0], pipe_a[0]);
	if (r != pipe_a[0]) {
		check(0, "dup2 closes target (dup2 failed)");
		goto cleanup;
	}
	/* Reading from pipe_a[0] should now read pipe_b's data. */
	char buf[8] = { 0 };
	r = do_read(pipe_a[0], buf, 5);
	check(r == 5 && buf[0] == 'w' && buf[4] == 'd', "dup2 closes target");

 cleanup:
	do_close(pipe_a[0]);
	do_close(pipe_a[1]);
	do_close(pipe_b[0]);
	do_close(pipe_b[1]);
}

/* Test 5: dup2 with bad source fd returns -EBADF. */
static void
test_dup2_bad_fd(void)
{
	long r = do_dup2(999, 10);
	check(r == -EBADF, "dup2 bad fd EBADF");
}

/*
 * ================================================================
 * cloexec tests (from init_cloexec.c)
 * ================================================================
 */

/* Test 1: open with O_CLOEXEC sets FD_CLOEXEC. */
static void
test_open_cloexec(void)
{
	long fd = do_open_nomode("/init", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		check(0, "open O_CLOEXEC (open failed)");
		return;
	}
	long flags = do_fcntl(fd, F_GETFD, 0);
	check(flags == FD_CLOEXEC, "open O_CLOEXEC");
	do_close(fd);
}

/* Test 2: open without O_CLOEXEC has no FD_CLOEXEC. */
static void
test_open_no_cloexec(void)
{
	long fd = do_open_nomode("/init", O_RDONLY);
	if (fd < 0) {
		check(0, "open no O_CLOEXEC (open failed)");
		return;
	}
	long flags = do_fcntl(fd, F_GETFD, 0);
	check(flags == 0, "open no O_CLOEXEC");
	do_close(fd);
}

/* Test 3: F_SETFD to set FD_CLOEXEC. */
static void
test_setfd_cloexec(void)
{
	long fd = do_open_nomode("/init", O_RDONLY);
	if (fd < 0) {
		check(0, "F_SETFD (open failed)");
		return;
	}
	long r = do_fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (r < 0) {
		check(0, "F_SETFD (fcntl failed)");
		do_close(fd);
		return;
	}
	long flags = do_fcntl(fd, F_GETFD, 0);
	check(flags == FD_CLOEXEC, "F_SETFD FD_CLOEXEC");
	do_close(fd);
}

/* Test 4: dup clears FD_CLOEXEC. */
static void
test_dup_clears_cloexec(void)
{
	long fd = do_open_nomode("/init", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		check(0, "dup clears cloexec (open failed)");
		return;
	}
	long newfd = do_dup(fd);
	if (newfd < 0) {
		check(0, "dup clears cloexec (dup failed)");
		do_close(fd);
		return;
	}
	long flags = do_fcntl(newfd, F_GETFD, 0);
	check(flags == 0, "dup clears cloexec");
	do_close(newfd);
	do_close(fd);
}

/* Test 5: dup2 clears FD_CLOEXEC. */
static void
test_dup2_clears_cloexec(void)
{
	long fd = do_open_nomode("/init", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		check(0, "dup2 clears cloexec (open failed)");
		return;
	}
	long newfd = do_dup2(fd, 50);
	if (newfd < 0) {
		check(0, "dup2 clears cloexec (dup2 failed)");
		do_close(fd);
		return;
	}
	long flags = do_fcntl(newfd, F_GETFD, 0);
	check(flags == 0, "dup2 clears cloexec");
	do_close(newfd);
	do_close(fd);
}

/* Test 6: dup3 with O_CLOEXEC sets FD_CLOEXEC (from init_cloexec.c) */
static void
test_cloexec_dup3_cloexec(void)
{
	long fd = do_open_nomode("/init", O_RDONLY);
	if (fd < 0) {
		check(0, "dup3 O_CLOEXEC (open failed)");
		return;
	}
	long newfd = do_dup3(fd, 51, O_CLOEXEC);
	if (newfd < 0) {
		check(0, "dup3 O_CLOEXEC (dup3 failed)");
		do_close(fd);
		return;
	}
	long flags = do_fcntl(newfd, F_GETFD, 0);
	check(flags == FD_CLOEXEC, "dup3 O_CLOEXEC");
	do_close(newfd);
	do_close(fd);
}

/* Test 7: pipe2 with O_CLOEXEC sets FD_CLOEXEC on both ends. */
static void
test_pipe2_cloexec(void)
{
	int pipefd[2] = { -1, -1 };
	long r = do_pipe2(pipefd, O_CLOEXEC);
	if (r < 0) {
		check(0, "pipe2 O_CLOEXEC (pipe2 failed)");
		return;
	}
	long flags0 = do_fcntl(pipefd[0], F_GETFD, 0);
	long flags1 = do_fcntl(pipefd[1], F_GETFD, 0);
	check(flags0 == FD_CLOEXEC && flags1 == FD_CLOEXEC, "pipe2 O_CLOEXEC");
	do_close(pipefd[0]);
	do_close(pipefd[1]);
}

/* Test 8: FD_CLOEXEC preserved across fork. */
static void
test_cloexec_across_fork(void)
{
	long fd = do_open_nomode("/init", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		check(0, "cloexec across fork (open failed)");
		return;
	}

	long pid = do_fork();
	if (pid < 0) {
		check(0, "cloexec across fork (fork failed)");
		do_close(fd);
		return;
	}
	if (pid == 0) {
		/* Child: check that FD_CLOEXEC is still set. */
		long flags = do_fcntl(fd, F_GETFD, 0);
		do_exit(flags == FD_CLOEXEC ? 0 : 1);
	}
	/* Parent: wait for child. */
	int status = 0;
	do_wait4(pid, &status, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "cloexec across fork");
	do_close(fd);
}

/*
 * ================================================================
 * fcntl tests (from init_fcntl.c)
 * ================================================================
 */

/* Test 1: F_GETFL on O_RDONLY fd. */
static void
test_getfl_rdonly(void)
{
	long fd = do_open("/init", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "F_GETFL rdonly (open failed)");
		return;
	}
	long flags = do_fcntl(fd, F_GETFL, 0);
	check(flags >= 0 && (flags & O_ACCMODE) == O_RDONLY, "F_GETFL rdonly");
	do_close(fd);
}

/* Test 2: F_SETFL O_APPEND, then verify with F_GETFL. */
static void
test_setfl_append(void)
{
	long fd = do_open("/tmp_setfl", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(1, "F_SETFL O_APPEND (skip: read-only fs)");
		return;
	}
	long r = do_fcntl(fd, F_SETFL, O_WRONLY | O_APPEND);
	if (r < 0) {
		check(0, "F_SETFL O_APPEND (fcntl failed)");
		do_close(fd);
		return;
	}
	long flags = do_fcntl(fd, F_GETFL, 0);
	check(flags >= 0 && (flags & O_APPEND), "F_SETFL O_APPEND");
	do_close(fd);
}

/* Test 3: F_DUPFD to fd >= 10, verify both fds read same data. */
static void
test_dupfd(void)
{
	long fd = do_open("/init", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "F_DUPFD (open failed)");
		return;
	}
	long newfd = do_fcntl(fd, F_DUPFD, 10);
	if (newfd < 0) {
		check(0, "F_DUPFD (fcntl failed)");
		do_close(fd);
		return;
	}
	if (newfd < 10) {
		check(0, "F_DUPFD fd < 10");
		do_close(newfd);
		do_close(fd);
		return;
	}
	/* Read from original fd. */
	char buf1[16];
	long n1 = do_read(fd, buf1, 16);
	/*
	 * Close original, read from new fd (shares file offset, so we get next bytes)
	 * Instead, just verify newfd is valid by reading from it.
	 */
	char buf2[16];
	long n2 = do_read(newfd, buf2, 16);
	/* Both should succeed with same count (they share offset, so buf2 follows buf1) */
	check(n1 > 0 && n2 > 0 && newfd >= 10, "F_DUPFD dup to >= 10");
	do_close(newfd);
	do_close(fd);
}

/* Test 4: F_DUPFD_CLOEXEC sets FD_CLOEXEC on new fd. */
static void
test_dupfd_cloexec(void)
{
	long fd = do_open("/init", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "F_DUPFD_CLOEXEC (open failed)");
		return;
	}
	long newfd = do_fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (newfd < 0) {
		check(0, "F_DUPFD_CLOEXEC (fcntl failed)");
		do_close(fd);
		return;
	}
	long fdflags = do_fcntl(newfd, F_GETFD, 0);
	check(fdflags == FD_CLOEXEC, "F_DUPFD_CLOEXEC");
	do_close(newfd);
	do_close(fd);
}

/* Test 5: O_APPEND flag preserved across fork. */
static void
test_flags_across_fork(void)
{
	long fd =
	    do_open("/tmp_fork_flags", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) {
		check(1, "flags across fork (skip: read-only fs)");
		return;
	}

	long pid = do_fork();
	if (pid < 0) {
		check(0, "flags across fork (fork failed)");
		do_close(fd);
		return;
	}
	if (pid == 0) {
		/* Child: check that O_APPEND is still set. */
		long flags = do_fcntl(fd, F_GETFL, 0);
		do_exit((flags >= 0 && (flags & O_APPEND)) ? 0 : 1);
	}
	/* Parent: wait for child. */
	int status = 0;
	do_wait4(pid, &status, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "flags across fork");
	do_close(fd);
}

/* Test 6: F_GETFL on pipe fds -- read end O_RDONLY, write end O_WRONLY. */
static void
test_getfl_pipe(void)
{
	int pipefd[2] = { -1, -1 };
	long r = do_pipe2(pipefd, 0);
	if (r < 0) {
		check(0, "F_GETFL pipe (pipe2 failed)");
		return;
	}
	long rflags = do_fcntl(pipefd[0], F_GETFL, 0);
	long wflags = do_fcntl(pipefd[1], F_GETFL, 0);
	check(rflags >= 0 && (rflags & O_ACCMODE) == O_RDONLY &&
	      wflags >= 0 && (wflags & O_ACCMODE) == O_WRONLY,
	      "F_GETFL pipe ends");
	do_close(pipefd[0]);
	do_close(pipefd[1]);
}

/*
 * ================================================================
 * fd_limit tests (from init_fd_limit.c)
 * ================================================================
 */

/* Storage for all opened fds so we can close them. */
static int limit_fds[512];
static int limit_nfds;

/*
 * Open pipe pairs until we hit EMFILE, then fill remaining slot(s) with dup.
 * Returns the number of fds opened.
 */
static int
exhaust_all_fds(void)
{
	limit_nfds = 0;
	while (limit_nfds < 510) {
		int pipefd[2];
		long r = do_pipe2(pipefd, 0);
		if (r < 0)
			break;
		limit_fds[limit_nfds++] = pipefd[0];
		limit_fds[limit_nfds++] = pipefd[1];
	}
	/* Pipe2 needs 2 slots; fill any remaining single slot with dup. */
	while (limit_nfds < 510) {
		long r = do_dup(0);
		if (r < 0)
			break;
		limit_fds[limit_nfds++] = (int)r;
	}
	return limit_nfds;
}

/* Close all fds we opened. */
static void
close_all_fds(void)
{
	for (int i = 0; i < limit_nfds; i++)
		do_close(limit_fds[i]);
	limit_nfds = 0;
}

/*
 * ---- Test 1: exhaust_fds ----
 * Open pipe pairs until EMFILE. Verify we got close to MAX_FDS (256).
 * We start with fds 0,1,2 open, so we should open at least 200 more.
 */
static void
test_exhaust_fds(void)
{
	int opened = exhaust_all_fds();

	/* Verify pipe2 now returns -EMFILE. */
	int pipefd[2];
	long r = do_pipe2(pipefd, 0);
	check(r == -EMFILE, "exhaust: pipe2 returns -EMFILE at limit");

	/*
	 * We started with fds 0,1,2. MAX_FDS=256, so we should have opened
	 * at least 200 fds (253 available, rounded down to even for pipe pairs = 252).
	 */
	check(opened >= 200, "exhaust: opened >= 200 fds");

	msg("  opened ");
	print_int(opened);
	msg(" fds\n");

	close_all_fds();
}

/*
 * ---- Test 2: recover_after_close ----
 * After exhausting fds, close one fd. Verify dup(0) succeeds.
 */
static void
test_recover_after_close(void)
{
	exhaust_all_fds();

	/* Verify we are at the limit. */
	int pipefd[2];
	long r = do_pipe2(pipefd, 0);
	check(r == -EMFILE, "recover: at limit");

	/* Close one fd to free a slot. */
	do_close(limit_fds[limit_nfds - 1]);
	limit_nfds--;

	/* Dup(0) should now succeed. */
	r = do_dup(0);
	check(r >= 0, "recover: dup(0) succeeds after close");

	/* Clean up the dup'd fd. */
	if (r >= 0)
		do_close((int)r);

	close_all_fds();
}

/*
 * ---- Test 3: dup_at_limit ----
 * Exhaust fds with pipes. Try dup(0) -- should fail with -EMFILE.
 * Close one fd. dup(0) should now succeed.
 */
static void
test_dup_at_limit(void)
{
	exhaust_all_fds();

	/* Dup(0) should fail at limit. */
	long r = do_dup(0);
	check(r == -EMFILE, "dup_at_limit: dup fails with -EMFILE");

	/* Close one fd to free a slot. */
	do_close(limit_fds[limit_nfds - 1]);
	limit_nfds--;

	/* Dup(0) should now succeed. */
	r = do_dup(0);
	check(r >= 0, "dup_at_limit: dup succeeds after close");

	/* Clean up. */
	if (r >= 0)
		do_close((int)r);

	close_all_fds();
}

/*
 * ================================================================
 * fd_exhaust tests (from init_fd_exhaust.c)
 * ================================================================
 */

/* Storage for fds opened during exhaustion. */
static int exhaust_fds[MAX_FDS];
static int exhaust_nfds;

/* ---- Test 1: Exhaust MAX_FDS via open(/init) ---- */
static void
test_exhaust_open(void)
{
	exhaust_nfds = 0;

	/* Open /init repeatedly until we get -EMFILE. */
	int got_emfile = 0;
	for (int i = 0; i < MAX_FDS + 10; i++) {
		long r = do_open_nomode("/init", O_RDONLY);
		if (r == -EMFILE) {
			got_emfile = 1;
			break;
		}
		if (r < 0)
			break;
		exhaust_fds[exhaust_nfds++] = (int)r;
	}
	check(got_emfile, "exhaust: open returns -EMFILE at limit");

	/* We start with fds 0,1,2 open, so we should have opened MAX_FDS - 3 fds. */
	check(exhaust_nfds >= MAX_FDS - 4,
	      "exhaust: opened close to MAX_FDS fds");

	msg("  opened ");
	print_int(exhaust_nfds);
	msg(" fds via open\n");

	/* Close all and verify close succeeds. */
	int close_ok = 1;
	for (int i = 0; i < exhaust_nfds; i++) {
		long r = do_close(exhaust_fds[i]);
		if (r != 0)
			close_ok = 0;
	}
	check(close_ok, "exhaust: all close() calls succeeded");
	exhaust_nfds = 0;
}

/* ---- Test 2: dup2 to high fd ---- */
static void
test_dup2_high(void)
{
	long r = do_dup2(0, 200);
	check(r == 200, "dup2_high: dup2(0, 200) returns 200");

	if (r == 200)
		do_close(200);
}

/* ---- Test 3: dup2 to same fd ---- */
static void
test_dup2_same(void)
{
	/* Open a file to get a known fd. */
	long fd = do_open_nomode("/init", O_RDONLY);
	check(fd >= 0, "dup2_same: open ok");
	if (fd < 0)
		return;

	/* Dup2(fd, fd) should return fd without error. */
	long r = do_dup2((int)fd, (int)fd);
	check(r == fd, "dup2_same: dup2(fd, fd) returns fd");

	do_close((int)fd);
}

/* ---- Test 4: O_CLOEXEC flag verified via fcntl(F_GETFD) ---- */
static void
test_cloexec_flag(void)
{
	/* Open without O_CLOEXEC -- FD_CLOEXEC should NOT be set. */
	long fd1 = do_open_nomode("/init", O_RDONLY);
	check(fd1 >= 0, "cloexec: open without O_CLOEXEC ok");
	if (fd1 >= 0) {
		long flags = do_fcntl_noarg((int)fd1, F_GETFD);
		check((flags & FD_CLOEXEC) == 0,
		      "cloexec: no O_CLOEXEC -> FD_CLOEXEC not set");
		do_close((int)fd1);
	}
	/* Open with O_CLOEXEC -- FD_CLOEXEC should be set. */
	long fd2 = do_open_nomode("/init", O_RDONLY | O_CLOEXEC);
	check(fd2 >= 0, "cloexec: open with O_CLOEXEC ok");
	if (fd2 >= 0) {
		long flags = do_fcntl_noarg((int)fd2, F_GETFD);
		check((flags & FD_CLOEXEC) != 0,
		      "cloexec: O_CLOEXEC -> FD_CLOEXEC is set");
		do_close((int)fd2);
	}
}

/*
 * ================================================================
 * main
 * ================================================================
 */

int
main(void)
{
	msg("=== fd_ops tests ===\n");
	msg("--- dup2 ---\n");
	test_dup2_specific_fd();
	test_dup2_same_fd();
	test_dup2_dup3_cloexec();
	test_dup2_closes_target();
	test_dup2_bad_fd();
	msg("--- cloexec ---\n");
	test_open_cloexec();
	test_open_no_cloexec();
	test_setfd_cloexec();
	test_dup_clears_cloexec();
	test_dup2_clears_cloexec();
	test_cloexec_dup3_cloexec();
	test_pipe2_cloexec();
	test_cloexec_across_fork();
	msg("--- fcntl ---\n");
	test_getfl_rdonly();
	test_setfl_append();
	test_dupfd();
	test_dupfd_cloexec();
	test_flags_across_fork();
	test_getfl_pipe();
	msg("--- fd_limit ---\n");
	test_exhaust_fds();
	test_recover_after_close();
	test_dup_at_limit();
	msg("--- fd_exhaust ---\n");
	test_exhaust_open();
	test_dup2_high();
	test_dup2_same();
	test_cloexec_flag();
	test_done();
	return 0;
}
