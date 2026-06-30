/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Block cache and ext2 filesystem stress test -- exercises cache pressure
 * with many small files, concurrent file creation across SMP cores,
 * large sequential writes spanning multiple blocks, and interleaved
 * read/write from forked children.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read    0
#define __NR_write   1
#define __NR_open    2
#define __NR_close   3
#define __NR_lseek   8
#define __NR_fork    57
#define __NR_exit    60
#define __NR_wait4   61
#define __NR_mkdir   83
#define __NR_unlink  87

/* Open flags. */
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_TRUNC    01000

/* Lseek whence. */
#define SEEK_SET   0

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

/* Build path like "/cache_test/file_NNN" into buf.  Returns pointer to buf. */
static char *
make_path(char *buf, const char *prefix, int n)
{
	int i = 0;
	while (*prefix)
		buf[i++] = *prefix++;
	/* Append number. */
	if (n == 0) {
		buf[i++] = '0';
	} else {
		char tmp[16];
		int j = 0;
		int num = n;
		while (num > 0) {
			tmp[j++] = '0' + (num % 10);
			num /= 10;
		}
		while (j > 0)
			buf[i++] = tmp[--j];
	}
	buf[i] = '\0';
	return buf;
}

/* ------------------------------------------------------------------ */
/* Test 1: cache_pressure. */
/* Create 150 small files, then read them all back and verify. */
/* ------------------------------------------------------------------ */
static void
test_cache_pressure(void)
{
	char path[64];
	char buf[64];
	int ok = 1;

	msg("test_cache_pressure\n");

	sys2(__NR_mkdir, (long)"/cache_test", 0755);

	/* Write phase: 150 files, 64 bytes each. */
	for (int i = 0; i < 150; i++) {
		make_path(path, "/cache_test/f_", i);
		my_memset(buf, (char)(i & 0xff), 64);
		long fd =
		    sys3(__NR_open, (long)path, O_WRONLY | O_CREAT | O_TRUNC,
			 0644);
		if (fd < 0) {
			msg("  open-write failed: ");
			print_int(i);
			msg("\n");
			ok = 0;
			continue;
		}
		sys3(__NR_write, fd, (long)buf, 64);
		sys1(__NR_close, fd);
	}

	/* Read-back phase. */
	for (int i = 0; i < 150; i++) {
		make_path(path, "/cache_test/f_", i);
		long fd = sys3(__NR_open, (long)path, O_RDONLY, 0);
		if (fd < 0) {
			msg("  open-read failed: ");
			print_int(i);
			msg("\n");
			ok = 0;
			continue;
		}
		my_memset(buf, 0, 64);
		sys3(__NR_read, fd, (long)buf, 64);
		if ((unsigned char)buf[0] != (unsigned char)(i & 0xff)) {
			msg("  verify failed: ");
			print_int(i);
			msg("\n");
			ok = 0;
		}
		sys1(__NR_close, fd);
	}

	check(ok, "cache_pressure: all 150 files verified");
}

/* ------------------------------------------------------------------ */
/* Test 2: concurrent_create. */
/* Fork 4 children, each creates 10 files in its own subdirectory. */
/* ------------------------------------------------------------------ */
static void
test_concurrent_create(void)
{
	char path[64];
	int status;

	msg("test_concurrent_create\n");

	/* Create directories before forking. */
	for (int c = 0; c < 4; c++) {
		make_path(path, "/conc_", c);
		sys2(__NR_mkdir, (long)path, 0755);
	}

	/* Fork 4 children. */
	for (int c = 0; c < 4; c++) {
		long pid = sys0(__NR_fork);
		if (pid == 0) {
			/* Child. */
			char cpath[64];
			char prefix[32];
			make_path(prefix, "/conc_", c);
			/* Append "/f_" to prefix. */
			int k = 0;
			while (prefix[k])
				k++;
			prefix[k++] = '/';
			prefix[k++] = 'f';
			prefix[k++] = '_';
			prefix[k] = '\0';

			for (int j = 0; j < 10; j++) {
				make_path(cpath, prefix, j);
				long fd =
				    sys3(__NR_open, (long)cpath,
					 O_WRONLY | O_CREAT, 0644);
				if (fd >= 0) {
					sys3(__NR_write, fd, (long)"x", 1);
					sys1(__NR_close, fd);
				}
			}
			sys1(__NR_exit, 0);
		}
	}

	/* Wait for all 4 children. */
	for (int i = 0; i < 4; i++)
		sys4(__NR_wait4, -1, (long)&status, 0, 0);

	/* Verify all 40 files exist. */
	int ok = 1;
	for (int c = 0; c < 4; c++) {
		char prefix[32];
		make_path(prefix, "/conc_", c);
		int k = 0;
		while (prefix[k])
			k++;
		prefix[k++] = '/';
		prefix[k++] = 'f';
		prefix[k++] = '_';
		prefix[k] = '\0';

		for (int j = 0; j < 10; j++) {
			make_path(path, prefix, j);
			long fd = sys3(__NR_open, (long)path, O_RDONLY, 0);
			if (fd < 0) {
				msg("  missing: ");
				msg(path);
				msg("\n");
				ok = 0;
			} else {
				sys1(__NR_close, fd);
			}
		}
	}

	check(ok, "concurrent_create: all 40 files exist");
}

