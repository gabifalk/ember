/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * ext2 write stress test -- exercises indirect blocks, double-indirect blocks,
 * truncate/extend, many small files, and deep directory nesting on ext2.
 */

#include <sys/stat.h>
#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read       0
#define __NR_write      1
#define __NR_open       2
#define __NR_close      3
#define __NR_fstat      5
#define __NR_lseek      8
#define __NR_ftruncate  77
#define __NR_mkdir      83
#define __NR_rmdir      84
#define __NR_unlink     87

/* Open flags. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200

/* Lseek whence. */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

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

/* Generate pattern byte for a given offset. */
static unsigned char
pattern_byte(long offset)
{
	return (unsigned char)((offset * 7 + 13) % 251);
}

/* Fill buffer with pattern starting at given file offset. */
static void
fill_pattern(char *buf, long file_offset, long len)
{
	for (long i = 0; i < len; i++)
		buf[i] = (char)pattern_byte(file_offset + i);
}

/*
 * Verify buffer matches expected pattern at given file offset.
 * Returns 1 on success, 0 on mismatch.
 */
static int
verify_pattern(const char *buf, long file_offset, long len)
{
	for (long i = 0; i < len; i++) {
		if ((unsigned char)buf[i] != pattern_byte(file_offset + i))
			return 0;
	}
	return 1;
}

/*
 * Write 'total' bytes of pattern data to fd starting at current position,
 * using 'chunk'-sized writes. Returns total bytes written or -1 on error.
 */
static long
write_pattern(int fd, long start_offset, long total, char *buf, long chunk)
{
	long written = 0;
	while (written < total) {
		long n = (total - written < chunk) ? total - written : chunk;
		fill_pattern(buf, start_offset + written, n);
		long r = sys3(__NR_write, fd, (long)buf, n);
		if (r <= 0)
			return -1;
		written += r;
	}
	return written;
}

/*
 * Read 'total' bytes from fd and verify against expected pattern.
 * Returns 1 on success, 0 on failure.
 */
static int
read_and_verify(int fd, long start_offset, long total, char *buf, long chunk)
{
	long verified = 0;
	while (verified < total) {
		long n = (total - verified < chunk) ? total - verified : chunk;
		long r = sys3(__NR_read, fd, (long)buf, n);
		if (r <= 0)
			return 0;
		if (!verify_pattern(buf, start_offset + verified, r))
			return 0;
		verified += r;
	}
	return 1;
}

/* Simple integer-to-string for building filenames. */
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

/* Build path like "/stress_NN". */
static void
build_stress_path(int idx, char *path)
{
	memcpy(path, "/stress_", 8);
	itoa_simple(idx, path + 8);
}

/*
 * -----------------------------------------------------------------------
 * Test 1: Large file write/read (64KB) -- exercises indirect blocks
 * -----------------------------------------------------------------------
 */
static void
test_large_file(void)
{
	static char buf[4096];
	const long total = 65536;	/* 64KB. */

	int fd =
	    (int)sys3(__NR_open, (long)"/large_test",
		      O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "large file (create)");
		return;
	}

	long wr = write_pattern(fd, 0, total, buf, sizeof(buf));
	sys1(__NR_close, fd);
	if (wr != total) {
		check(0, "large file (write)");
		return;
	}

	fd = (int)sys3(__NR_open, (long)"/large_test", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "large file (reopen)");
		return;
	}

	int ok = read_and_verify(fd, 0, total, buf, sizeof(buf));
	sys1(__NR_close, fd);
	check(ok, "large file 64KB write+verify");

	sys1(__NR_unlink, (long)"/large_test");
}

/*
 * -----------------------------------------------------------------------
 * Test 2: Double-indirect blocks (~300KB)
 * With 1024-byte blocks: 12 direct + 256 indirect = 268KB before double-indirect.
 * 300KB guarantees we hit double-indirect blocks.
 * -----------------------------------------------------------------------
 */
static void
test_double_indirect(void)
{
	static char buf[4096];
	const long total = 300L * 1024;	/* 300KB. */

	int fd =
	    (int)sys3(__NR_open, (long)"/dind_test",
		      O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "double-indirect (create)");
		return;
	}

	long wr = write_pattern(fd, 0, total, buf, sizeof(buf));
	sys1(__NR_close, fd);
	if (wr != total) {
		check(0, "double-indirect (write)");
		return;
	}

	fd = (int)sys3(__NR_open, (long)"/dind_test", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "double-indirect (reopen)");
		return;
	}

	int ok = read_and_verify(fd, 0, total, buf, sizeof(buf));
	sys1(__NR_close, fd);
	check(ok, "double-indirect 300KB write+verify");

	/* Verify size via fstat. */
	fd = (int)sys3(__NR_open, (long)"/dind_test", O_RDONLY, 0);
	if (fd >= 0) {
		struct stat st;
		memset(&st, 0, sizeof(st));
		long r = sys2(__NR_fstat, fd, (long)&st);
		sys1(__NR_close, fd);
		check(r == 0
		      && st.st_size == total, "double-indirect fstat size");
	}

	sys1(__NR_unlink, (long)"/dind_test");
}

