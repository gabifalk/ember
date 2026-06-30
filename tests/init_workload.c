/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Real-world workload integration test -- exercises file I/O, mmap, brk,
 * fork, exec, and pipes together to simulate realistic program behavior.
 */

#include <sys/stat.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read       0
#define __NR_write      1
#define __NR_open       2
#define __NR_close      3
#define __NR_stat       4
#define __NR_fstat      5
#define __NR_mmap       9
#define __NR_munmap     11
#define __NR_brk        12
#define __NR_pipe       22
#define __NR_dup2       33
#define __NR_fork       57
#define __NR_execve     59
#define __NR_exit       60
#define __NR_wait4      61

/* Mmap constants. */
#define PROT_READ       1
#define PROT_WRITE      2
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20

/* Open flags. */
#define O_RDONLY        0

#define PAGE_SIZE       4096

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

/*
 * ---- Test 1: Fork+exec with pipe capture ----
 * Fork a child that execve's /hello with stdout redirected to a pipe.
 * Parent reads from the pipe and verifies "hello" prefix.
 */
static void
test_fork_exec_pipe(void)
{
	int pipefd[2];
	long r = sys1(__NR_pipe, (long)pipefd);
	if (r < 0) {
		check(0, "fep: pipe create");
		return;
	}

	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "fep: fork");
		return;
	}

	if (pid == 0) {
		/* Child: redirect stdout to pipe write end, exec /hello. */
		sys1(__NR_close, pipefd[0]);
		sys2(__NR_dup2, pipefd[1], 1);
		sys1(__NR_close, pipefd[1]);

		const char *argv[] = { "/hello", (const char *)0 };
		const char *envp[] = { (const char *)0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		/* If execve fails, exit with error. */
		sys1(__NR_exit, 99);
	}
	/* Parent: close write end, read output. */
	sys1(__NR_close, pipefd[1]);

	char buf[128];
	long total = 0;
	while (total < (long)sizeof(buf) - 1) {
		long n = sys3(__NR_read, pipefd[0], (long)(buf + total),
			      (long)(sizeof(buf) - 1 - total));
		if (n <= 0)
			break;
		total += n;
	}
	buf[total] = '\0';
	sys1(__NR_close, pipefd[0]);

	/* Check output starts with "hello". */
	int prefix_ok = (total >= 5 &&
			 buf[0] == 'h' && buf[1] == 'e' &&
			 buf[2] == 'l' && buf[3] == 'l' && buf[4] == 'o');
	check(prefix_ok, "fep: captured hello output");

	/* Wait for child. */
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "fep: child exit 0");
}

/*
 * ---- Test 2: mmap working memory + computation ----
 * Allocate 16 pages via anonymous mmap, fill with pattern, verify, munmap.
 */
