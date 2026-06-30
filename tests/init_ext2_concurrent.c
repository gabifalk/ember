/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Concurrent ext2 write tests -- exercises spinlock correctness for ext2
 * block allocation, inode updates, and directory operations under
 * multi-process write contention. SMP-varied (1, 2, 4 cores).
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read       0
#define __NR_write      1
#define __NR_open       2
#define __NR_close      3
#define __NR_stat       4
#define __NR_fork       57
#define __NR_exit       60
#define __NR_wait4      61
#define __NR_mkdir      83
#define __NR_unlink     87

/* Open flags. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0x40
#define O_TRUNC     0x200

/* Struct stat: st_size is at offset 48 on x86_64 Linux. */
#define STAT_SIZE_OFF   48

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

static void
my_memset(void *p, int c, long n)
{
	char *d = p;
	while (n--)
		*d++ = c;
}

static void
my_memcpy(void *dst, const void *src, long n)
{
	char *d = dst;
	const char *s = src;
	while (n--)
		*d++ = *s++;
}

static int
my_memcmp(const void *a, const void *b, unsigned long n)
{
	const unsigned char *p = a, *q = b;
	for (unsigned long i = 0; i < n; i++)
		if (p[i] != q[i])
			return p[i] - q[i];
	return 0;
}

/* Simple integer-to-string. */
static int
itoa_simple(int val, char *buf)
{
	if (val == 0) {
		buf[0] = '0';
		buf[1] = '\0';
		return 1;
	}
	char tmp[12];
	int len = 0;
	while (val > 0) {
		tmp[len++] = '0' + (val % 10);
		val /= 10;
	}
	for (int i = 0; i < len; i++)
		buf[i] = tmp[len - 1 - i];
	buf[len] = '\0';
	return len;
}

/* Wait for N children, return 1 if all exited with code 0. */
static int
wait_children(int n)
{
	int ok = 1;
	for (int i = 0; i < n; i++) {
		int status = 0;
		long rpid = sys4(__NR_wait4, -1, (long)&status, 0, 0);
		if (rpid < 0) {
			ok = 0;
			continue;
		}
		int code = (status >> 8) & 0xff;
		if (code != 0)
			ok = 0;
	}
	return ok;
}

/* ------------------------------------------------------------------ */
/* Test 1: Two children writing 4KB to different files. */
/*  */
/* Fork 2 children, each creates /w1 or /w2 and writes 4096 bytes. */
/* Of a distinct fill byte. Parent reads both back and verifies. */
/* ------------------------------------------------------------------ */
static void
test_two_writers(void)
{
	for (int c = 0; c < 2; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "two-writers (fork)");
			return;
		}
		if (pid == 0) {
			char path[8];
			my_memcpy(path, "/w", 2);
			path[2] = '1' + c;
			path[3] = '\0';

			int fd = (int)sys3(__NR_open, (long)path,
					   O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0) {
				sys1(__NR_exit, 1);
				__builtin_unreachable();
			}

			/* Write 4096 bytes, fill byte = 'A' + c. */
			char buf[512];
			my_memset(buf, 'A' + c, sizeof(buf));
			for (int i = 0; i < 8; i++) {
				long wr =
				    sys3(__NR_write, fd, (long)buf,
					 sizeof(buf));
				if (wr != (long)sizeof(buf)) {
					sys1(__NR_close, fd);
					sys1(__NR_exit, 2);
					__builtin_unreachable();
				}
			}
			sys1(__NR_close, fd);
			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	int ok = wait_children(2);
	check(ok, "two-writers (children ok)");

	/* Verify each file. */
	int verify_ok = 1;
	for (int c = 0; c < 2; c++) {
		char path[8];
		my_memcpy(path, "/w", 2);
		path[2] = '1' + c;
		path[3] = '\0';

		int fd = (int)sys2(__NR_open, (long)path, O_RDONLY);
		if (fd < 0) {
			verify_ok = 0;
			break;
		}

		char expected = 'A' + c;
		long total = 0;
		char buf[512];
		while (1) {
			long n = sys3(__NR_read, fd, (long)buf, sizeof(buf));
			if (n <= 0)
				break;
			for (long j = 0; j < n; j++) {
				if (buf[j] != expected) {
					verify_ok = 0;
					break;
				}
			}
			if (!verify_ok)
				break;
			total += n;
		}
		sys1(__NR_close, fd);
		if (total != 4096)
			verify_ok = 0;
		if (!verify_ok)
			break;
	}
	check(verify_ok, "two-writers (data verified)");

	/* Cleanup. */
	sys1(__NR_unlink, (long)"/w1");
	sys1(__NR_unlink, (long)"/w2");
}