/* ------------------------------------------------------------------ */
/* Test 3: large_sequential_write. */
/* Write 32KB (64 x 512-byte chunks) then read back and verify. */
/* ------------------------------------------------------------------ */
static void
test_large_sequential_write(void)
{
	char buf[512];
	int ok = 1;

	msg("test_large_sequential_write\n");

	/* Write phase. */
	long fd =
	    sys3(__NR_open, (long)"/large_seq", O_WRONLY | O_CREAT | O_TRUNC,
		 0644);
	check(fd >= 0, "large_sequential: open for write");

	for (int i = 0; i < 64; i++) {
		my_memset(buf, (char)(i & 0xff), 512);
		sys3(__NR_write, fd, (long)buf, 512);
	}
	sys1(__NR_close, fd);

	/* Read-back phase. */
	fd = sys3(__NR_open, (long)"/large_seq", O_RDONLY, 0);
	check(fd >= 0, "large_sequential: open for read");

	for (int i = 0; i < 64; i++) {
		my_memset(buf, 0, 512);
		sys3(__NR_read, fd, (long)buf, 512);
		for (int b = 0; b < 512; b++) {
			if ((unsigned char)buf[b] != (unsigned char)(i & 0xff)) {
				msg("  mismatch at chunk ");
				print_int(i);
				msg(" byte ");
				print_int(b);
				msg("\n");
				ok = 0;
				break;
			}
		}
	}
	sys1(__NR_close, fd);

	check(ok, "large_sequential: 32KB verified");
}

/* ------------------------------------------------------------------ */
/* Test 4: interleaved_rw. */
/* Fork 2 children writing different patterns, then verify no. */
/* Corruption after both complete. */
/* ------------------------------------------------------------------ */
static void
test_interleaved_rw(void)
{
	char buf[128];
	int status;

	msg("test_interleaved_rw\n");

	for (int c = 0; c < 2; c++) {
		long pid = sys0(__NR_fork);
		if (pid == 0) {
			/* Child. */
			const char *prefix =
			    (c == 0) ? "/inter_a_" : "/inter_b_";
			char pattern = (c == 0) ? 'A' : 'B';
			char cpath[64];

			for (int j = 0; j < 10; j++) {
				make_path(cpath, prefix, j);
				long fd =
				    sys3(__NR_open, (long)cpath,
					 O_WRONLY | O_CREAT, 0644);
				if (fd >= 0) {
					my_memset(buf, pattern, 128);
					sys3(__NR_write, fd, (long)buf, 128);
					sys1(__NR_close, fd);
				}
			}
			sys1(__NR_exit, 0);
		}
	}

	/* Wait for both children. */
	sys4(__NR_wait4, -1, (long)&status, 0, 0);
	sys4(__NR_wait4, -1, (long)&status, 0, 0);

	/* Verify all 20 files. */
	int ok = 1;
	char path[64];

	for (int j = 0; j < 10; j++) {
		/* Check /inter_a_j contains 'A'. */
		make_path(path, "/inter_a_", j);
		long fd = sys3(__NR_open, (long)path, O_RDONLY, 0);
		if (fd < 0) {
			msg("  missing: ");
			msg(path);
			msg("\n");
			ok = 0;
		} else {
			my_memset(buf, 0, 128);
			sys3(__NR_read, fd, (long)buf, 128);
			if (buf[0] != 'A') {
				msg("  corruption in ");
				msg(path);
				msg("\n");
				ok = 0;
			}
			sys1(__NR_close, fd);
		}

		/* Check /inter_b_j contains 'B'. */
		make_path(path, "/inter_b_", j);
		fd = sys3(__NR_open, (long)path, O_RDONLY, 0);
		if (fd < 0) {
			msg("  missing: ");
			msg(path);
			msg("\n");
			ok = 0;
		} else {
			my_memset(buf, 0, 128);
			sys3(__NR_read, fd, (long)buf, 128);
			if (buf[0] != 'B') {
				msg("  corruption in ");
				msg(path);
				msg("\n");
				ok = 0;
			}
			sys1(__NR_close, fd);
		}
	}

	check(ok, "interleaved_rw: no corruption");
}

/* ------------------------------------------------------------------ */
int
main(void)
{
	msg("=== stress cache tests ===\n");
	test_cache_pressure();
	test_concurrent_create();
	test_large_sequential_write();
	test_interleaved_rw();
	test_done();
	return 0;
}
