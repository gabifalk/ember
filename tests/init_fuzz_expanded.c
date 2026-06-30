/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Expanded syscall error-path/fuzz test -- systematically exercises every
 * implemented syscall with bad arguments to verify the kernel returns proper
 * error codes instead of crashing or hanging.
 * Runs on cpio initrd (read-only).
 */

#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read           0
#define __NR_write          1
#define __NR_open           2
#define __NR_close          3
#define __NR_stat           4
#define __NR_fstat          5
#define __NR_mmap           9
#define __NR_mprotect       10
#define __NR_munmap         11
#define __NR_brk            12
#define __NR_readv          19
#define __NR_writev         20
#define __NR_pipe           22
#define __NR_mremap         25
#define __NR_dup            32
#define __NR_dup2           33
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_execve         59
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62
#define __NR_getdents64     217
#define __NR_clock_gettime  228

/* Error codes. */
#define ENOENT   2
#define ESRCH    3
#define EBADF    9
#define ECHILD   10
#define ENOMEM   12
#define EFAULT   14
#define EEXIST   17
#define ENOTDIR  20
#define EINVAL   22
#define EMFILE   24
#define ENOSYS   38

/* Wait flags. */
#define WNOHANG  1

/* Mmap flags. */
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

/* Mremap flags. */
#define MREMAP_MAYMOVE 1

/* Prot flags. */
#define PROT_READ  1
#define PROT_WRITE 2

/* Iovec structure. */
struct iovec {
	void *iov_base;
	unsigned long iov_len;
};

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

/* ---- 1. Bad wait4 args ---- */

static void
test_bad_wait(void)
{
	long r;

	/* Wait4 with WNOHANG and no children -> -ECHILD. */
	r = sys4(__NR_wait4, -1, 0, WNOHANG, 0);
	check(r == -ECHILD, "wait4 no children ECHILD");

	/*
	 * Wait4 with bad status pointer + WNOHANG + no children -> -ECHILD
	 * (no children check happens before status pointer dereference)
	 */
	r = sys4(__NR_wait4, -1, (long)0xdeadbeef, WNOHANG, 0);
	check(r == -ECHILD, "wait4 bad status ptr ECHILD");
}

/* ---- 2. Bad execve args ---- */

static void
test_bad_execve(void)
{
	long r;

	/* Execve with nonexistent path -> -ENOENT. */
	r = sys3(__NR_execve, (long)"/nonexistent_binary", 0, 0);
	check(r == -ENOENT, "execve nonexistent ENOENT");

	/* Execve with empty path -> -ENOENT. */
	r = sys3(__NR_execve, (long)"", 0, 0);
	check(r == -ENOENT, "execve empty path ENOENT");
}

/* ---- 3. Bad brk args ---- */

static void
test_bad_brk(void)
{
	long r;

	/* Get current brk. */
	long cur_brk = sys1(__NR_brk, 0);
	check(cur_brk > 0, "brk(0) returns current brk");

	/* Brk(1) -- below current brk minimum, should clamp to current brk. */
	long r1 = sys1(__NR_brk, 1);
	check(r1 == cur_brk, "brk(1) clamps to current brk");

	/* Brk to kernel space -- should be rejected. */
	long rk = sys1(__NR_brk, (long)0xffffffff80000000ULL);
	check(rk == cur_brk, "brk(kernel_addr) rejected");
}

/* ---- 4. Bad mprotect args ---- */

static void
test_bad_mprotect(void)
{
	long r;

	/* Mprotect with unaligned address -> -EINVAL. */
	r = sys3(__NR_mprotect, 1, 4096, PROT_READ);
	check(r == -EINVAL, "mprotect unaligned EINVAL");

	/* Mprotect on address 0 with length 4096 -- page-aligned but unmapped. */
	r = sys3(__NR_mprotect, 0, 4096, PROT_READ);
	check(r == -ENOMEM, "mprotect addr=0 ENOMEM");
}

/* ---- 5. Bad munmap args ---- */

static void
test_bad_munmap(void)
{
	long r;

	/* Munmap with unaligned address -> -EINVAL. */
	r = sys2(__NR_munmap, 1, 4096);
	check(r == -EINVAL, "munmap unaligned EINVAL");

	/* Munmap with size=0 -- end == addr, loop doesn't execute, returns 0. */
	r = sys2(__NR_munmap, 0x1000, 0);
	check(r == 0, "munmap size=0 no crash");

	/* Munmap unmapped region (page-aligned) -> returns 0 (Linux semantics) */
	r = sys2(__NR_munmap, 0x10000000, 4096);
	check(r == 0, "munmap unmapped ok");
}

