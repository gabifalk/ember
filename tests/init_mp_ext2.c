/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Multiprocess concurrent ext2 tests -- verifies that concurrent file
 * operations from multiple processes do not corrupt data.
 */

#include "test_common.h"

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
sys3(long nr, long a1, long a2, long a3)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2), "d"(a3)
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

#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_OPEN   2
#define SYS_CLOSE  3
#define SYS_LSEEK  8
#define SYS_FORK   57
#define SYS_EXIT   60
#define SYS_WAIT4  61

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define O_APPEND   0x400

#define SEEK_SET   0

static long
do_open(const char *path, long flags, long mode)
{
	return sys3(SYS_OPEN, (long)path, flags, mode);
}

static long
do_close(long fd)
{
	return sys1(SYS_CLOSE, fd);
}

static long
do_read(long fd, void *buf, long count)
{
	return sys3(SYS_READ, fd, (long)buf, count);
}

static long
do_write(long fd, const void *buf, long count)
{
	return sys3(SYS_WRITE, fd, (long)buf, count);
}

static long
do_fork(void)
{
	return sys0(SYS_FORK);
}

static void
do_exit(long code)
{
	sys1(SYS_EXIT, code);
	__builtin_unreachable();
}

static long
do_wait(long pid, int *status)
{
	return sys4(SYS_WAIT4, pid, (long)status, 0, 0);
}

static void
memfill(char *buf, char c, long n)
{
	for (long i = 0; i < n; i++)
		buf[i] = c;
}

static int
memchk(const char *buf, char c, long n)
{
	for (long i = 0; i < n; i++)
		if (buf[i] != c)
			return 0;
	return 1;
}

/*
 * -----------------------------------------------------------------------
 * Test 1: Two children create separate files concurrently
 *
 * Child A creates /fileA (1024 bytes of 'A'), child B creates /fileB
 * (1024 bytes of 'B'). Parent waits for both, reads back both files,
 * and verifies contents are correct.
 * -----------------------------------------------------------------------
 */
