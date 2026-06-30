/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * O_APPEND tests -- verifies append-mode write semantics: basic append,
 * lseek ignored for writes, append across fork, and multiple small writes.
 * Requires ext2 filesystem.
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read       0
#define __NR_write      1
#define __NR_open       2
#define __NR_close      3
#define __NR_lseek      8
#define __NR_fork       57
#define __NR_exit       60
#define __NR_wait4      61

/* Open flags. */
#define O_RDONLY        0
#define O_WRONLY        1
#define O_RDWR          2
#define O_CREAT         0x40
#define O_TRUNC         0x200
#define O_APPEND        0x400

/* Lseek. */
#define SEEK_SET        0
#define SEEK_END        2

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
sys4(long nr, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr), "D"(a1), "S"(a2),
			  "d"(a3), "r"(r10):"rcx", "r11", "memory");
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

/*
 * ---------------------------------------------------------------------------
 * Test 1: append_basic
 *
 * Create "/app1" with O_WRONLY|O_CREAT|O_TRUNC, write "AAAA". Close.
 * Reopen with O_WRONLY|O_APPEND. Write "BBBB". Close.
 * Open O_RDONLY, read all, verify content is "AAAABBBB" (8 bytes).
 * ---------------------------------------------------------------------------
 */
static void
test_append_basic(void)
{
	long fd = do_open("/app1", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	check(fd >= 0, "append_basic: create");
	if (fd < 0)
		return;

	long w = do_write(fd, "AAAA", 4);
	check(w == 4, "append_basic: write1");
	do_close(fd);

	fd = do_open("/app1", O_WRONLY | O_APPEND, 0);
	check(fd >= 0, "append_basic: reopen append");
	if (fd < 0)
		return;

	w = do_write(fd, "BBBB", 4);
	check(w == 4, "append_basic: write2");
	do_close(fd);

	fd = do_open("/app1", O_RDONLY, 0);
	check(fd >= 0, "append_basic: open read");
	if (fd < 0)
		return;

	char buf[16];
	long n = do_read(fd, buf, 16);
	do_close(fd);

	check(n == 8 && mem_eq(buf, "AAAABBBB", 8), "append_basic");
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: append_seek_ignored
 *
 * Create "/app2" with O_CREAT|O_TRUNC|O_WRONLY, write "HELLO". Close.
 * Reopen with O_WRONLY|O_APPEND. lseek to offset 0. Write "WORLD". Close.
 * Read back and verify "HELLOWORLD" -- lseek should be ignored for writes.
 * ---------------------------------------------------------------------------
 */
static void
test_append_seek_ignored(void)
{
	long fd = do_open("/app2", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	check(fd >= 0, "append_seek: create");
	if (fd < 0)
		return;

	long w = do_write(fd, "HELLO", 5);
	check(w == 5, "append_seek: write1");
	do_close(fd);

	fd = do_open("/app2", O_WRONLY | O_APPEND, 0);
	check(fd >= 0, "append_seek: reopen");
	if (fd < 0)
		return;

	long pos = do_lseek(fd, 0, SEEK_SET);
	check(pos == 0, "append_seek: lseek");

	w = do_write(fd, "WORLD", 5);
	check(w == 5, "append_seek: write2");
	do_close(fd);

	fd = do_open("/app2", O_RDONLY, 0);
	check(fd >= 0, "append_seek: open read");
	if (fd < 0)
		return;

	char buf[16];
	long n = do_read(fd, buf, 16);
	do_close(fd);

	check(n == 10 && mem_eq(buf, "HELLOWORLD", 10), "append_seek");
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: append_fork
 *
 * Create "/app3" with O_CREAT|O_TRUNC|O_RDWR. Write "START". Open again
 * with O_WRONLY|O_APPEND. Fork. Child writes "CHILD" via the append fd
 * and exits. Parent waits for child, then writes "PARENT" via the append fd.
 * Close. Read file back and verify total length is 15 (5+5+5).
 * ---------------------------------------------------------------------------
 */
static void
test_append_fork(void)
{
	long fd = do_open("/app3", O_RDWR | O_CREAT | O_TRUNC, 0644);
	check(fd >= 0, "append_fork: create");
	if (fd < 0)
		return;

	long w = do_write(fd, "START", 5);
	check(w == 5, "append_fork: write initial");
	do_close(fd);

	long afd = do_open("/app3", O_WRONLY | O_APPEND, 0);
	check(afd >= 0, "append_fork: open append");
	if (afd < 0)
		return;

	long pid = do_fork();
	check(pid >= 0, "append_fork: fork");
	if (pid < 0) {
		do_close(afd);
		return;
	}

	if (pid == 0) {
		/* Child: write via append fd and exit. */
		long cw = do_write(afd, "CHILD", 5);
		do_close(afd);
		do_exit(cw == 5 ? 0 : 1);
	}
	/* Parent: wait for child, then write. */
	int status = 0;
	long rpid = do_wait(pid, &status);
	check(rpid == pid, "append_fork: wait");
	int code = (status >> 8) & 0xff;
	check(code == 0, "append_fork: child ok");

	w = do_write(afd, "PARENT", 6);
	check(w == 6, "append_fork: parent write");
	do_close(afd);

	/* Read back and verify total length. */
	fd = do_open("/app3", O_RDONLY, 0);
	check(fd >= 0, "append_fork: open read");
	if (fd < 0)
		return;

	/* Verify file starts with "START". */
	char buf[32];
	long n = do_read(fd, buf, 32);
	do_close(fd);

	check(n == 16, "append_fork: total length");
	check(mem_eq(buf, "START", 5), "append_fork: prefix");
	/* Child wrote "CHILD", parent wrote "PARENT" -- both appended. */
	check(mem_eq(buf + 5, "CHILD", 5), "append_fork: child data");
	check(mem_eq(buf + 10, "PARENT", 6), "append_fork: parent data");
}

/*
 * ---------------------------------------------------------------------------
 * Test 4: append_multiple
 *
 * Open "/app4" with O_CREAT|O_TRUNC|O_WRONLY|O_APPEND. Write "A" five
 * times. Close. Read back and verify "AAAAA" (5 bytes).
 * ---------------------------------------------------------------------------
 */
static void
test_append_multiple(void)
{
	long fd =
	    do_open("/app4", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	check(fd >= 0, "append_multi: create");
	if (fd < 0)
		return;

	for (int i = 0; i < 5; i++) {
		long w = do_write(fd, "A", 1);
		if (w != 1) {
			check(0, "append_multi: write");
			do_close(fd);
			return;
		}
	}
	do_close(fd);

	fd = do_open("/app4", O_RDONLY, 0);
	check(fd >= 0, "append_multi: open read");
	if (fd < 0)
		return;

	char buf[8];
	long n = do_read(fd, buf, 8);
	do_close(fd);

	check(n == 5 && mem_eq(buf, "AAAAA", 5), "append_multi");
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== O_APPEND tests ===\n");

	test_append_basic();
	test_append_seek_ignored();
	test_append_fork();
	test_append_multiple();

	test_done();
	return 0;
}
