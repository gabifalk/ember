/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Syscall negative/fuzz test -- calls syscalls with bad arguments to verify
 * the kernel returns proper error codes instead of crashing or hanging.
 * Runs on cpio initrd (read-only).
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read           0
#define __NR_write          1
#define __NR_open           2
#define __NR_close          3
#define __NR_stat           4
#define __NR_fstat          5
#define __NR_lseek          8
#define __NR_mmap           9
#define __NR_rt_sigaction   13
#define __NR_dup            32
#define __NR_getcwd         79
#define __NR_mkdir          83

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
	register long r10 __asm__("r10") = a3;
	(void)r10;
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

/* Custom raw syscall with arbitrary number in RAX (for invalid syscall test) */
static long
sys_raw(long nr)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret):"a"(nr):"rcx", "r11", "memory");
	return ret;
}

/* Error codes. */
#define EBADF   9
#define EFAULT  14
#define EINVAL  22
#define ENOENT  2
#define ENOSYS  38

/* ---- 1. Bad file descriptors ---- */

static void
test_bad_fd(void)
{
	long r;

	/* Fd = -1. */
	r = sys3(__NR_read, -1, (long)"x", 1);
	check(r == -EBADF, "read fd=-1 EBADF");

	r = sys3(__NR_write, -1, (long)"x", 1);
	check(r == -EBADF, "write fd=-1 EBADF");

	r = sys1(__NR_close, -1);
	check(r == -EBADF, "close fd=-1 EBADF");

	struct stat st;
	r = sys2(__NR_fstat, -1, (long)&st);
	check(r == -EBADF, "fstat fd=-1 EBADF");

	r = sys3(__NR_lseek, -1, 0, 0);
	check(r == -EBADF, "lseek fd=-1 EBADF");

	r = sys1(__NR_dup, -1);
	check(r == -EBADF, "dup fd=-1 EBADF");

	/* Fd = 9999 (way beyond any open fd) */
	r = sys3(__NR_read, 9999, (long)"x", 1);
	check(r == -EBADF, "read fd=9999 EBADF");

	r = sys3(__NR_write, 9999, (long)"x", 1);
	check(r == -EBADF, "write fd=9999 EBADF");

	r = sys1(__NR_close, 9999);
	check(r == -EBADF, "close fd=9999 EBADF");

	r = sys2(__NR_fstat, 9999, (long)&st);
	check(r == -EBADF, "fstat fd=9999 EBADF");

	r = sys3(__NR_lseek, 9999, 0, 0);
	check(r == -EBADF, "lseek fd=9999 EBADF");

	r = sys1(__NR_dup, 9999);
	check(r == -EBADF, "dup fd=9999 EBADF");
}

/* ---- 2. NULL pointers ---- */

static void
test_null_pointers(void)
{
	long r;
	struct stat st;

	/* Stat(NULL, buf) -- should return EFAULT or EINVAL, not crash. */
	r = sys2(__NR_stat, 0, (long)&st);
	check(r == -EFAULT || r == -EINVAL || r == -ENOENT, "stat NULL path");

	/* Open(NULL, 0) -- should return EFAULT or EINVAL, not crash. */
	r = sys2(__NR_open, 0, 0);
	check(r == -EFAULT || r == -EINVAL || r == -ENOENT, "open NULL path");

	/* read(valid_fd, NULL, 100) -- should return EFAULT or EINVAL, not crash. */
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd >= 0) {
		r = sys3(__NR_read, fd, 0, 100);
		check(r == -EFAULT || r == -EINVAL || r == 0, "read NULL buf");
		sys1(__NR_close, fd);
	}
	/* Write(1, NULL, 100) -- should return EFAULT or EINVAL, not crash. */
	r = sys3(__NR_write, 1, 0, 100);
	check(r == -EFAULT || r == -EINVAL || r == 0, "write NULL buf");

	/* Getcwd(NULL, 0) -- should return EFAULT or EINVAL, not crash. */
	r = sys2(__NR_getcwd, 0, 0);
	check(r < 0, "getcwd NULL buf");
}

/* ---- 3. Invalid (empty) paths ---- */

static void
test_invalid_paths(void)
{
	long r;
	struct stat st;

	r = sys2(__NR_open, (long)"", 0);
	check(r == -ENOENT, "open empty path ENOENT");

	r = sys2(__NR_mkdir, (long)"", 0755);
	check(r == -ENOENT, "mkdir empty path ENOENT");

	r = sys2(__NR_stat, (long)"", (long)&st);
	check(r == -ENOENT, "stat empty path ENOENT");
}

