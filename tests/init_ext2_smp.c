/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Concurrent ext2 operations test -- SMP stress testing for ext2 filesystem
 * under multi-process contention. Exercises spinlock correctness for ext2,
 * block cache, and VFS under parallel file/directory operations.
 */

#include <sys/stat.h>
#include <string.h>
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
#define __NR_getdents64 217

/* Open flags. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_TRUNC     0x200

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

/* Build path like "/proc<proc_id>_file<file_id>". */
static void
build_proc_file_path(int proc_id, int file_id, char *path)
{
	memcpy(path, "/proc", 5);
	int off = 5;
	off += itoa_simple(proc_id, path + off);
	memcpy(path + off, "_file", 5);
	off += 5;
	off += itoa_simple(file_id, path + off);
	path[off] = '\0';
}

/* Build path like "/wr<id>.txt". */
static void
build_wr_path(int id, char *path)
{
	memcpy(path, "/wr", 3);
	int off = 3;
	off += itoa_simple(id, path + off);
	memcpy(path + off, ".txt", 4);
	off += 4;
	path[off] = '\0';
}

/* Build path like "/churn_<id>". */
static void
build_churn_path(int id, char *path)
{
	memcpy(path, "/churn_", 7);
	itoa_simple(id, path + 7);
}

/* Build path like "/smpdir<id>". */
static void
build_dir_path(int id, char *path)
{
	memcpy(path, "/smpdir", 7);
	int off = 7;
	off += itoa_simple(id, path + off);
	path[off] = '\0';
}

/* Build path like "/smpdir<dir_id>/sub<sub_id>". */
static void
build_subdir_path(int dir_id, int sub_id, char *path)
{
	memcpy(path, "/smpdir", 7);
	int off = 7;
	off += itoa_simple(dir_id, path + off);
	memcpy(path + off, "/sub", 4);
	off += 4;
	off += itoa_simple(sub_id, path + off);
	path[off] = '\0';
}

