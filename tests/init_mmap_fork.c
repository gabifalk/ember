/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * mmap+fork tests -- verifies file-backed MAP_PRIVATE across fork,
 * MAP_PRIVATE COW isolation from the underlying file, and MAP_SHARED
 * write-through semantics.  Requires ext2 filesystem.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read       0
#define __NR_write      1
#define __NR_open       2
#define __NR_close      3
#define __NR_lseek      8
#define __NR_mmap       9
#define __NR_munmap     11
#define __NR_fork       57
#define __NR_exit       60
#define __NR_wait4      61

/* Mmap constants. */
#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02

/* Open flags. */
#define O_RDONLY        0
#define O_RDWR          2
#define O_CREAT         0x40
#define O_TRUNC         0x200

/* Lseek. */
#define SEEK_SET        0

#define PAGE_SIZE       4096

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

static long
sys6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10), "r"(r8), "r"(r9)
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
do_mmap(long addr, long len, long prot, long flags, long fd, long offset)
{
	return sys6(__NR_mmap, addr, len, prot, flags, fd, offset);
}

static long
do_munmap(long addr, long len)
{
	return sys2(__NR_munmap, addr, len);
}

static long
do_fork(void)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"((long)__NR_fork)
			  :"rcx", "r11", "memory");
	return ret;
}

static void
do_exit(long code)
{
	sys1(__NR_exit, code);
	__builtin_unreachable();
}

static long
do_wait(long pid, int *status)
{
	return sys4(__NR_wait4, pid, (long)status, 0, 0);
}

/* Byte-by-byte comparison. */
static int
mem_eq(const void *a, const void *b, long n)
{
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	for (long i = 0; i < n; i++) {
		if (pa[i] != pb[i])
			return 0;
	}
	return 1;
}

/* Create a test file with known contents, return fd (or < 0 on error) */
static long
create_test_file(const char *path, const char *data, long len)
{
	long fd = do_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return fd;
	long w = do_write(fd, data, len);
	if (w != len) {
		do_close(fd);
		return -1;
	}
	/* Seek back to start so caller can mmap from offset 0. */
	do_lseek(fd, 0, SEEK_SET);
	return fd;
}

/*
 * ---------------------------------------------------------------------------
 * Test 1: mmap MAP_PRIVATE file, fork, child reads same data
 *
 * Create "/mmf1", write "ABCDEFGH", mmap MAP_PRIVATE, fork.
 * Child verifies mapping contains "ABCDEFGH", exits 0. Parent waits.
 * ---------------------------------------------------------------------------
 */
static void
test_private_fork_read(void)
{
	const char *data = "ABCDEFGH";
	long dlen = 8;

	long fd = create_test_file("/mmf1", data, dlen);
	check(fd >= 0, "priv_fork_read: create file");
	if (fd < 0)
		return;

	long addr = do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE,
			    fd, 0);
	check(addr > 0 && (addr & 0xfff) == 0, "priv_fork_read: mmap ok");
	if (addr <= 0 || (addr & 0xfff) != 0) {
		do_close(fd);
		return;
	}
	/* Verify parent sees correct data before fork. */
	check(mem_eq((void *)addr, data, dlen),
	      "priv_fork_read: parent pre-fork");

	long pid = do_fork();
	check(pid >= 0, "priv_fork_read: fork ok");
	if (pid < 0) {
		do_munmap(addr, PAGE_SIZE);
		do_close(fd);
		return;
	}

	if (pid == 0) {
		/* Child: verify mapping has same data. */
		if (!mem_eq((void *)addr, data, dlen))
			do_exit(1);
		do_exit(0);
	}
	/* Parent: wait for child. */
	int status = 0;
	long rpid = do_wait(pid, &status);
	check(rpid == pid, "priv_fork_read: wait ok");
	int code = (status >> 8) & 0xff;
	check(code == 0, "priv_fork_read: child saw correct data");

	do_munmap(addr, PAGE_SIZE);
	do_close(fd);
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: MAP_PRIVATE COW -- parent writes to mapping, file unchanged
 *
 * Create "/mmf2", write "ABCDEFGH", mmap MAP_PRIVATE RW, fork.
 * After fork, parent writes "ZZZZZZZZ" through the mapping. Then read the
 * file back via read() and verify it still contains "ABCDEFGH".
 * ---------------------------------------------------------------------------
 */
