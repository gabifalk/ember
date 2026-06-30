/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "test_common.h"

/* Raw syscall wrappers (no libc beyond write/reboot in test_common) */
static long
sys0(long nr)
{
	long ret;
	__asm__ volatile ("syscall":"=a" (ret)
			  :"a"(nr)
			  :"rcx", "r11", "memory");
	return ret;
}

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

/* Syscall numbers. */
#define __NR_write     1
#define __NR_open      2
#define __NR_close     3
#define __NR_access    21
#define __NR_chown     92
#define __NR_fchown    93
#define __NR_lchown    94
#define __NR_getuid    102
#define __NR_setuid    105
#define __NR_getgid    104
#define __NR_setgid    106
#define __NR_geteuid   107
#define __NR_getegid   108
#define __NR_getgroups 115
#define __NR_setgroups 116
#define __NR_faccessat 269

/* Constants. */
#define O_WRONLY   1
#define O_CREAT    0100
#define F_OK       0
#define AT_FDCWD   -100
#define ENOENT     2
#define EBADF      9

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
do_write(long fd, const void *buf, long count)
{
	return sys3(__NR_write, fd, (long)buf, count);
}

/* Create a file and close it; returns 0 on success, negative on error. */
static long
create_file(const char *path, long mode)
{
	long fd = do_open(path, O_WRONLY | O_CREAT, mode);
	if (fd < 0)
		return fd;
	do_close(fd);
	return 0;
}

/* --- Tests --- */

static void
test_getuid(void)
{
	long r = sys0(__NR_getuid);
	check(r == 0, "getuid == 0");
}

static void
test_geteuid(void)
{
	long r = sys0(__NR_geteuid);
	check(r == 0, "geteuid == 0");
}

static void
test_getgid(void)
{
	long r = sys0(__NR_getgid);
	check(r == 0, "getgid == 0");
}

static void
test_getegid(void)
{
	long r = sys0(__NR_getegid);
	check(r == 0, "getegid == 0");
}

static void
test_setuid_stub(void)
{
	long r = sys1(__NR_setuid, 0);
	check(r == 0, "setuid(0) stub");
}

static void
test_setgid_stub(void)
{
	long r = sys1(__NR_setgid, 0);
	check(r == 0, "setgid(0) stub");
}

static void
test_setgroups_stub(void)
{
	long r = sys2(__NR_setgroups, 0, 0);
	check(r == 0, "setgroups(0,0) stub");
}

static void
test_getgroups(void)
{
	long r = sys2(__NR_getgroups, 0, 0);
	check(r >= 0, "getgroups >= 0");
}

static void
test_chown_stub(void)
{
	long r = create_file("/chown_test", 0644);
	if (r < 0) {
		check(0, "chown stub (create)");
		return;
	}

	r = sys3(__NR_chown, (long)"/chown_test", 0, 0);
	check(r == 0, "chown stub");
}

static void
test_fchown_stub(void)
{
	long fd = do_open("/fchown_test", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "fchown stub (open)");
		return;
	}

	long r = sys3(__NR_fchown, fd, 0, 0);
	check(r == 0, "fchown stub");
	do_close(fd);
}

static void
test_lchown_stub(void)
{
	long r = sys3(__NR_lchown, (long)"/chown_test", 0, 0);
	check(r == 0, "lchown stub");
}

static void
test_access_exists(void)
{
	long fd = do_open("/access_test", O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		check(0, "access exists (create)");
		return;
	}
	do_write(fd, "x", 1);
	do_close(fd);

	long r = sys2(__NR_access, (long)"/access_test", F_OK);
	check(r == 0, "access F_OK existing");
}

static void
test_access_noent(void)
{
	long r = sys2(__NR_access, (long)"/nonexistent_file_xyz", F_OK);
	check(r == -ENOENT, "access F_OK nonexistent");
}

static void
test_faccessat(void)
{
	long r = sys3(__NR_faccessat, AT_FDCWD, (long)"/access_test", F_OK);
	check(r == 0, "faccessat AT_FDCWD F_OK");
}

int
main(void)
{
	msg("=== identity/permission tests ===\n");
	test_getuid();
	test_geteuid();
	test_getgid();
	test_getegid();
	test_setuid_stub();
	test_setgid_stub();
	test_setgroups_stub();
	test_getgroups();
	test_chown_stub();
	test_fchown_stub();
	test_lchown_stub();
	test_access_exists();
	test_access_noent();
	test_faccessat();
	test_done();
	return 0;
}