/*
 * -----------------------------------------------------------------------
 * Test 3: Truncate and extend
 * Write 8KB, truncate to 4KB, verify size, extend by writing at 8KB,
 * then verify: [0..4KB] = original data, [4KB..8KB] = zeros, [8KB..12KB] = new data
 * -----------------------------------------------------------------------
 */
static void
test_truncate_extend(void)
{
	static char buf[4096];
	const long sz_8k = 8192;
	const long sz_4k = 4096;

	/* Write 8KB of pattern data. */
	int fd =
	    (int)sys3(__NR_open, (long)"/trunc_test",
		      O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "truncate (create)");
		return;
	}

	long wr = write_pattern(fd, 0, sz_8k, buf, sizeof(buf));
	if (wr != sz_8k) {
		sys1(__NR_close, fd);
		check(0, "truncate (write 8K)");
		return;
	}
	/* Truncate to 4KB. */
	long r = sys2(__NR_ftruncate, fd, sz_4k);
	check(r == 0, "ftruncate to 4KB");

	/* Verify size via fstat. */
	struct stat st;
	memset(&st, 0, sizeof(st));
	sys2(__NR_fstat, fd, (long)&st);
	check(st.st_size == sz_4k, "fstat after truncate");

	/* Seek to 8KB and write 4KB of new pattern (offset 8192) */
	sys3(__NR_lseek, fd, sz_8k, SEEK_SET);
	fill_pattern(buf, sz_8k, sz_4k);
	sys3(__NR_write, fd, (long)buf, sz_4k);
	sys1(__NR_close, fd);

	/* Read back and verify three regions. */
	fd = (int)sys3(__NR_open, (long)"/trunc_test", O_RDONLY, 0);
	if (fd < 0) {
		check(0, "truncate (reopen)");
		return;
	}
	/* Region 1: [0..4KB] should be original pattern. */
	r = sys3(__NR_read, fd, (long)buf, sz_4k);
	int ok1 = (r == sz_4k) && verify_pattern(buf, 0, sz_4k);
	check(ok1, "truncate: first 4KB intact");

	/* Region 2: [4KB..8KB] should be zeros (truncated then extended = sparse) */
	r = sys3(__NR_read, fd, (long)buf, sz_4k);
	int ok2 = (r == sz_4k);
	if (ok2) {
		for (long i = 0; i < sz_4k; i++) {
			if (buf[i] != 0) {
				ok2 = 0;
				break;
			}
		}
	}
	check(ok2, "truncate: middle 4KB zeros");

	/* Region 3: [8KB..12KB] should be new pattern data. */
	r = sys3(__NR_read, fd, (long)buf, sz_4k);
	int ok3 = (r == sz_4k) && verify_pattern(buf, sz_8k, sz_4k);
	check(ok3, "truncate: last 4KB new data");

	sys1(__NR_close, fd);
	sys1(__NR_unlink, (long)"/trunc_test");
}

/*
 * -----------------------------------------------------------------------
 * Test 4: Many small files (50 files, create/verify/unlink)
 * -----------------------------------------------------------------------
 */