/* Build path like "/smpdir<dir_id>/sub<sub_id>/f<file_id>". */
static void
build_subfile_path(int dir_id, int sub_id, int file_id, char *path)
{
	memcpy(path, "/smpdir", 7);
	int off = 7;
	off += itoa_simple(dir_id, path + off);
	memcpy(path + off, "/sub", 4);
	off += 4;
	off += itoa_simple(sub_id, path + off);
	memcpy(path + off, "/f", 2);
	off += 2;
	off += itoa_simple(file_id, path + off);
	path[off] = '\0';
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

/*
 * ---------------------------------------------------------------------------
 * Test 1: Concurrent file creation
 *
 * Fork 4 children, each creates 10 files with unique names
 * (e.g., /proc0_file0 through /proc0_file9). Parent waits for all,
 * then verifies all 40 files exist via stat().
 * ---------------------------------------------------------------------------
 */
static void
test_concurrent_create(void)
{
	for (int c = 0; c < 4; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "concurrent_create: fork");
			return;
		}
		if (pid == 0) {
			char path[64];
			for (int f = 0; f < 10; f++) {
				build_proc_file_path(c, f, path);
				int fd = (int)sys3(__NR_open, (long)path,
						   O_WRONLY | O_CREAT | O_TRUNC,
						   0644);
				if (fd < 0) {
					sys1(__NR_exit, 1);
					__builtin_unreachable();
				}
				/* Write a small marker so the file is non-empty. */
				char marker = (char)('A' + c);
				sys3(__NR_write, fd, (long)&marker, 1);
				sys1(__NR_close, fd);
			}
			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	int ok = wait_children(4);
	check(ok, "concurrent_create: all children exited 0");

	/* Verify all 40 files exist. */
	int all_exist = 1;
	char path[64];
	struct stat st;
	for (int c = 0; c < 4; c++) {
		for (int f = 0; f < 10; f++) {
			build_proc_file_path(c, f, path);
			long r = sys2(__NR_stat, (long)path, (long)&st);
			if (r != 0) {
				all_exist = 0;
				break;
			}
		}
		if (!all_exist)
			break;
	}
	check(all_exist, "concurrent_create: all 40 files exist");

	/* Cleanup. */
	for (int c = 0; c < 4; c++) {
		for (int f = 0; f < 10; f++) {
			build_proc_file_path(c, f, path);
			sys1(__NR_unlink, (long)path);
		}
	}
}

/*
 * ---------------------------------------------------------------------------
 * Test 2: Concurrent write/read
 *
 * Fork 2 children, each writes unique data to its own file. Parent reads
 * both files after children exit and verifies the contents match.
 * ---------------------------------------------------------------------------
 */
static void
test_concurrent_write_read(void)
{
	static const char *data0 = "child zero wrote this data\n";
	static const char *data1 = "child one wrote this data!!\n";
	int len0 = 27;
	int len1 = 28;

	for (int c = 0; c < 2; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "concurrent_wr: fork");
			return;
		}
		if (pid == 0) {
			char path[32];
			build_wr_path(c, path);
			int fd = (int)sys3(__NR_open, (long)path,
					   O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (fd < 0) {
				sys1(__NR_exit, 1);
				__builtin_unreachable();
			}
			const char *data = (c == 0) ? data0 : data1;
			int len = (c == 0) ? len0 : len1;
			long wr = sys3(__NR_write, fd, (long)data, len);
			sys1(__NR_close, fd);
			sys1(__NR_exit, wr == len ? 0 : 2);
			__builtin_unreachable();
		}
	}

	int ok = wait_children(2);
	check(ok, "concurrent_wr: children exited 0");

	/* Read back and verify each file. */
	char rbuf[64];
	int verify_ok = 1;

	/* File 0. */
	char path[32];
	build_wr_path(0, path);
	int fd = (int)sys3(__NR_open, (long)path, O_RDONLY, 0);
	if (fd < 0) {
		verify_ok = 0;
	} else {
		memset(rbuf, 0, sizeof(rbuf));
		long n = sys3(__NR_read, fd, (long)rbuf, sizeof(rbuf));
		sys1(__NR_close, fd);
		if (n != len0 || memcmp(rbuf, data0, len0) != 0)
			verify_ok = 0;
	}

	/* File 1. */
	build_wr_path(1, path);
	fd = (int)sys3(__NR_open, (long)path, O_RDONLY, 0);
	if (fd < 0) {
		verify_ok = 0;
	} else {
		memset(rbuf, 0, sizeof(rbuf));
		long n = sys3(__NR_read, fd, (long)rbuf, sizeof(rbuf));
		sys1(__NR_close, fd);
		if (n != len1 || memcmp(rbuf, data1, len1) != 0)
			verify_ok = 0;
	}

	check(verify_ok, "concurrent_wr: read-back verified");

	/* Cleanup. */
	build_wr_path(0, path);
	sys1(__NR_unlink, (long)path);
	build_wr_path(1, path);
	sys1(__NR_unlink, (long)path);
}

/*
 * ---------------------------------------------------------------------------
 * Test 3: Concurrent create + unlink
 *
 * Fork 2 children: child 0 creates files /churn_0 .. /churn_19 rapidly,
 * child 1 unlinks /churn_0 .. /churn_19 (some may not exist yet, which
 * is fine -- unlink returns ENOENT). The goal is no crashes and no stale
 * directory entries after both finish.
 * ---------------------------------------------------------------------------
 */
static void
test_create_unlink_race(void)
{
	for (int c = 0; c < 2; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "create_unlink_race: fork");
			return;
		}
		if (pid == 0) {
			char path[32];
			if (c == 0) {
				/* Creator: make files 0..19. */
				for (int i = 0; i < 20; i++) {
					build_churn_path(i, path);
					int fd =
					    (int)sys3(__NR_open, (long)path,
						      O_WRONLY | O_CREAT |
						      O_TRUNC, 0644);
					if (fd >= 0) {
						sys3(__NR_write, fd, (long)"x",
						     1);
						sys1(__NR_close, fd);
					}
				}
			} else {
				/* Unlinker: remove files 0..19 (ENOENT is acceptable) */
				for (int i = 0; i < 20; i++) {
					build_churn_path(i, path);
					sys1(__NR_unlink, (long)path);
				}
			}
			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	int ok = wait_children(2);
	check(ok, "create_unlink_race: no crashes");

	/* Cleanup: remove any survivors (creator may have won the race) */
	char path[32];
	for (int i = 0; i < 20; i++) {
		build_churn_path(i, path);
		sys1(__NR_unlink, (long)path);
	}

	/* Verify no stale entries remain: stat all churn files, expect ENOENT. */
	int clean = 1;
	struct stat st;
	for (int i = 0; i < 20; i++) {
		build_churn_path(i, path);
		long r = sys2(__NR_stat, (long)path, (long)&st);
		if (r == 0) {
			clean = 0;
			break;
		}
	}
	check(clean, "create_unlink_race: no stale entries");
}

