/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* --- Syscall numbers --- */
#define __NR_read    0
#define __NR_write   1
#define __NR_open    2
#define __NR_close   3
#define __NR_mmap    9
#define __NR_munmap  11
#define __NR_dup2    33
#define __NR_fcntl   72
#define __NR_pipe2   293

/* --- Constants --- */
#define O_RDONLY     0
#define O_WRONLY     1
#define O_CREAT      0100
#define PROT_READ    1
#define PROT_WRITE   2
#define MAP_PRIVATE  0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED    0x10
#define PAGE_SIZE    4096

#define EBADF   9
#define EINVAL  22
#define ENOMEM  12
#define ENOENT  2
#define EFAULT  14

/* --- Raw syscall wrappers --- */
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

/* --- Test functions --- */

static void
test_mmap_zero_length(void)
{
	long r = sys6(__NR_mmap, 0, 0, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check_errno(r, -EINVAL, "mmap length=0");
}

static void
test_mmap_bad_prot(void)
{
	long r = sys6(__NR_mmap, 0, PAGE_SIZE, 0xFF,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check_errno(r, -EINVAL, "mmap bad prot 0xFF");
}

static void
test_munmap_bad_alignment(void)
{
	long r = sys2(__NR_munmap, 1, PAGE_SIZE);
	check_errno(r, -EINVAL, "munmap unaligned addr=1");
}

static void
test_read_bad_fd(void)
{
	char buf[1];
	long r = sys3(__NR_read, -1, (long)buf, 1);
	check_errno(r, -EBADF, "read fd=-1");
}

static void
test_read_bad_fd_large(void)
{
	char buf[1];
	long r = sys3(__NR_read, 999, (long)buf, 1);
	check_errno(r, -EBADF, "read fd=999");
}

static void
test_write_bad_fd(void)
{
	long r = sys3(__NR_write, -1, (long)"x", 1);
	check_errno(r, -EBADF, "write fd=-1");
}

static void
test_close_bad_fd(void)
{
	long r = sys1(__NR_close, -1);
	check_errno(r, -EBADF, "close fd=-1");
}

static void
test_close_bad_fd_large(void)
{
	long r = sys1(__NR_close, 999);
	check_errno(r, -EBADF, "close fd=999");
}

static void
test_open_nonexistent(void)
{
	long r = sys3(__NR_open, (long)"/no/such/file/ever", O_RDONLY, 0);
	check_errno(r, -ENOENT, "open nonexistent path");
}

static void
test_dup2_bad_oldfd(void)
{
	long r = sys2(__NR_dup2, -1, 10);
	check_errno(r, -EBADF, "dup2 oldfd=-1");
}

static void
test_dup2_bad_newfd(void)
{
	long r = sys2(__NR_dup2, 0, -1);
	check_errno(r, -EBADF, "dup2 newfd=-1");
}

static void
test_pipe2_bad_flags(void)
{
	int fds[2];
	long r = sys2(__NR_pipe2, (long)fds, 0x80000000);
	/*
	 * Ember does not validate pipe2 flags (accepts any); canary for when
	 * validation is added (will change from 0 to -EINVAL)
	 */
	check(r == 0, "pipe2 bad flags: accepted (stub canary)");
	if (r == 0) {
		sys1(__NR_close, fds[0]);
		sys1(__NR_close, fds[1]);
	}
}

static void
test_fcntl_bad_fd(void)
{
	long r = sys2(__NR_fcntl, -1, 1);	/* F_GETFD on fd=-1. */
	check_errno(r, -EBADF, "fcntl fd=-1");
}

int
main(void)
{
	msg("=== negative args tests ===\n");

	test_mmap_zero_length();
	test_mmap_bad_prot();
	test_munmap_bad_alignment();
	test_read_bad_fd();
	test_read_bad_fd_large();
	test_write_bad_fd();
	test_close_bad_fd();
	test_close_bad_fd_large();
	test_open_nonexistent();
	test_dup2_bad_oldfd();
	test_dup2_bad_newfd();
	test_pipe2_bad_flags();
	test_fcntl_bad_fd();

	test_done();
}
