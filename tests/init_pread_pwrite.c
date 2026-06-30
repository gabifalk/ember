/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * pread64/pwrite64 tests -- verifies positional read/write semantics:
 * data correctness, file offset preservation, EOF behavior, and file
 * extension.  Requires ext2 filesystem.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read       0
#define __NR_write      1
#define __NR_open       2
#define __NR_close      3
#define __NR_lseek      8
#define __NR_pread64    17
#define __NR_pwrite64   18

/* Open flags. */
#define O_RDWR          2
#define O_CREAT         0x40
#define O_TRUNC         0x200

/* Lseek. */
#define SEEK_SET        0
#define SEEK_CUR        1

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
sys3(long nr, long a1, long a2, long a3)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3):"rcx", "r11", "memory");
	return ret;
}

static long
sys5(long nr, long a1, long a2, long a3, long a4, long a5)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10), "r"(r8)
			  :"rcx", "r11", "memory");
	return ret;
}

/* Helpers. */
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
do_read(long fd, void *buf, long len)
{
	return sys3(__NR_read, fd, (long)buf, len);
}

static long
do_write(long fd, const void *buf, long len)
{
	return sys3(__NR_write, fd, (long)buf, len);
}

static long
do_lseek(long fd, long offset, long whence)
{
	return sys3(__NR_lseek, fd, offset, whence);
}

static long
do_pread64(long fd, void *buf, long count, long offset)
{
	return sys5(__NR_pread64, fd, (long)buf, count, offset, 0);
}