/*
 * ---------------------------------------------------------------------------
 * Test 4: Directory operations under contention
 *
 * Fork 2 children. Each creates its own subdirectory (/smpdir0, /smpdir1),
 * creates 2 sub-subdirectories within, and a file in each sub-subdirectory.
 * Parent waits, then verifies the entire structure exists.
 * ---------------------------------------------------------------------------
 */
static void
test_concurrent_dirs(void)
{
	for (int c = 0; c < 2; c++) {
		long pid = sys0(__NR_fork);
		if (pid < 0) {
			check(0, "concurrent_dirs: fork");
			return;
		}
		if (pid == 0) {
			char path[64];

			/* Create top-level dir: /smpdir<c>. */
			build_dir_path(c, path);
			long r = sys2(__NR_mkdir, (long)path, 0755);
			if (r != 0) {
				sys1(__NR_exit, 1);
				__builtin_unreachable();
			}
			/* Create 2 subdirectories and a file in each. */
			for (int s = 0; s < 2; s++) {
				build_subdir_path(c, s, path);
				r = sys2(__NR_mkdir, (long)path, 0755);
				if (r != 0) {
					sys1(__NR_exit, 2);
					__builtin_unreachable();
				}

				build_subfile_path(c, s, 0, path);
				int fd = (int)sys3(__NR_open, (long)path,
						   O_WRONLY | O_CREAT, 0644);
				if (fd < 0) {
					sys1(__NR_exit, 3);
					__builtin_unreachable();
				}
				char tag[16];
				memcpy(tag, "dir", 3);
				int tlen = 3;
				tlen += itoa_simple(c, tag + tlen);
				tag[tlen++] = '_';
				tlen += itoa_simple(s, tag + tlen);
				tag[tlen++] = '\n';
				sys3(__NR_write, fd, (long)tag, tlen);
				sys1(__NR_close, fd);
			}

			sys1(__NR_exit, 0);
			__builtin_unreachable();
		}
	}

	int ok = wait_children(2);
	check(ok, "concurrent_dirs: children exited 0");

	/* Verify structure: 2 top dirs, each with 2 subdirs and 1 file per subdir. */
	int structure_ok = 1;
	char path[64];
	struct stat st;

	for (int c = 0; c < 2; c++) {
		/* Check top dir exists. */
		build_dir_path(c, path);
		if (sys2(__NR_stat, (long)path, (long)&st) != 0) {
			structure_ok = 0;
			break;
		}

		for (int s = 0; s < 2; s++) {
			/* Check subdir exists. */
			build_subdir_path(c, s, path);
			if (sys2(__NR_stat, (long)path, (long)&st) != 0) {
				structure_ok = 0;
				break;
			}
			/* Check file exists and is non-empty. */
			build_subfile_path(c, s, 0, path);
			if (sys2(__NR_stat, (long)path, (long)&st) != 0) {
				structure_ok = 0;
				break;
			}
			if (st.st_size == 0) {
				structure_ok = 0;
				break;
			}
		}
		if (!structure_ok)
			break;
	}
	check(structure_ok, "concurrent_dirs: structure verified");

	/* Cleanup: unlink files, rmdir subdirs, rmdir top dirs. */
	for (int c = 0; c < 2; c++) {
		for (int s = 0; s < 2; s++) {
			build_subfile_path(c, s, 0, path);
			sys1(__NR_unlink, (long)path);
			build_subdir_path(c, s, path);
			sys1(84 /* __NR_rmdir. */ , (long)path);
		}
		build_dir_path(c, path);
		sys1(84 /* __NR_rmdir. */ , (long)path);
	}
}

/* --------------------------------------------------------------------------- */

int
main(void)
{
	msg("=== ext2 smp tests ===\n");

	test_concurrent_create();
	test_concurrent_write_read();
	test_create_unlink_race();
	test_concurrent_dirs();

	msg("all ext2 smp tests passed\n");
	test_done();
	return 0;
}