static void
test_separate_files(void)
{
	char buf[1024];
	long pid_a, pid_b;

	pid_a = do_fork();
	check(pid_a >= 0, "sep_files: fork child A");
	if (pid_a < 0)
		return;

	if (pid_a == 0) {
		/* Child A: create and write /fileA. */
		long fd = do_open("/fileA", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			do_exit(1);
		memfill(buf, 'A', 1024);
		long n = do_write(fd, buf, 1024);
		do_close(fd);
		do_exit(n == 1024 ? 0 : 1);
	}

	pid_b = do_fork();
	check(pid_b >= 0, "sep_files: fork child B");
	if (pid_b < 0) {
		int st = 0;
		do_wait(pid_a, &st);
		return;
	}

	if (pid_b == 0) {
		/* Child B: create and write /fileB. */
		long fd = do_open("/fileB", O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			do_exit(1);
		memfill(buf, 'B', 1024);
		long n = do_write(fd, buf, 1024);
		do_close(fd);
		do_exit(n == 1024 ? 0 : 1);
	}

	/* Parent: wait for both children. */
	int status_a = 0, status_b = 0;
	long ra = do_wait(pid_a, &status_a);
	long rb = do_wait(pid_b, &status_b);

	check(ra == pid_a, "sep_files: wait child A ok");
	check(rb == pid_b, "sep_files: wait child B ok");
	check(((status_a >> 8) & 0xff) == 0, "sep_files: child A exit 0");
	check(((status_b >> 8) & 0xff) == 0, "sep_files: child B exit 0");

	/* Read back /fileA and verify. */
	long fd = do_open("/fileA", O_RDONLY, 0);
	check(fd >= 0, "sep_files: open /fileA");
	if (fd >= 0) {
		long n = do_read(fd, buf, 1024);
		check(n == 1024, "sep_files: read /fileA 1024 bytes");
		check(memchk(buf, 'A', 1024), "sep_files: /fileA all 'A'");
		do_close(fd);
	}

	/* Read back /fileB and verify. */
	fd = do_open("/fileB", O_RDONLY, 0);
	check(fd >= 0, "sep_files: open /fileB");
	if (fd >= 0) {
		long n = do_read(fd, buf, 1024);
		check(n == 1024, "sep_files: read /fileB 1024 bytes");
		check(memchk(buf, 'B', 1024), "sep_files: /fileB all 'B'");
		do_close(fd);
	}
}

/*
 * -----------------------------------------------------------------------
 * Test 2: Two children append to the same file concurrently
 *
 * Both children open /shared with O_APPEND and write 4-byte chunks
 * 100 times each (child A writes "AAAA", child B writes "BBBB").
 * Parent waits for both, reads the file, verifies total size is 800
 * bytes and each 4-byte chunk is either "AAAA" or "BBBB".
 * -----------------------------------------------------------------------
 */
static int
chunk_is(const char *p, char c)
{
	return p[0] == c && p[1] == c && p[2] == c && p[3] == c;
}

static void
test_shared_append(void)
{
	char buf[800];
	long pid_a, pid_b;

	/* Create the file first (empty) */
	long fd = do_open("/shared", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	check(fd >= 0, "shared_append: create /shared");
	if (fd < 0)
		return;
	do_close(fd);

	pid_a = do_fork();
	check(pid_a >= 0, "shared_append: fork child A");
	if (pid_a < 0)
		return;

	if (pid_a == 0) {
		/* Child A: append "AAAA" 100 times. */
		long afd = do_open("/shared", O_WRONLY | O_APPEND, 0);
		if (afd < 0)
			do_exit(1);
		for (int i = 0; i < 100; i++) {
			long n = do_write(afd, "AAAA", 4);
			if (n != 4) {
				do_close(afd);
				do_exit(1);
			}
		}
		do_close(afd);
		do_exit(0);
	}

	pid_b = do_fork();
	check(pid_b >= 0, "shared_append: fork child B");
	if (pid_b < 0) {
		int st = 0;
		do_wait(pid_a, &st);
		return;
	}

	if (pid_b == 0) {
		/* Child B: append "BBBB" 100 times. */
		long bfd = do_open("/shared", O_WRONLY | O_APPEND, 0);
		if (bfd < 0)
			do_exit(1);
		for (int i = 0; i < 100; i++) {
			long n = do_write(bfd, "BBBB", 4);
			if (n != 4) {
				do_close(bfd);
				do_exit(1);
			}
		}
		do_close(bfd);
		do_exit(0);
	}

	/* Parent: wait for both children. */
	int status_a = 0, status_b = 0;
	long ra = do_wait(pid_a, &status_a);
	long rb = do_wait(pid_b, &status_b);

	check(ra == pid_a, "shared_append: wait child A ok");
	check(rb == pid_b, "shared_append: wait child B ok");
	check(((status_a >> 8) & 0xff) == 0, "shared_append: child A exit 0");
	check(((status_b >> 8) & 0xff) == 0, "shared_append: child B exit 0");

	/* Read back /shared and verify. */
	fd = do_open("/shared", O_RDONLY, 0);
	check(fd >= 0, "shared_append: open /shared for read");
	if (fd < 0)
		return;

	long total = 0;
	while (total < 800) {
		long n = do_read(fd, buf + total, 800 - total);
		if (n <= 0)
			break;
		total += n;
	}
	do_close(fd);

	check(total == 800, "shared_append: total size 800");

	/* Verify each 4-byte chunk is either "AAAA" or "BBBB". */
	int chunks_ok = 1;
	int count_a = 0, count_b = 0;
	for (long i = 0; i < 800; i += 4) {
		if (chunk_is(buf + i, 'A')) {
			count_a++;
		} else if (chunk_is(buf + i, 'B')) {
			count_b++;
		} else {
			chunks_ok = 0;
			break;
		}
	}

	check(chunks_ok, "shared_append: no interleaved chunks");
	check(count_a == 100, "shared_append: 100 'AAAA' chunks");
	check(count_b == 100, "shared_append: 100 'BBBB' chunks");
}

int
main(void)
{
	msg("=== multiprocess ext2 tests ===\n");

	test_separate_files();
	test_shared_append();

	test_done();
	return 0;
}
