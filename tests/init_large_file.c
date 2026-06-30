/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* Raw syscall wrappers. */
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

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_stat   4
#define SYS_lseek  8

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000

#define SEEK_SET  0
#define SEEK_END  2

#define STAT_SIZE   144
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
do_stat(const char *path, void *buf)
{
	return sys2(SYS_stat, (long)path, (long)buf);
}

static long
do_lseek(long fd, long offset, long whence)
{
	return sys3(SYS_lseek, fd, offset, whence);
}

/* Expected byte at position i. */
static unsigned char
pattern(long i)
{
	return (unsigned char)((i * 7 + 3) & 0xff);
}

#define SIZE_50K  (50L * 1024)
#define SIZE_10K  (10L * 1024)
#define SIZE_60K  (60L * 1024)
#define CHUNK     4096

static const char *testpath = "/test_large.bin";

/* Test 1: write 50KB patterned data in 4KB chunks, reopen, read back and verify. */
static void
test_sequential_write_read(void)
{
	char buf[CHUNK];
	long fd, n, i, off;
	int ok;

	fd = do_open(testpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "seq_wr (open write)");
		return;
	}

	/* Write 50KB in 4KB chunks. */
	for (off = 0; off < SIZE_50K; off += CHUNK) {
		long todo = SIZE_50K - off;
		if (todo > CHUNK)
			todo = CHUNK;
		for (i = 0; i < todo; i++)
			buf[i] = (char)pattern(off + i);
		n = do_write(fd, buf, todo);
		if (n != todo) {
			do_close(fd);
			check(0, "seq_wr (write)");
			return;
		}
	}
	do_close(fd);

	/* Reopen and read back. */
	fd = do_open(testpath, O_RDONLY, 0);
	if (fd < 0) {
		check(0, "seq_wr (open read)");
		return;
	}

	ok = 1;
	for (off = 0; off < SIZE_50K; off += CHUNK) {
		long todo = SIZE_50K - off;
		if (todo > CHUNK)
			todo = CHUNK;
		n = do_read(fd, buf, todo);
		if (n != todo) {
			ok = 0;
			break;
		}
		for (i = 0; i < todo; i++) {
			if ((unsigned char)buf[i] != pattern(off + i)) {
				ok = 0;
				break;
			}
		}
		if (!ok)
			break;
	}
	do_close(fd);
	check(ok, "seq_wr 50KB write+read");
}

/* Test 2: random reads at various offsets. */
static void
test_random_read(void)
{
	long offsets[] = { 0, 25000, 49000, 12345 };
	int noffsets = 4;
	char buf[100];
	long fd, n, i, j;
	int ok = 1;

	fd = do_open(testpath, O_RDONLY, 0);
	if (fd < 0) {
		check(0, "rand_read (open)");
		return;
	}

	for (j = 0; j < noffsets; j++) {
		long pos = do_lseek(fd, offsets[j], SEEK_SET);
		if (pos != offsets[j]) {
			ok = 0;
			break;
		}
		n = do_read(fd, buf, 100);
		/* At offset 49000, only 51200-49000=2200 bytes remain, so 100 is fine. */
		if (n != 100) {
			ok = 0;
			break;
		}
		for (i = 0; i < 100; i++) {
			if ((unsigned char)buf[i] != pattern(offsets[j] + i)) {
				ok = 0;
				break;
			}
		}
		if (!ok)
			break;
	}
	do_close(fd);
	check(ok, "rand_read at offsets");
}

/* Test 3: stat size after 50KB write. */
static void
test_stat_size(void)
{
	char statbuf[STAT_SIZE];
	long r = do_stat(testpath, statbuf);
	if (r != 0) {
		check(0, "stat_size (stat)");
		return;
	}
	long sz = *(long *)(statbuf + ST_SIZE_OFF);
	check(sz == SIZE_50K, "stat_size == 51200");
}

/* Test 4: append 10KB, verify total 60KB, read back appended region. */
static void
test_append(void)
{
	char buf[CHUNK];
	long fd, n, i, off;
	int ok;

	fd = do_open(testpath, O_RDWR, 0);
	if (fd < 0) {
		check(0, "append (open)");
		return;
	}

	/* Seek to end. */
	long end = do_lseek(fd, 0, SEEK_END);
	if (end != SIZE_50K) {
		do_close(fd);
		check(0, "append (seek end)");
		return;
	}

	/* Write 10KB more with continuing pattern. */
	for (off = 0; off < SIZE_10K; off += CHUNK) {
		long todo = SIZE_10K - off;
		if (todo > CHUNK)
			todo = CHUNK;
		for (i = 0; i < todo; i++)
			buf[i] = (char)pattern(SIZE_50K + off + i);
		n = do_write(fd, buf, todo);
		if (n != todo) {
			do_close(fd);
			check(0, "append (write)");
			return;
		}
	}
	do_close(fd);

	/* Verify total size via stat. */
	char statbuf[STAT_SIZE];
	long r = do_stat(testpath, statbuf);
	if (r != 0) {
		check(0, "append (stat)");
		return;
	}
	long sz = *(long *)(statbuf + ST_SIZE_OFF);
	if (sz != SIZE_60K) {
		check(0, "append (size 60K)");
		return;
	}

	/* Read back appended region and verify. */
	fd = do_open(testpath, O_RDONLY, 0);
	if (fd < 0) {
		check(0, "append (open read)");
		return;
	}

	do_lseek(fd, SIZE_50K, SEEK_SET);
	ok = 1;
	for (off = 0; off < SIZE_10K; off += CHUNK) {
		long todo = SIZE_10K - off;
		if (todo > CHUNK)
			todo = CHUNK;
		n = do_read(fd, buf, todo);
		if (n != todo) {
			ok = 0;
			break;
		}
		for (i = 0; i < todo; i++) {
			if ((unsigned char)buf[i] !=
			    pattern(SIZE_50K + off + i)) {
				ok = 0;
				break;
			}
		}
		if (!ok)
			break;
	}
	do_close(fd);
	check(ok, "append 10KB verify");
}

int
main(void)
{
	msg("=== large file tests ===\n");
	test_sequential_write_read();
	test_random_read();
	test_stat_size();
	test_append();
	test_done();
	return 0;
}
