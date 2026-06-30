/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* Raw syscall wrappers (no libc beyond write/reboot in test_common) */
static long
sys1(long nr, long a1)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1)
			  :"rcx", "r11", "memory");
	return ret;
}

static long
sys2(long nr, long a1, long a2)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr), "D"(a1), "S"(a2)
			  :"rcx", "r11", "memory");
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

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_fstat      5
#define SYS_lseek      8
#define SYS_ftruncate  77

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000

#define SEEK_SET  0

#define EBADF   9
#define EINVAL  22

/* st_size is at offset 48 in the x86_64 Linux struct stat. */
#define STAT_SIZE  144
#define ST_SIZE_OFF 48

static long
do_open(const char *path, long flags, long mode)
{
	return sys3(SYS_open, (long)path, flags, mode);
}

static long
do_close(long fd)
{
	return sys1(SYS_close, fd);
}

static long
do_read(long fd, void *buf, long count)
{
	return sys3(SYS_read, fd, (long)buf, count);
}

static long
do_write(long fd, const void *buf, long count)
{
	return sys3(SYS_write, fd, (long)buf, count);
}

static long
do_fstat(long fd, void *buf)
{
	return sys2(SYS_fstat, fd, (long)buf);
}

static long
do_lseek(long fd, long offset, long whence)
{
	return sys3(SYS_lseek, fd, offset, whence);
}

static long
do_ftruncate(long fd, long length)
{
	return sys2(SYS_ftruncate, fd, length);
}

static long
get_size(long fd)
{
	char statbuf[STAT_SIZE];
	long r = do_fstat(fd, statbuf);
	if (r != 0)
		return -1;
	return *(long *)(statbuf + ST_SIZE_OFF);
}

/* Test 1: create file, write data, ftruncate to 0, verify size==0. */
static void
test_truncate_to_zero(void)
{
	long fd =
	    do_open("/test_ftrunc.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "trunc_zero (open)");
		return;
	}

	do_write(fd, "hello world", 11);

	long r = do_ftruncate(fd, 0);
	if (r != 0) {
		do_close(fd);
		check(0, "trunc_zero (ftruncate)");
		return;
	}

	long sz = get_size(fd);
	do_close(fd);
	check(sz == 0, "trunc_zero");
}

/* Test 2: write 1000 bytes, ftruncate to 500, verify size and data. */
static void
test_shrink(void)
{
	long fd = do_open("/test_shrink.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "shrink (open)");
		return;
	}

	/* Write 1000 bytes: repeating pattern 0..249 four times. */
	char buf[1000];
	int i;
	for (i = 0; i < 1000; i++)
		buf[i] = (char)(i & 0xFF);

	do_write(fd, buf, 1000);

	long r = do_ftruncate(fd, 500);
	if (r != 0) {
		do_close(fd);
		check(0, "shrink (ftruncate)");
		return;
	}

	long sz = get_size(fd);
	if (sz != 500) {
		do_close(fd);
		check(0, "shrink (size)");
		return;
	}

	/* Seek to start and read back first 500 bytes. */
	do_lseek(fd, 0, SEEK_SET);
	char rbuf[500];
	long n = do_read(fd, rbuf, 500);
	do_close(fd);

	if (n != 500) {
		check(0, "shrink (read count)");
		return;
	}

	int ok = 1;
	for (i = 0; i < 500; i++) {
		if (rbuf[i] != (char)(i & 0xFF)) {
			ok = 0;
			break;
		}
	}
	check(ok, "shrink");
}

/* Test 3: ftruncate to grow, verify zero-filled hole. */
static void
test_grow(void)
{
	long fd = do_open("/test_grow.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "grow (open)");
		return;
	}

	/* Write 500 bytes. */
	char buf[500];
	int i;
	for (i = 0; i < 500; i++)
		buf[i] = (char)(i & 0xFF);
	do_write(fd, buf, 500);

	/* Grow to 2000. */
	long r = do_ftruncate(fd, 2000);
	if (r != 0) {
		do_close(fd);
		check(0, "grow (ftruncate)");
		return;
	}

	long sz = get_size(fd);
	if (sz != 2000) {
		do_close(fd);
		check(0, "grow (size)");
		return;
	}

	/* Read bytes 500..1999 and verify they are zero. */
	do_lseek(fd, 500, SEEK_SET);
	char zbuf[256];
	int ok = 1;
	long remaining = 1500;
	while (remaining > 0) {
		long toread = remaining < 256 ? remaining : 256;
		long n = do_read(fd, zbuf, toread);
		if (n <= 0) {
			ok = 0;
			break;
		}
		for (i = 0; i < n; i++) {
			if (zbuf[i] != 0) {
				ok = 0;
				break;
			}
		}
		if (!ok)
			break;
		remaining -= n;
	}
	do_close(fd);
	check(ok && remaining == 0, "grow");
}

/* Test 4: ftruncate on read-only fd should fail. */
static void
test_rdonly(void)
{
	/* Ensure file exists. */
	long fd =
	    do_open("/test_rdonly.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "rdonly (create)");
		return;
	}
	do_write(fd, "data", 4);
	do_close(fd);

	fd = do_open("/test_rdonly.txt", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "rdonly (open)");
		return;
	}

	long r = do_ftruncate(fd, 0);
	do_close(fd);
	/* Should fail with -EINVAL or -EBADF. */
	check(r == -EINVAL || r == -EBADF, "rdonly");
}

/* Test 5: ftruncate on bad fd should return -EBADF. */
static void
test_bad_fd(void)
{
	long r = do_ftruncate(-1, 0);
	check(r == -EBADF, "bad_fd");
}

/* Test 6: ftruncate to same size is a no-op, file unchanged. */
static void
test_same_size(void)
{
	long fd = do_open("/test_same.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "same_size (open)");
		return;
	}

	do_write(fd, "ABCDE", 5);

	long r = do_ftruncate(fd, 5);
	if (r != 0) {
		do_close(fd);
		check(0, "same_size (ftruncate)");
		return;
	}

	long sz = get_size(fd);
	if (sz != 5) {
		do_close(fd);
		check(0, "same_size (size)");
		return;
	}

	/* Read back and verify contents. */
	do_lseek(fd, 0, SEEK_SET);
	char buf[5];
	long n = do_read(fd, buf, 5);
	do_close(fd);

	check(n == 5 && buf[0] == 'A' && buf[1] == 'B' && buf[2] == 'C' &&
	      buf[3] == 'D' && buf[4] == 'E', "same_size");
}

int
main(void)
{
	msg("=== ftruncate tests ===\n");
	test_truncate_to_zero();
	test_shrink();
	test_grow();
	test_rdonly();
	test_bad_fd();
	test_same_size();
	test_done();
	return 0;
}