/* ------------------------------------------------------------------ */
/* Test 2: Two children creating files in same directory. */
/*  */
/* Fork 2 children, each creates 4 files (/c1_0..3 and /c2_0..3). */
/* Parent verifies all 8 files exist via stat. */
/* ------------------------------------------------------------------ */
static void
test_concurrent_same_dir(void)
{
	for (int c = 0; c < 2; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "same-dir (fork)");
			return;
		}
		if (pid == 0) {
			for (int f = 0; f < 4; f++) {
				char path[16];
				my_memcpy(path, "/c", 2);
				path[2] = '1' + c;
				path[3] = '_';
				int off = 4;
				off += itoa_simple(f, path + off);
				path[off] = '\0';

				int fd = (int)sys3(__NR_open, (long)path,
						   O_WRONLY | O_CREAT | O_TRUNC,
						   0644);
				if (fd < 0) {
					sys1(__NR_exit, 1);
					__builtin_unreachable();
				}
				/* Write a small marker. */
				char marker = (char)('0' + f);
				sys3(__NR_write, fd, (long)&marker, 1);
				sys1(__NR_close, fd);
			}
			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	int ok = wait_children(2);
	check(ok, "same-dir (children ok)");

	/* Verify all 8 files exist. */
	int all_exist = 1;
	char statbuf[144];
	for (int c = 0; c < 2; c++) {
		for (int f = 0; f < 4; f++) {
			char path[16];
			my_memcpy(path, "/c", 2);
			path[2] = '1' + c;
			path[3] = '_';
			int off = 4;
			off += itoa_simple(f, path + off);
			path[off] = '\0';

			long r = sys2(__NR_stat, (long)path, (long)statbuf);
			if (r != 0) {
				all_exist = 0;
				break;
			}
		}
		if (!all_exist)
			break;
	}
	check(all_exist, "same-dir (all 8 exist)");

	/* Cleanup. */
	for (int c = 0; c < 2; c++) {
		for (int f = 0; f < 4; f++) {
			char path[16];
			my_memcpy(path, "/c", 2);
			path[2] = '1' + c;
			path[3] = '_';
			int off = 4;
			off += itoa_simple(f, path + off);
			path[off] = '\0';
			sys1(__NR_unlink, (long)path);
		}
	}
}

/* ------------------------------------------------------------------ */
/* Test 3: Write + read concurrency. */
/*  */
/* Fork child that writes to /rw_file in 4 chunks of 1KB each. */
/* Parent waits, then reads the entire file and verifies size = 4096. */
/* ------------------------------------------------------------------ */
static void
test_write_then_read(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "wr-read (fork)");
		return;
	}

	if (pid == 0) {
		int fd = (int)sys3(__NR_open, (long)"/rw_file",
				   O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			sys1(__NR_exit, 1);
			__builtin_unreachable();
		}

		char buf[1024];
		my_memset(buf, 'X', sizeof(buf));
		for (int i = 0; i < 4; i++) {
			long wr = sys3(__NR_write, fd, (long)buf, sizeof(buf));
			if (wr != (long)sizeof(buf)) {
				sys1(__NR_close, fd);
				sys1(__NR_exit, 2);
				__builtin_unreachable();
			}
		}
		sys1(__NR_close, fd);
		sys1(__NR_exit, 0);
		__builtin_unreachable();
	}

	int ok = wait_children(1);
	check(ok, "wr-read (child ok)");

	/* Read back and verify size. */
	int fd = (int)sys2(__NR_open, (long)"/rw_file", O_RDONLY);
	if (fd < 0) {
		check(0, "wr-read (open)");
		return;
	}

	long total = 0;
	int data_ok = 1;
	char buf[512];
	while (1) {
		long n = sys3(__NR_read, fd, (long)buf, sizeof(buf));
		if (n <= 0)
			break;
		for (long j = 0; j < n; j++) {
			if (buf[j] != 'X') {
				data_ok = 0;
				break;
			}
		}
		total += n;
	}
	sys1(__NR_close, fd);

	check(total == 4096, "wr-read (size 4096)");
	check(data_ok, "wr-read (data correct)");

	/* Cleanup. */
	sys1(__NR_unlink, (long)"/rw_file");
}

int
main(void)
{
	msg("=== ext2 concurrent write tests ===\n");

	test_two_writers();
	test_concurrent_same_dir();
	test_write_then_read();

	msg("all ext2 concurrent write tests passed\n");
	test_done();
	return 0;
}
