/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * File-backed mmap test -- exercises MAP_PRIVATE file mappings, COW semantics,
 * mmap-vs-read consistency, and offset mappings on cpio initrd.
 */

#include <sys/stat.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read           0
#define __NR_open           2
#define __NR_close          3
#define __NR_fstat          5
#define __NR_lseek          8
#define __NR_mmap           9
#define __NR_munmap         11

/* Mmap constants. */
#define PROT_READ   1
#define PROT_WRITE  2
#define MAP_PRIVATE 0x02

/* Lseek constants. */
#define SEEK_SET    0

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

static long
sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10), "r"(r8), "r"(r9):"rcx", "r11",
			  "memory");
	return ret;
}

/* Byte-by-byte comparison (no string.h) */
static int
bytes_equal(const unsigned char *a, const unsigned char *b, long n)
{
	for (long i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/* Get file size via fstat. */
static long
file_size(int fd)
{
	struct stat st;
	long r = sys2(__NR_fstat, fd, (long)&st);
	if (r < 0)
		return -1;
	return st.st_size;
}

/* ---- Test 1: Basic file mmap with ELF magic verification ---- */

static void
test_basic_file_mmap(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0 /* O_RDONLY. */ );
	check(fd >= 0, "mmap_basic: open /init");
	if (fd < 0)
		return;

	long sz = file_size((int)fd);
	check(sz > 0, "mmap_basic: fstat size");
	if (sz <= 0) {
		sys1(__NR_close, fd);
		return;
	}

	long addr = sys6(__NR_mmap, 0, sz, PROT_READ, MAP_PRIVATE, fd, 0);
	check(addr > 0 && (addr & 0xfff) == 0, "mmap_basic: mmap ok");
	if (addr <= 0 || (addr & 0xfff) != 0) {
		sys1(__NR_close, fd);
		return;
	}
	/* Verify ELF magic: 0x7f 'E' 'L' 'F'. */
	unsigned char *p = (unsigned char *)addr;
	check(p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F',
	      "mmap_basic: ELF magic");

	sys2(__NR_munmap, addr, sz);
	sys1(__NR_close, fd);
}

/* ---- Test 2: mmap vs read consistency ---- */

static void
test_mmap_vs_read(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	check(fd >= 0, "mmap_read: open /init");
	if (fd < 0)
		return;

	long sz = file_size((int)fd);
	check(sz > 0, "mmap_read: fstat size");
	if (sz <= 0) {
		sys1(__NR_close, fd);
		return;
	}
	/* Read first 64 bytes with read() */
	unsigned char rbuf[64];
	long nr = sys3(__NR_read, fd, (long)rbuf, 64);
	check(nr == 64, "mmap_read: read 64 bytes");
	if (nr != 64) {
		sys1(__NR_close, fd);
		return;
	}
	/* Seek back to start. */
	long pos = sys3(__NR_lseek, fd, 0, SEEK_SET);
	check(pos == 0, "mmap_read: lseek to 0");

	/* Mmap the file. */
	long addr = sys6(__NR_mmap, 0, sz, PROT_READ, MAP_PRIVATE, fd, 0);
	check(addr > 0 && (addr & 0xfff) == 0, "mmap_read: mmap ok");
	if (addr <= 0 || (addr & 0xfff) != 0) {
		sys1(__NR_close, fd);
		return;
	}
	/* Compare mmap'd bytes with read bytes. */
	check(bytes_equal((unsigned char *)addr, rbuf, 64),
	      "mmap_read: mmap matches read");

	sys2(__NR_munmap, addr, sz);
	sys1(__NR_close, fd);
}

/* ---- Test 3: MAP_PRIVATE COW ---- */

static void
test_mmap_private_cow(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	check(fd >= 0, "mmap_cow: open /init");
	if (fd < 0)
		return;

	long sz = file_size((int)fd);
	check(sz > 0, "mmap_cow: fstat size");
	if (sz <= 0) {
		sys1(__NR_close, fd);
		return;
	}
	/* Mmap MAP_PRIVATE with read+write. */
	long addr =
	    sys6(__NR_mmap, 0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	check(addr > 0 && (addr & 0xfff) == 0, "mmap_cow: mmap RW ok");
	if (addr <= 0 || (addr & 0xfff) != 0) {
		sys1(__NR_close, fd);
		return;
	}
	/* Read original first byte. */
	unsigned char *p = (unsigned char *)addr;
	unsigned char orig = p[0];

	/* Write a different byte. */
	unsigned char modified = (unsigned char)(orig ^ 0xff);
	p[0] = modified;
	check(p[0] == modified, "mmap_cow: write took effect");

	/* Munmap and re-mmap -- original data should be back. */
	sys2(__NR_munmap, addr, sz);

	long addr2 = sys6(__NR_mmap, 0, sz, PROT_READ, MAP_PRIVATE, fd, 0);
	check(addr2 > 0 && (addr2 & 0xfff) == 0, "mmap_cow: re-mmap ok");
	if (addr2 <= 0 || (addr2 & 0xfff) != 0) {
		sys1(__NR_close, fd);
		return;
	}

	unsigned char *p2 = (unsigned char *)addr2;
	check(p2[0] == orig, "mmap_cow: original data restored");

	sys2(__NR_munmap, addr2, sz);
	sys1(__NR_close, fd);
}

/* ---- Test 4: mmap with page-aligned offset ---- */

static void
test_mmap_offset(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	check(fd >= 0, "mmap_off: open /init");
	if (fd < 0)
		return;

	long sz = file_size((int)fd);
	check(sz > 0, "mmap_off: fstat size");
	if (sz <= 4096) {
		/* File too small for offset test, skip. */
		msg("  SKIP mmap offset (file < 2 pages)\n");
		sys1(__NR_close, fd);
		return;
	}

	long offset = 4096;
	long map_len = sz - offset;

	/* Read bytes at offset 4096 via read() */
	long pos = sys3(__NR_lseek, fd, offset, SEEK_SET);
	check(pos == offset, "mmap_off: lseek to 4096");

	unsigned char rbuf[64];
	long nr = sys3(__NR_read, fd, (long)rbuf, 64);
	check(nr == 64, "mmap_off: read 64 bytes at offset");
	if (nr != 64) {
		sys1(__NR_close, fd);
		return;
	}
	/* Mmap with offset. */
	long addr =
	    sys6(__NR_mmap, 0, map_len, PROT_READ, MAP_PRIVATE, fd, offset);
	check(addr > 0 && (addr & 0xfff) == 0, "mmap_off: mmap ok");
	if (addr <= 0 || (addr & 0xfff) != 0) {
		sys1(__NR_close, fd);
		return;
	}
	/* Compare mmap'd bytes with read bytes. */
	check(bytes_equal((unsigned char *)addr, rbuf, 64),
	      "mmap_off: offset data matches read");

	sys2(__NR_munmap, addr, map_len);
	sys1(__NR_close, fd);
}

int
main(void)
{
	msg("=== mmap file tests ===\n");

	test_basic_file_mmap();
	test_mmap_vs_read();
	test_mmap_private_cow();
	test_mmap_offset();

	test_done();
	return 0;
}