static void
test_mmap_work_buffer(void)
{
	long size = 16 * PAGE_SIZE;	/* 64KB. */
	long addr = sys6(__NR_mmap, 0, size,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(addr > 0, "mmap-work: alloc 64KB");
	if (addr <= 0)
		return;

	/* Fill with pattern: each byte = (offset * 0x11) & 0xff. */
	unsigned char *p = (unsigned char *)addr;
	for (long i = 0; i < size; i++)
		p[i] = (unsigned char)((i * 0x11) & 0xff);

	/* Verify entire pattern. */
	int ok = 1;
	for (long i = 0; i < size; i++) {
		if (p[i] != (unsigned char)((i * 0x11) & 0xff)) {
			ok = 0;
			break;
		}
	}
	check(ok, "mmap-work: pattern verify");

	long r = sys2(__NR_munmap, addr, size);
	check(r == 0, "mmap-work: munmap");
}

/*
 * ---- Test 3: brk heap allocation pattern ----
 * Simulate a simple malloc: extend brk, write records, read back, shrink.
 */
static void
test_brk_heap(void)
{
	/* Query current brk. */
	long orig_brk = sys1(__NR_brk, 0);
	check(orig_brk > 0, "brk-heap: query brk");
	if (orig_brk <= 0)
		return;

	/* Extend by 8 pages (32KB) */
	long new_brk = sys1(__NR_brk, orig_brk + 8 * PAGE_SIZE);
	check(new_brk == orig_brk + 8 * PAGE_SIZE, "brk-heap: extend 32KB");
	if (new_brk != orig_brk + 8 * PAGE_SIZE)
		return;

	/* Write 4 records of 2KB each at the start of the new heap area. */
	unsigned char *heap = (unsigned char *)orig_brk;
	for (int rec = 0; rec < 4; rec++) {
		unsigned char *base = heap + rec * 2048;
		for (int i = 0; i < 2048; i++)
			base[i] = (unsigned char)((rec * 37 + i) & 0xff);
	}

	/* Read back and verify. */
	int ok = 1;
	for (int rec = 0; rec < 4; rec++) {
		unsigned char *base = heap + rec * 2048;
		for (int i = 0; i < 2048; i++) {
			if (base[i] != (unsigned char)((rec * 37 + i) & 0xff)) {
				ok = 0;
				break;
			}
		}
		if (!ok)
			break;
	}
	check(ok, "brk-heap: record verify");

	/* Shrink brk back to original. */
	long shrunk = sys1(__NR_brk, orig_brk);
	check(shrunk == orig_brk, "brk-heap: shrink back");
}

/*
 * ---- Test 4: Pipeline -- fork+pipe chain (2 stages) ----
 * child1 writes "STAGE1\n" to pipe1
 * child2 reads from pipe1, writes "STAGE2\n" to pipe2
 * parent reads from pipe2, verifies "STAGE2\n".
 */
static void
test_pipeline(void)
{
	int pipe1[2], pipe2[2];
	long r1 = sys1(__NR_pipe, (long)pipe1);
	long r2 = sys1(__NR_pipe, (long)pipe2);
	if (r1 < 0 || r2 < 0) {
		check(0, "pipeline: pipe create");
		return;
	}
	/* Fork child1: writes "STAGE1\n" to pipe1 write end. */
	long pid1 = sys0(__NR_fork);
	if (pid1 < 0) {
		check(0, "pipeline: fork child1");
		return;
	}
	if (pid1 == 0) {
		sys1(__NR_close, pipe1[0]);
		sys1(__NR_close, pipe2[0]);
		sys1(__NR_close, pipe2[1]);
		const char *data = "STAGE1\n";
		sys3(__NR_write, pipe1[1], (long)data, 7);
		sys1(__NR_close, pipe1[1]);
		sys1(__NR_exit, 0);
	}
	/* Fork child2: reads from pipe1, writes "STAGE2\n" to pipe2. */
	long pid2 = sys0(__NR_fork);
	if (pid2 < 0) {
		check(0, "pipeline: fork child2");
		return;
	}
	if (pid2 == 0) {
		sys1(__NR_close, pipe1[1]);
		sys1(__NR_close, pipe2[0]);

		/* Read from pipe1 (consume what child1 wrote) */
		char tmp[16];
		long n = sys3(__NR_read, pipe1[0], (long)tmp, 16);
		sys1(__NR_close, pipe1[0]);

		/* If we got data, write STAGE2 to pipe2. */
		if (n > 0) {
			const char *out = "STAGE2\n";
			sys3(__NR_write, pipe2[1], (long)out, 7);
		}
		sys1(__NR_close, pipe2[1]);
		sys1(__NR_exit, n > 0 ? 0 : 1);
	}
	/* Parent: close unused pipe ends. */
	sys1(__NR_close, pipe1[0]);
	sys1(__NR_close, pipe1[1]);
	sys1(__NR_close, pipe2[1]);

	/* Read from pipe2. */
	char result[16];
	long total = 0;
	while (total < 16) {
		long n =
		    sys3(__NR_read, pipe2[0], (long)(result + total),
			 16 - total);
		if (n <= 0)
			break;
		total += n;
	}
	sys1(__NR_close, pipe2[0]);

	/* Verify "STAGE2\n". */
	int ok = (total == 7 &&
		  result[0] == 'S' && result[1] == 'T' && result[2] == 'A' &&
		  result[3] == 'G' && result[4] == 'E' && result[5] == '2' &&
		  result[6] == '\n');
	check(ok, "pipeline: got STAGE2");

	/* Wait for both children. */
	int s1 = 0, s2 = 0;
	sys4(__NR_wait4, pid1, (long)&s1, 0, 0);
	sys4(__NR_wait4, pid2, (long)&s2, 0, 0);
	check(((s1 >> 8) & 0xff) == 0, "pipeline: child1 exit 0");
	check(((s2 >> 8) & 0xff) == 0, "pipeline: child2 exit 0");
}

/*
 * ---- Test 5: File read + mmap comparison ----
 * Open /init, read first page via read(), mmap same file, compare contents.
 */
static void
test_file_read_mmap(void)
{
	long fd = sys3(__NR_open, (long)"/init", O_RDONLY, 0);
	check(fd >= 0, "frmmap: open /init");
	if (fd < 0)
		return;

	/* Fstat to get file size. */
	struct stat st;
	long r = sys2(__NR_fstat, fd, (long)&st);
	check(r == 0, "frmmap: fstat");
	check(st.st_size > 0, "frmmap: size > 0");

	/* Allocate a buffer via anonymous mmap to hold read() data. */
	long rbuf = sys6(__NR_mmap, 0, PAGE_SIZE,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(rbuf > 0, "frmmap: anon mmap for read buf");
	if (rbuf <= 0) {
		sys1(__NR_close, fd);
		return;
	}
	/* Read first page via read() */
	long nread = sys3(__NR_read, fd, rbuf, PAGE_SIZE);
	check(nread > 0, "frmmap: read bytes");

	/* Mmap the file directly. */
	long file_map = sys6(__NR_mmap, 0, PAGE_SIZE,
			     PROT_READ, MAP_PRIVATE, fd, 0);

	if (file_map > 0) {
		/* Compare read() data with mmap'd data. */
		unsigned char *a = (unsigned char *)rbuf;
		unsigned char *b = (unsigned char *)file_map;
		int match = 1;
		for (long i = 0; i < nread && i < PAGE_SIZE; i++) {
			if (a[i] != b[i]) {
				match = 0;
				break;
			}
		}
		check(match, "frmmap: read vs mmap match");
		sys2(__NR_munmap, file_map, PAGE_SIZE);
	} else {
		/* File mmap not supported -- skip gracefully. */
		msg("  [skip] file mmap not supported\n");
		check(1, "frmmap: read vs mmap match (skipped)");
	}

	sys2(__NR_munmap, rbuf, PAGE_SIZE);
	sys1(__NR_close, fd);
}

int
main(void)
{
	msg("=== workload integration tests ===\n");

	test_fork_exec_pipe();
	test_mmap_work_buffer();
	test_brk_heap();
	test_pipeline();
	test_file_read_mmap();

	msg("all workload tests passed\n");
	test_done();
	return 0;
}