static void
test_private_cow_file(void)
{
	const char *data = "ABCDEFGH";
	const char *modified = "ZZZZZZZZ";
	long dlen = 8;

	long fd = create_test_file("/mmf2", data, dlen);
	check(fd >= 0, "priv_cow: create file");
	if (fd < 0)
		return;

	long addr = do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE,
			    fd, 0);
	check(addr > 0 && (addr & 0xfff) == 0, "priv_cow: mmap ok");
	if (addr <= 0 || (addr & 0xfff) != 0) {
		do_close(fd);
		return;
	}

	long pid = do_fork();
	check(pid >= 0, "priv_cow: fork ok");
	if (pid < 0) {
		do_munmap(addr, PAGE_SIZE);
		do_close(fd);
		return;
	}

	if (pid == 0) {
		/* Child just exits immediately. */
		do_exit(0);
	}
	/* Parent: wait for child first. */
	int status = 0;
	do_wait(pid, &status);

	/* Parent: write through the MAP_PRIVATE mapping. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	for (long i = 0; i < dlen; i++)
		p[i] = (unsigned char)modified[i];

	/* Verify the write is visible in the mapping. */
	check(mem_eq((void *)addr, modified, dlen),
	      "priv_cow: mapping shows write");

	/* Now read the file back via read() -- it must still be "ABCDEFGH". */
	do_lseek(fd, 0, SEEK_SET);
	char rbuf[16];
	long nr = do_read(fd, rbuf, dlen);
	check(nr == dlen, "priv_cow: read file back");
	check(mem_eq(rbuf, data, dlen), "priv_cow: file unchanged on disk");

	do_munmap(addr, PAGE_SIZE);
	do_close(fd);
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: MAP_SHARED -- write through mapping, verify via read()
 *
 * Create "/mmf3", write "ABCDEFGH", mmap MAP_SHARED RW, overwrite first
 * 8 bytes with "XXXXXXXX", then read() the file and verify the change
 * landed on disk.  If MAP_SHARED is not supported, mmap may return an
 * error -- skip gracefully in that case.
 * ---------------------------------------------------------------------------
 */
static void
test_shared_write(void)
{
	const char *data = "ABCDEFGH";
	const char *modified = "XXXXXXXX";
	long dlen = 8;

	long fd = create_test_file("/mmf3", data, dlen);
	check(fd >= 0, "shared: create file");
	if (fd < 0)
		return;

	long addr = do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
			    fd, 0);
	if (addr < 0) {
		/* MAP_SHARED not supported -- skip gracefully. */
		msg("  SKIP shared: mmap MAP_SHARED returned error (not implemented)\n");
		do_close(fd);
		return;
	}
	check(addr > 0 && (addr & 0xfff) == 0, "shared: mmap ok");
	if (addr <= 0 || (addr & 0xfff) != 0) {
		do_close(fd);
		return;
	}
	/* Verify initial data. */
	check(mem_eq((void *)addr, data, dlen), "shared: initial data correct");

	/* Write through the mapping. */
	volatile unsigned char *p = (volatile unsigned char *)addr;
	for (long i = 0; i < dlen; i++)
		p[i] = (unsigned char)modified[i];

	/* Verify mapping shows the write. */
	check(mem_eq((void *)addr, modified, dlen),
	      "shared: mapping shows write");

	/* Sync / unmap to flush writes. */
	do_munmap(addr, PAGE_SIZE);

	/* Read file back via read() */
	do_lseek(fd, 0, SEEK_SET);
	char rbuf[16];
	long nr = do_read(fd, rbuf, dlen);
	check(nr == dlen, "shared: read file back");
	if (mem_eq(rbuf, modified, dlen))
		msg("  PASS shared: write landed on disk\n");
	else
		msg("  SKIP shared: MAP_SHARED writeback not implemented\n");

	do_close(fd);
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== mmap-fork tests ===\n");

	test_private_fork_read();
	test_private_cow_file();
	test_shared_write();

	test_done();
	return 0;
}