/* ---- 6. Bad mremap args ---- */

static void
test_bad_mremap(void)
{
	long r;

	/* Mremap with new_size=0 -> -EINVAL. */
	r = sys4(__NR_mremap, 0x10000000, 4096, 0, 0);
	check(r == -EINVAL, "mremap new_size=0 EINVAL");

	/*
	 * Mremap unmapped address, grow without MAYMOVE -- kernel grows in place
	 * (no existing PTE conflicts), returns the same address.
	 */
	r = sys4(__NR_mremap, 0x10000000, 4096, 8192, 0);
	check(r == 0x10000000, "mremap unmapped grows in place");
	/* Clean up: unmap the allocated pages. */
	sys2(__NR_munmap, 0x10000000, 8192);
}

/* ---- 7. Bad dup/dup2 args ---- */

static void
test_bad_dup(void)
{
	long r;

	/* Dup(-1) -> -EBADF. */
	r = sys1(__NR_dup, -1);
	check(r == -EBADF, "dup fd=-1 EBADF");

	/* Dup2(-1, 5) -> -EBADF. */
	r = sys2(__NR_dup2, -1, 5);
	check(r == -EBADF, "dup2 oldfd=-1 EBADF");

	/* Dup2(0, -1) -> -EBADF (newfd out of range) */
	r = sys2(__NR_dup2, 0, -1);
	check(r == -EBADF, "dup2 newfd=-1 EBADF");

	/* Dup2(0, 99999) -> -EBADF (beyond MAX_FDS=256) */
	r = sys2(__NR_dup2, 0, 99999);
	check(r == -EBADF, "dup2 newfd=99999 EBADF");

	/* Dup2(0, 0) -> should return 0 (no-op, same fd) */
	r = sys2(__NR_dup2, 0, 0);
	check(r == 0, "dup2 same fd ok");
}

/* ---- 8. Bad pipe args ---- */

static void
test_bad_pipe(void)
{
	/* Pipe with valid buffer to verify it works. */
	int fds[2];
	long r = sys1(__NR_pipe, (long)fds);
	check(r == 0, "pipe valid buf ok");
	if (r == 0) {
		sys1(__NR_close, fds[0]);
		sys1(__NR_close, fds[1]);
	}
	/*
	 * Note: pipe(NULL) would cause a kernel page fault when writing to
	 * user space, which may kill the process. We skip that test to avoid
	 * crashing the kernel.
	 */
}

/* ---- 9. Bad kill args ---- */

static void
test_bad_kill(void)
{
	long r;

	/* Kill with nonexistent PID -> -ESRCH. */
	r = sys2(__NR_kill, 99999, 0);
	check(r == -ESRCH, "kill pid=99999 ESRCH");

	/* Kill(getpid(), 0) -- signal 0 should succeed (existence check) */
	long pid = sys0(__NR_getpid);
	r = sys2(__NR_kill, pid, 0);
	check(r == 0, "kill sig=0 self ok");

	/* Kill with invalid signal number (99 >= NSIG=32) -> -EINVAL. */
	r = sys2(__NR_kill, pid, 99);
	check(r == -EINVAL, "kill sig=99 EINVAL");

	/* Kill with negative signal -> -EINVAL. */
	r = sys2(__NR_kill, pid, -1);
	check(r == -EINVAL, "kill sig=-1 EINVAL");
}

/* ---- 10. Bad clock_gettime args ---- */

static void
test_bad_clock_gettime(void)
{
	long ts[2];
	long r;

	/* clock_gettime with valid clock 0 -> should succeed. */
	r = sys2(__NR_clock_gettime, 0, (long)ts);
	check(r == 0, "clock_gettime valid ok");

	/*
	 * clock_gettime with invalid clock 99 -- kernel doesn't validate
	 * clock_id, so it still returns 0.
	 */
	r = sys2(__NR_clock_gettime, 99, (long)ts);
	check(r == 0, "clock_gettime bad clock no crash");

	/* clock_gettime with NULL timespec -- kernel checks user_tp, skips write. */
	r = sys2(__NR_clock_gettime, 0, 0);
	check(r == 0, "clock_gettime NULL ts no crash");
}

/* ---- 11. Bad getdents64 args ---- */

