/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * sendfile syscall tests -- verifies file-to-file, file-to-pipe,
 * offset-based, partial, zero-count, and bad-fd error behavior.
 * Requires ext2 filesystem.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read     0
#define __NR_write    1
#define __NR_open     2
#define __NR_close    3
#define __NR_lseek    8
#define __NR_sendfile 40
#define __NR_pipe2    293

/* Open flags. */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

/* Lseek. */
#define SEEK_SET 0

/* Errno. */
#define EBADF    9

/* Raw syscall wrappers. */
static long
sys1(long nr, long a1)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys2(long nr, long a1, long a2)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2)
			  :"rcx", "r11", "memory");
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

/* Test 1: sendfile from file to file. */
static void
test_sendfile_file_to_file(void)
{
	char buf[100];
	int i;

	/* Create source file with 100 bytes of 'A'. */
	long src_fd =
	    sys3(__NR_open, (long)"/sf_src", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (src_fd < 0) {
		check(0, "sendfile file-to-file: open src");
		return;
	}
	for (i = 0; i < 100; i++)
		buf[i] = 'A';
	sys3(__NR_write, src_fd, (long)buf, 100);
	sys1(__NR_close, src_fd);

	/* Reopen source for reading. */
	src_fd = sys3(__NR_open, (long)"/sf_src", O_RDONLY, 0);
	if (src_fd < 0) {
		check(0, "sendfile file-to-file: reopen src");
		return;
	}

	/* Create destination file. */
	long dst_fd =
	    sys3(__NR_open, (long)"/sf_dst", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (dst_fd < 0) {
		check(0, "sendfile file-to-file: open dst");
		return;
	}

	/* Sendfile. */
	long ret = sys4(__NR_sendfile, dst_fd, src_fd, 0, 100);
	check(ret == 100, "sendfile file-to-file: return value");

	sys1(__NR_close, src_fd);
	sys1(__NR_close, dst_fd);

	/* Verify destination contents. */
	dst_fd = sys3(__NR_open, (long)"/sf_dst", O_RDONLY, 0);
	if (dst_fd < 0) {
		check(0, "sendfile file-to-file: reopen dst");
		return;
	}
	for (i = 0; i < 100; i++)
		buf[i] = 0;
	long n = sys3(__NR_read, dst_fd, (long)buf, 100);
	sys1(__NR_close, dst_fd);

	int ok = (n == 100);
	for (i = 0; i < 100 && ok; i++)
		if (buf[i] != 'A')
			ok = 0;
	check(ok, "sendfile file-to-file: data");
}

/* Test 2: sendfile from file to pipe. */
static void
test_sendfile_file_to_pipe(void)
{
	char buf[50];
	int i;

	/* Create source file with 50 bytes of 'B'. */
	long src_fd =
	    sys3(__NR_open, (long)"/sf_pipe_src", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (src_fd < 0) {
		check(0, "sendfile file-to-pipe: open src");
		return;
	}
	for (i = 0; i < 50; i++)
		buf[i] = 'B';
	sys3(__NR_write, src_fd, (long)buf, 50);
	sys1(__NR_close, src_fd);

	/* Reopen source for reading. */
	src_fd = sys3(__NR_open, (long)"/sf_pipe_src", O_RDONLY, 0);
	if (src_fd < 0) {
		check(0, "sendfile file-to-pipe: reopen src");
		return;
	}

	/* Create pipe. */
	int pipefd[2];
	long ret = sys2(__NR_pipe2, (long)pipefd, 0);
	if (ret < 0) {
		check(0, "sendfile file-to-pipe: pipe2");
		sys1(__NR_close, src_fd);
		return;
	}

	/* Sendfile into pipe write end. */
	ret = sys4(__NR_sendfile, (long)pipefd[1], src_fd, 0, 50);
	check(ret == 50, "sendfile file-to-pipe: return value");

	sys1(__NR_close, src_fd);
	sys1(__NR_close, (long)pipefd[1]);

	/* Read from pipe read end and verify. */
	for (i = 0; i < 50; i++)
		buf[i] = 0;
	long n = sys3(__NR_read, (long)pipefd[0], (long)buf, 50);
	sys1(__NR_close, (long)pipefd[0]);

	int ok = (n == 50);
	for (i = 0; i < 50 && ok; i++)
		if (buf[i] != 'B')
			ok = 0;
	check(ok, "sendfile file-to-pipe: data");
}

/* Test 3: sendfile with offset pointer. */
static void
test_sendfile_with_offset(void)
{
	char buf[100];
	int i;

	/* Create source: 50 bytes 'C' then 50 bytes 'D'. */
	long src_fd =
	    sys3(__NR_open, (long)"/sf_off", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (src_fd < 0) {
		check(0, "sendfile offset: open src");
		return;
	}
	for (i = 0; i < 50; i++)
		buf[i] = 'C';
	for (i = 50; i < 100; i++)
		buf[i] = 'D';
	sys3(__NR_write, src_fd, (long)buf, 100);
	sys1(__NR_close, src_fd);

	/* Reopen source for reading. */
	src_fd = sys3(__NR_open, (long)"/sf_off", O_RDONLY, 0);
	if (src_fd < 0) {
		check(0, "sendfile offset: reopen src");
		return;
	}

	/* Create destination. */
	long dst_fd =
	    sys3(__NR_open, (long)"/sf_off_dst", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (dst_fd < 0) {
		check(0, "sendfile offset: open dst");
		return;
	}

	/* Sendfile with offset=50. */
	long offset = 50;
	long ret = sys4(__NR_sendfile, dst_fd, src_fd, (long)&offset, 50);
	check(ret == 50, "sendfile offset: return value");
	check(offset == 100, "sendfile offset: offset updated");

	sys1(__NR_close, src_fd);
	sys1(__NR_close, dst_fd);

	/* Verify destination: should be 50 bytes of 'D'. */
	dst_fd = sys3(__NR_open, (long)"/sf_off_dst", O_RDONLY, 0);
	if (dst_fd < 0) {
		check(0, "sendfile offset: reopen dst");
		return;
	}
	for (i = 0; i < 50; i++)
		buf[i] = 0;
	long n = sys3(__NR_read, dst_fd, (long)buf, 50);
	sys1(__NR_close, dst_fd);

	int ok = (n == 50);
	for (i = 0; i < 50 && ok; i++)
		if (buf[i] != 'D')
			ok = 0;
	check(ok, "sendfile offset: data");
}

/* Test 4: sendfile partial transfer. */
static void
test_sendfile_partial(void)
{
	char buf[200];
	int i;

	/* Create source with 200 bytes of 'E'. */
	long src_fd =
	    sys3(__NR_open, (long)"/sf_partial", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (src_fd < 0) {
		check(0, "sendfile partial: open src");
		return;
	}
	for (i = 0; i < 200; i++)
		buf[i] = 'E';
	sys3(__NR_write, src_fd, (long)buf, 200);
	sys1(__NR_close, src_fd);

	/* Reopen source for reading. */
	src_fd = sys3(__NR_open, (long)"/sf_partial", O_RDONLY, 0);
	if (src_fd < 0) {
		check(0, "sendfile partial: reopen src");
		return;
	}

	/* Create destination. */
	long dst_fd =
	    sys3(__NR_open, (long)"/sf_partial_dst",
		 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (dst_fd < 0) {
		check(0, "sendfile partial: open dst");
		return;
	}

	/* Transfer only 100 of 200 bytes. */
	long ret = sys4(__NR_sendfile, dst_fd, src_fd, 0, 100);
	check(ret == 100, "sendfile partial: return value");

	sys1(__NR_close, src_fd);
	sys1(__NR_close, dst_fd);

	/* Verify destination: 100 bytes of 'E'. */
	dst_fd = sys3(__NR_open, (long)"/sf_partial_dst", O_RDONLY, 0);
	if (dst_fd < 0) {
		check(0, "sendfile partial: reopen dst");
		return;
	}
	for (i = 0; i < 100; i++)
		buf[i] = 0;
	long n = sys3(__NR_read, dst_fd, (long)buf, 100);
	sys1(__NR_close, dst_fd);

	int ok = (n == 100);
	for (i = 0; i < 100 && ok; i++)
		if (buf[i] != 'E')
			ok = 0;
	check(ok, "sendfile partial: data");
}

/* Test 5: sendfile with zero count. */
static void
test_sendfile_zero_count(void)
{
	/* Create a source file. */
	long src_fd =
	    sys3(__NR_open, (long)"/sf_zero", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (src_fd < 0) {
		check(0, "sendfile zero: open src");
		return;
	}
	char buf[10];
	for (int i = 0; i < 10; i++)
		buf[i] = 'Z';
	sys3(__NR_write, src_fd, (long)buf, 10);
	sys1(__NR_close, src_fd);

	src_fd = sys3(__NR_open, (long)"/sf_zero", O_RDONLY, 0);
	if (src_fd < 0) {
		check(0, "sendfile zero: reopen src");
		return;
	}

	long dst_fd =
	    sys3(__NR_open, (long)"/sf_zero_dst", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	if (dst_fd < 0) {
		check(0, "sendfile zero: open dst");
		return;
	}

	long ret = sys4(__NR_sendfile, dst_fd, src_fd, 0, 0);
	check(ret == 0, "sendfile zero count");

	sys1(__NR_close, src_fd);
	sys1(__NR_close, dst_fd);
}

/* Test 6: sendfile with bad file descriptors. */
static void
test_sendfile_bad_fd(void)
{
	long ret = sys4(__NR_sendfile, -1, -1, 0, 100);
	check(ret == -EBADF, "sendfile bad fd");
}

int
main(void)
{
	msg("=== sendfile tests ===\n");

	test_sendfile_file_to_file();
	test_sendfile_file_to_pipe();
	test_sendfile_with_offset();
	test_sendfile_partial();
	test_sendfile_zero_count();
	test_sendfile_bad_fd();

	test_done();
	return 0;
}