static long
do_pwrite64(long fd, const void *buf, long count, long offset)
{
	return sys5(__NR_pwrite64, fd, (long)buf, count, offset, 0);
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: pwrite64 basic -- write "AAAA", pwrite64 "BB" at offset 2,
 * read back and verify "AABB"
 * ---------------------------------------------------------------------------
 */
static void
test_pwrite64_basic(void)
{
	long fd = do_open("/test_pw1.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "pwrite64_basic (open)");
		return;
	}

	long w = do_write(fd, "AAAA", 4);
	if (w != 4) {
		do_close(fd);
		check(0, "pwrite64_basic (write)");
		return;
	}

	/* Pwrite64 "BB" at offset 2. */
	long pw = do_pwrite64(fd, "BB", 2, 2);
	if (pw != 2) {
		do_close(fd);
		check(0, "pwrite64_basic (pwrite)");
		return;
	}

	/* Read back from beginning. */
	do_lseek(fd, 0, SEEK_SET);
	char buf[4];
	long n = do_read(fd, buf, 4);
	do_close(fd);

	check(n == 4 && buf[0] == 'A' && buf[1] == 'A' &&
	      buf[2] == 'B' && buf[3] == 'B', "pwrite64_basic");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: pwrite64 preserves file offset -- after pwrite64, lseek(SEEK_CUR)
 * should return the position before the pwrite64, not after it
 * ---------------------------------------------------------------------------
 */
static void
test_pwrite64_offset(void)
{
	long fd = do_open("/test_pw2.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "pwrite64_offset (open)");
		return;
	}

	/* Write 4 bytes -- file offset now at 4. */
	long w = do_write(fd, "AAAA", 4);
	if (w != 4) {
		do_close(fd);
		check(0, "pwrite64_offset (write)");
		return;
	}

	/* Pwrite64 at offset 10 -- should NOT change file offset. */
	long pw = do_pwrite64(fd, "XX", 2, 10);
	if (pw != 2) {
		do_close(fd);
		check(0, "pwrite64_offset (pwrite)");
		return;
	}

	/* Verify offset is still 4. */
	long pos = do_lseek(fd, 0, SEEK_CUR);
	do_close(fd);

	check(pos == 4, "pwrite64_offset");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: pread64 basic -- write "ABCDEFGH", pread64 4 bytes at offset 4,
 * verify reads "EFGH"
 * ---------------------------------------------------------------------------
 */
static void
test_pread64_basic(void)
{
	long fd = do_open("/test_pr1.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "pread64_basic (open)");
		return;
	}

	long w = do_write(fd, "ABCDEFGH", 8);
	if (w != 8) {
		do_close(fd);
		check(0, "pread64_basic (write)");
		return;
	}

	/* Seek to beginning so we can verify offset preservation later. */
	do_lseek(fd, 0, SEEK_SET);

	/* Pread64 4 bytes at offset 4. */
	char buf[4];
	long n = do_pread64(fd, buf, 4, 4);
	do_close(fd);

	check(n == 4 && buf[0] == 'E' && buf[1] == 'F' &&
	      buf[2] == 'G' && buf[3] == 'H', "pread64_basic");
}

/*
 * ---------------------------------------------------------------------------
 * Test 4: pread64 preserves file offset -- after pread64, lseek(SEEK_CUR)
 * should return the position before the pread64
 * ---------------------------------------------------------------------------
 */
static void
test_pread64_offset(void)
{
	long fd = do_open("/test_pr2.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "pread64_offset (open)");
		return;
	}

	long w = do_write(fd, "ABCDEFGH", 8);
	if (w != 8) {
		do_close(fd);
		check(0, "pread64_offset (write)");
		return;
	}

	/* Seek to offset 2. */
	do_lseek(fd, 2, SEEK_SET);

	/* Pread64 at offset 6 -- should NOT change file offset. */
	char buf[2];
	long n = do_pread64(fd, buf, 2, 6);
	if (n != 2) {
		do_close(fd);
		check(0, "pread64_offset (pread)");
		return;
	}

	/* Verify offset is still 2. */
	long pos = do_lseek(fd, 0, SEEK_CUR);
	do_close(fd);

	check(pos == 2, "pread64_offset");
}

/*
 * ---------------------------------------------------------------------------
 * Test 5: pread64 past EOF -- pread64 beyond file size returns 0
 * ---------------------------------------------------------------------------
 */
static void
test_pread64_eof(void)
{
	long fd = do_open("/test_pr3.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "pread64_eof (open)");
		return;
	}

	long w = do_write(fd, "ABCD", 4);
	if (w != 4) {
		do_close(fd);
		check(0, "pread64_eof (write)");
		return;
	}

	/* Pread64 at offset 100, well past EOF. */
	char buf[4];
	long n = do_pread64(fd, buf, 4, 100);
	do_close(fd);

	check(n == 0, "pread64_eof");
}

/*
 * ---------------------------------------------------------------------------
 * Test 6: pwrite64 extends file -- pwrite64 at offset beyond current size,
 * verify file was extended and gap contains zeros
 * ---------------------------------------------------------------------------
 */
static void
test_pwrite64_extend(void)
{
	long fd = do_open("/test_pw3.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		check(0, "pwrite64_extend (open)");
		return;
	}

	/* Write initial 4 bytes. */
	long w = do_write(fd, "AAAA", 4);
	if (w != 4) {
		do_close(fd);
		check(0, "pwrite64_extend (write)");
		return;
	}

	/* Pwrite64 "XY" at offset 8 -- creates a gap of 4 zero bytes. */
	long pw = do_pwrite64(fd, "XY", 2, 8);
	if (pw != 2) {
		do_close(fd);
		check(0, "pwrite64_extend (pwrite)");
		return;
	}

	/* Read back the entire file (should be 10 bytes) */
	do_lseek(fd, 0, SEEK_SET);
	char buf[10];
	long n = do_read(fd, buf, 10);
	do_close(fd);

	if (n != 10) {
		check(0, "pwrite64_extend (size)");
		return;
	}

	/* Verify: "AAAA" + 4 zero bytes + "XY". */
	int ok = (buf[0] == 'A' && buf[1] == 'A' && buf[2] == 'A'
		  && buf[3] == 'A');
	ok = ok && (buf[4] == 0 && buf[5] == 0 && buf[6] == 0 && buf[7] == 0);
	ok = ok && (buf[8] == 'X' && buf[9] == 'Y');
	check(ok, "pwrite64_extend");
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== pread64/pwrite64 tests ===\n");

	test_pwrite64_basic();
	test_pwrite64_offset();
	test_pread64_basic();
	test_pread64_offset();
	test_pread64_eof();
	test_pwrite64_extend();

	test_done();
	return 0;
}