static void
test_many_small_files(void)
{
	char path[32];
	char wbuf[64];
	char rbuf[64];
	int all_ok = 1;

	/* Create 50 files with unique content. */
	for (int i = 0; i < 50; i++) {
		build_stress_path(i, path);
		int fd =
		    (int)sys3(__NR_open, (long)path,
			      O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			all_ok = 0;
			break;
		}
		/* Write "fileNN:XXYYZZ\n" with pattern bytes unique to i. */
		int len = 0;
		memcpy(wbuf, "file", 4);
		len = 4;
		len += itoa_simple(i, wbuf + len);
		wbuf[len++] = ':';
		for (int j = 0; j < 8; j++)
			wbuf[len++] = (char)('A' + ((i * 3 + j) % 26));
		wbuf[len++] = '\n';

		long wr = sys3(__NR_write, fd, (long)wbuf, len);
		sys1(__NR_close, fd);
		if (wr != len) {
			all_ok = 0;
			break;
		}
	}
	check(all_ok, "many files: create 50");

	/* Read back and verify all 50. */
	all_ok = 1;
	for (int i = 0; i < 50; i++) {
		build_stress_path(i, path);
		int fd = (int)sys3(__NR_open, (long)path, O_RDONLY, 0);
		if (fd < 0) {
			all_ok = 0;
			break;
		}
		/* Reconstruct expected content. */
		int len = 0;
		memcpy(wbuf, "file", 4);
		len = 4;
		len += itoa_simple(i, wbuf + len);
		wbuf[len++] = ':';
		for (int j = 0; j < 8; j++)
			wbuf[len++] = (char)('A' + ((i * 3 + j) % 26));
		wbuf[len++] = '\n';

		memset(rbuf, 0, sizeof(rbuf));
		long rd = sys3(__NR_read, fd, (long)rbuf, sizeof(rbuf));
		sys1(__NR_close, fd);

		if (rd != len || memcmp(rbuf, wbuf, len) != 0) {
			all_ok = 0;
			break;
		}
	}
	check(all_ok, "many files: verify 50");

	/* Unlink all 50. */
	all_ok = 1;
	for (int i = 0; i < 50; i++) {
		build_stress_path(i, path);
		long r = sys1(__NR_unlink, (long)path);
		if (r != 0) {
			all_ok = 0;
			break;
		}
	}
	check(all_ok, "many files: unlink 50");
}

/*
 * -----------------------------------------------------------------------
 * Test 5: Deep directory nesting (/d1/d2/d3/d4/d5)
 * -----------------------------------------------------------------------
 */
static void
test_deep_dirs(void)
{
	/* Create nested directories. */
	long r;
	r = sys2(__NR_mkdir, (long)"/d1", 0755);
	if (r != 0) {
		check(0, "deep dirs (mkdir d1)");
		return;
	}
	r = sys2(__NR_mkdir, (long)"/d1/d2", 0755);
	if (r != 0) {
		check(0, "deep dirs (mkdir d2)");
		return;
	}
	r = sys2(__NR_mkdir, (long)"/d1/d2/d3", 0755);
	if (r != 0) {
		check(0, "deep dirs (mkdir d3)");
		return;
	}
	r = sys2(__NR_mkdir, (long)"/d1/d2/d3/d4", 0755);
	if (r != 0) {
		check(0, "deep dirs (mkdir d4)");
		return;
	}
	r = sys2(__NR_mkdir, (long)"/d1/d2/d3/d4/d5", 0755);
	if (r != 0) {
		check(0, "deep dirs (mkdir d5)");
		return;
	}

	check(1, "deep dirs: mkdir 5 levels");

	/* Create a file inside the deepest directory. */
	const char *fpath = "/d1/d2/d3/d4/d5/deep.txt";
	const char *data = "deep nesting works\n";
	int dlen = 19;

	int fd = (int)sys3(__NR_open, (long)fpath, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "deep dirs (create file)");
		return;
	}
	sys3(__NR_write, fd, (long)data, dlen);
	sys1(__NR_close, fd);

	/* Read it back. */
	char rbuf[64];
	memset(rbuf, 0, sizeof(rbuf));
	fd = (int)sys3(__NR_open, (long)fpath, O_RDONLY, 0);
	if (fd < 0) {
		check(0, "deep dirs (reopen file)");
		return;
	}
	long rd = sys3(__NR_read, fd, (long)rbuf, sizeof(rbuf));
	sys1(__NR_close, fd);
	check(rd == dlen
	      && memcmp(rbuf, data, dlen) == 0, "deep dirs: file read+verify");

	/* Cleanup: unlink file, then rmdir in reverse order. */
	sys1(__NR_unlink, (long)fpath);
	r = sys1(__NR_rmdir, (long)"/d1/d2/d3/d4/d5");
	r |= sys1(__NR_rmdir, (long)"/d1/d2/d3/d4");
	r |= sys1(__NR_rmdir, (long)"/d1/d2/d3");
	r |= sys1(__NR_rmdir, (long)"/d1/d2");
	r |= sys1(__NR_rmdir, (long)"/d1");
	check(r == 0, "deep dirs: cleanup rmdir");
}

int
main(void)
{
	msg("=== ext2 stress tests ===\n");

	test_large_file();
	test_double_indirect();
	test_truncate_extend();
	test_many_small_files();
	test_deep_dirs();

	test_done();
	return 0;
}
