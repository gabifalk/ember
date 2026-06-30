/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * readv/writev iovec tests -- multi-segment I/O on files and pipes,
 * zero-length iovec entries, single-iovec fallback.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read            0
#define __NR_write           1
#define __NR_open            2
#define __NR_close           3
#define __NR_lseek           8
#define __NR_readv           19
#define __NR_writev          20
#define __NR_pipe2           293

/* Flags. */
#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT      0100
#define O_TRUNC      01000

/* Lseek whence. */
#define SEEK_SET     0

struct iovec {
	void *iov_base;
	unsigned long iov_len;
};

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
do_open(const char *path, long flags, long mode)
{
	return sys3(__NR_open, (long)path, flags, mode);
}

static long
do_close(long fd)
{
	return sys1(__NR_close, fd);
}

static long
do_read(long fd, void *buf, long count)
{
	return sys3(__NR_read, fd, (long)buf, count);
}

static long
do_lseek(long fd, long offset, long whence)
{
	return sys3(__NR_lseek, fd, offset, whence);
}

static long
do_readv(long fd, const struct iovec *iov, long iovcnt)
{
	return sys3(__NR_readv, fd, (long)iov, iovcnt);
}

static long
do_writev(long fd, const struct iovec *iov, long iovcnt)
{
	return sys3(__NR_writev, fd, (long)iov, iovcnt);
}

static long
do_pipe2(int pipefd[2], long flags)
{
	return sys2(__NR_pipe2, (long)pipefd, flags);
}

/* Simple memcmp. */
static int
my_memcmp(const void *a, const void *b, unsigned long n)
{
	const unsigned char *pa = a, *pb = b;
	for (unsigned long i = 0; i < n; i++) {
		if (pa[i] != pb[i])
			return pa[i] - pb[i];
	}
	return 0;
}

/* Simple memset. */
static void
my_memset(void *dst, int c, unsigned long n)
{
	unsigned char *p = dst;
	for (unsigned long i = 0; i < n; i++)
		p[i] = (unsigned char)c;
}

/* Test 1: writev multiple segments to file, read back and verify concatenation. */
static void
test_writev_file(void)
{
	const char seg1[] = "Hello";
	const char seg2[] = ", ";
	const char seg3[] = "world!";

	struct iovec iov[3] = {
		{(void *)seg1, 5},
		{(void *)seg2, 2},
		{(void *)seg3, 6},
	};

	long fd = do_open("/tmp_writev", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "writev file (open failed)");
		return;
	}

	long n = do_writev(fd, iov, 3);
	do_close(fd);

	if (n != 13) {
		check(0, "writev file (wrong count)");
		return;
	}
	/* Read back. */
	fd = do_open("/tmp_writev", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "writev file (reopen failed)");
		return;
	}

	char buf[32];
	my_memset(buf, 0, sizeof(buf));
	long nr = do_read(fd, buf, sizeof(buf));
	do_close(fd);

	check(nr == 13 && my_memcmp(buf, "Hello, world!", 13) == 0,
	      "writev file concatenation");
}

/* Test 2: readv multiple buffers from file - verify scatter. */
static void
test_readv_file(void)
{
	/* First write known data. */
	long fd = do_open("/tmp_readv", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "readv file (open failed)");
		return;
	}

	const char data[] = "AABBCCDDEE";
	struct iovec wv[1] = { {(void *)data, 10} };
	long n = do_writev(fd, wv, 1);
	do_close(fd);
	if (n != 10) {
		check(0, "readv file (write failed)");
		return;
	}
	/* Now readv into 3 buffers: 3 + 4 + 3. */
	fd = do_open("/tmp_readv", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "readv file (reopen failed)");
		return;
	}

	char b1[4], b2[5], b3[4];
	my_memset(b1, 0, sizeof(b1));
	my_memset(b2, 0, sizeof(b2));
	my_memset(b3, 0, sizeof(b3));

	struct iovec rv[3] = {
		{b1, 3},
		{b2, 4},
		{b3, 3},
	};
	n = do_readv(fd, rv, 3);
	do_close(fd);

	int ok = (n == 10) &&
	    my_memcmp(b1, "AAB", 3) == 0 &&
	    my_memcmp(b2, "BCCD", 4) == 0 && my_memcmp(b3, "DEE", 3) == 0;
	check(ok, "readv file scatter");
}

/* Test 3: writev to pipe, read on other end. */
static void
test_writev_pipe(void)
{
	int pipefd[2] = { -1, -1 };
	long r = do_pipe2(pipefd, 0);
	if (r < 0) {
		check(0, "writev pipe (pipe2 failed)");
		return;
	}

	const char s1[] = "abc";
	const char s2[] = "DEF";
	struct iovec iov[2] = {
		{(void *)s1, 3},
		{(void *)s2, 3},
	};

	long n = do_writev(pipefd[1], iov, 2);
	if (n != 6) {
		check(0, "writev pipe (wrong count)");
		do_close(pipefd[0]);
		do_close(pipefd[1]);
		return;
	}

	char buf[8];
	my_memset(buf, 0, sizeof(buf));
	long nr = do_read(pipefd[0], buf, sizeof(buf));
	do_close(pipefd[0]);
	do_close(pipefd[1]);

	check(nr == 6 && my_memcmp(buf, "abcDEF", 6) == 0,
	      "writev pipe integrity");
}

/* Test 4: zero-length iovec in the middle. */
static void
test_zero_length_iovec(void)
{
	const char s1[] = "XX";
	const char s2[] = "YY";

	struct iovec iov[3] = {
		{(void *)s1, 2},
		{(void *)s1, 0},	/* Zero-length, should be skipped. */
		{(void *)s2, 2},
	};

	long fd = do_open("/tmp_zerolen", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "zero-len iovec (open failed)");
		return;
	}

	long n = do_writev(fd, iov, 3);
	do_close(fd);

	if (n != 4) {
		check(0, "zero-len iovec (wrong count)");
		return;
	}

	fd = do_open("/tmp_zerolen", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "zero-len iovec (reopen failed)");
		return;
	}

	char buf[8];
	my_memset(buf, 0, sizeof(buf));
	long nr = do_read(fd, buf, sizeof(buf));
	do_close(fd);

	check(nr == 4 && my_memcmp(buf, "XXYY", 4) == 0,
	      "zero-len iovec skipped");
}

/* Test 5: single iovec behaves like normal write/read. */
static void
test_single_iovec(void)
{
	const char data[] = "single";
	struct iovec wv[1] = { {(void *)data, 6} };

	long fd = do_open("/tmp_single", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "single iovec (open failed)");
		return;
	}

	long n = do_writev(fd, wv, 1);
	do_close(fd);

	if (n != 6) {
		check(0, "single iovec (wrong write count)");
		return;
	}

	fd = do_open("/tmp_single", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "single iovec (reopen failed)");
		return;
	}

	char buf[8];
	my_memset(buf, 0, sizeof(buf));
	struct iovec rv[1] = { {buf, 6} };
	n = do_readv(fd, rv, 1);
	do_close(fd);

	check(n == 6 && my_memcmp(buf, "single", 6) == 0,
	      "single iovec read/write");
}

int
main(void)
{
	msg("=== readv/writev tests ===\n");
	test_writev_file();
	test_readv_file();
	test_writev_pipe();
	test_zero_length_iovec();
	test_single_iovec();
	test_done();
	return 0;
}