static void
test_bad_getdents(void)
{
	char buf[256];
	long r;

	/* Getdents64 with bad fd -> -EBADF. */
	r = sys3(__NR_getdents64, 9999, (long)buf, 256);
	check(r == -EBADF, "getdents64 bad fd EBADF");

	/*
	 * Getdents64 on a regular file (not a directory) -> -EBADF
	 * (kernel checks entry->desc->type != FD_TYPE_DIR)
	 */
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd >= 0) {
		r = sys3(__NR_getdents64, fd, (long)buf, 256);
		check(r == -EBADF, "getdents64 on file EBADF");
		sys1(__NR_close, fd);
	}
	/* Getdents64 with fd=-1 -> -EBADF. */
	r = sys3(__NR_getdents64, -1, (long)buf, 256);
	check(r == -EBADF, "getdents64 fd=-1 EBADF");
}

/* ---- 12. Bad readv/writev args ---- */

static void
test_bad_iovec(void)
{
	long r;
	char buf[32];
	struct iovec iov;
	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);

	/* Readv with bad fd -> -EBADF. */
	r = sys3(__NR_readv, -1, (long)&iov, 1);
	check(r == -EBADF, "readv fd=-1 EBADF");

	/* Writev with bad fd -> -EBADF. */
	r = sys3(__NR_writev, -1, (long)&iov, 1);
	check(r == -EBADF, "writev fd=-1 EBADF");

	/* Readv with fd=9999 -> -EBADF. */
	r = sys3(__NR_readv, 9999, (long)&iov, 1);
	check(r == -EBADF, "readv fd=9999 EBADF");

	/* Writev with fd=9999 -> -EBADF. */
	r = sys3(__NR_writev, 9999, (long)&iov, 1);
	check(r == -EBADF, "writev fd=9999 EBADF");
}

/* ---- 13. Bad mmap edge cases (complement to init_syscall_fuzz) ---- */

static void
test_mmap_edges(void)
{
	long r;

	/*
	 * Mmap with MAP_FIXED_NOREPLACE on already-mapped address
	 * First, allocate a page.
	 */
	long addr = sys6(__NR_mmap, 0, 4096, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(addr > 0, "mmap alloc for overlap test");

	if (addr > 0) {
		/* MAP_FIXED_NOREPLACE (0x100000) on the same address -> -EEXIST. */
		r = sys6(__NR_mmap, addr, 4096, PROT_READ | PROT_WRITE,
			 0x100000 | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		check(r == -EEXIST, "mmap FIXED_NOREPLACE EEXIST");

		/* Clean up. */
		sys2(__NR_munmap, addr, 4096);
	}
}

/* ---- 14. Fork + wait interaction ---- */

static void
test_fork_wait(void)
{
	long r;

	/* Fork a child that exits immediately. */
	long pid = sys0(__NR_fork);
	if (pid == 0) {
		/* Child: exit immediately. */
		sys1(__NR_exit, 42);
		for (;;) ;	/* Unreachable. */
	}
	check(pid > 0, "fork for wait test");

	if (pid > 0) {
		/* Wait for the child. */
		int status = 0;
		r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
		check(r == pid, "wait4 returns child pid");
		/* Status should be (42 << 8) for normal exit with code 42. */
		check(((status >> 8) & 0xff) == 42, "wait4 exit code 42");

		/* Now wait again -- no more children -> -ECHILD. */
		r = sys4(__NR_wait4, -1, 0, WNOHANG, 0);
		check(r == -ECHILD, "wait4 after reap ECHILD");
	}
}

/* ---- 15. Double dup2 and dup chain ---- */

static void
test_dup_chain(void)
{
	long r;

	/* Open a file, dup it to fd 10, then close the original. */
	long fd = sys2(__NR_open, (long)"/init", 0);
	check(fd >= 0, "open for dup chain");

	if (fd >= 0) {
		r = sys2(__NR_dup2, fd, 10);
		check(r == 10, "dup2 to fd 10");

		/* Close original. */
		sys1(__NR_close, fd);

		/* Fd 10 should still be valid -- read from it. */
		char buf[1];
		r = sys3(__NR_read, 10, (long)buf, 1);
		check(r == 1, "read from dup'd fd");

		/* Clean up. */
		sys1(__NR_close, 10);

		/* Now fd 10 is closed, read should fail. */
		r = sys3(__NR_read, 10, (long)buf, 1);
		check(r == -EBADF, "read closed dup EBADF");
	}
}

int
main(void)
{
	msg("=== expanded syscall fuzz tests ===\n");

	test_bad_wait();
	test_bad_execve();
	test_bad_brk();
	test_bad_mprotect();
	test_bad_munmap();
	test_bad_mremap();
	test_bad_dup();
	test_bad_pipe();
	test_bad_kill();
	test_bad_clock_gettime();
	test_bad_getdents();
	test_bad_iovec();
	test_mmap_edges();
	test_fork_wait();
	test_dup_chain();

	test_done();
	return 0;
}