/* ---- 4. Huge sizes ---- */

static void
test_huge_sizes(void)
{
	long r;
	char buf[16];

	/* Open a valid file and try to read with huge count. */
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "huge read (open)");
		return;
	}
	/* Read with count = 0xffffffffffffffff -- should not hang. */
	r = sys3(__NR_read, fd, (long)buf, (long)0xffffffffffffffff);
	/*
	 * The kernel might return a partial read, EFAULT, or EINVAL -- just
	 * verify it returned and didn't hang.
	 */
	check(1, "read huge count no hang");
	sys1(__NR_close, fd);

	/* Write with kernel-space buffer address -- should return EFAULT. */
	r = sys3(__NR_write, 1, (long)0xffffffffff000000ULL, 4096);
	check(r == -EFAULT, "write kernel addr EFAULT");

	/* Write with count so large that buf+count wraps -- should return EFAULT. */
	r = sys3(__NR_write, 1, (long)0x7fffffffe000ULL, (long)0x100000);
	check(r == -EFAULT, "write overflow EFAULT");
}

/* ---- 5. Invalid syscall number ---- */

static void
test_invalid_syscall(void)
{
	long r = sys_raw(99999);
	check(r == -ENOSYS, "syscall 99999 ENOSYS");
}

/* ---- 6. Double close ---- */

static void
test_double_close(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "double close (open)");
		return;
	}

	long r = sys1(__NR_close, fd);
	check(r == 0, "first close ok");

	r = sys1(__NR_close, fd);
	check(r == -EBADF, "second close EBADF");
}

/* ---- 7. Bad mmap args ---- */

static void
test_bad_mmap(void)
{
	long r;

	/* Mmap with invalid prot (all bits set) */
	r = sys6(__NR_mmap, 0, 4096, 0xff, 0x22 /* MAP_PRIVATE|MAP_ANON. */ , -1,
		 0);
	check(r < 0, "mmap bad prot");

	/* Mmap with no MAP_PRIVATE or MAP_SHARED (flags=0x20, MAP_ANON only) */
	r = sys6(__NR_mmap, 0, 4096, 3 /* RW. */ , 0x20 /* MAP_ANON only */ , -1,
		 0);
	check(r < 0 || r > 0, "mmap no PRIVATE/SHARED no crash");

	/* Mmap with bad fd (not anon, real fd that isn't a file) */
	r = sys6(__NR_mmap, 0, 4096, 3, 0x02 /* MAP_PRIVATE, no ANON. */ , 99999,
		 0);
	check(r < 0, "mmap bad fd");

	/* Mmap with size=0. */
	r = sys6(__NR_mmap, 0, 0, 3, 0x22, -1, 0);
	check(r < 0, "mmap size=0");
}

/* ---- 8. Bad signal args ---- */

static void
test_bad_sigaction(void)
{
	struct {
		void (*handler) (int);
		unsigned long flags;
		void (*restorer) (void);
		unsigned long mask;
	} sa;
	memset(&sa, 0, sizeof(sa));

	long r;

	/* Signal 0 (invalid) */
	r = sys4(__NR_rt_sigaction, 0, (long)&sa, 0, 8);
	check(r == -EINVAL, "sigaction sig=0 EINVAL");

	/* Signal 64 (out of range for Ember's NSIG=32) */
	r = sys4(__NR_rt_sigaction, 64, (long)&sa, 0, 8);
	check(r == -EINVAL, "sigaction sig=64 EINVAL");

	/* Signal -1 (negative) */
	r = sys4(__NR_rt_sigaction, -1, (long)&sa, 0, 8);
	check(r == -EINVAL, "sigaction sig=-1 EINVAL");

	/* SIGKILL (9) -- should not be catchable. */
	r = sys4(__NR_rt_sigaction, 9, (long)&sa, 0, 8);
	check(r == -EINVAL, "sigaction SIGKILL EINVAL");

	/* SIGSTOP (19) -- should not be catchable. */
	r = sys4(__NR_rt_sigaction, 19, (long)&sa, 0, 8);
	check(r == -EINVAL, "sigaction SIGSTOP EINVAL");
}

int
main(void)
{
	msg("=== syscall fuzz tests ===\n");

	test_bad_fd();
	test_null_pointers();
	test_invalid_paths();
	test_huge_sizes();
	test_invalid_syscall();
	test_double_close();
	test_bad_mmap();
	test_bad_sigaction();

	test_done();
	return 0;
}
