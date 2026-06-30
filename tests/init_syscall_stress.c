/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * Syscall stress/coverage test -- exercises every major implemented syscall
 * at least once to catch crashes, stack overflows, and incorrect error returns.
 * Runs on cpio initrd (read-only), so fs-write tests are skipped.
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include "test_common.h"

/* Syscall numbers (Linux x86_64 ABI) */
#define __NR_read           0
#define __NR_write          1
#define __NR_open           2
#define __NR_close          3
#define __NR_stat           4
#define __NR_fstat          5
#define __NR_lstat          6
#define __NR_lseek          8
#define __NR_mmap           9
#define __NR_mprotect       10
#define __NR_munmap         11
#define __NR_brk            12
#define __NR_rt_sigaction   13
#define __NR_rt_sigprocmask 14
#define __NR_ioctl          16
#define __NR_access         21
#define __NR_pipe           22
#define __NR_dup            32
#define __NR_dup2           33
#define __NR_getpid         39
#define __NR_fork           57
#define __NR_execve         59
#define __NR_exit           60
#define __NR_wait4          61
#define __NR_kill           62
#define __NR_uname          63
#define __NR_fcntl          72
#define __NR_getcwd         79
#define __NR_chdir          80
#define __NR_mkdir          83
#define __NR_rmdir          84
#define __NR_unlink         87
#define __NR_symlink        88
#define __NR_readlink       89
#define __NR_chmod          90
#define __NR_rename         82
#define __NR_getuid         102
#define __NR_getgid         104
#define __NR_geteuid        107
#define __NR_getegid        108
#define __NR_getppid        110
#define __NR_setsid         112
#define __NR_getpgid        121
#define __NR_gettid         186
#define __NR_clock_gettime  228
#define __NR_getdents64     217
#define __NR_set_tid_address 218
#define __NR_arch_prctl     158
#define __NR_umask          95
#define __NR_sched_yield    24
#define __NR_nanosleep      35
#define __NR_getrandom      318
#define __NR_madvise        28
#define __NR_pipe2          293
#define __NR_prlimit64      302

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

/* ---- File ops ---- */

static void
test_open_close(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0 /* O_RDONLY. */ );
	check(fd >= 0, "open /init");
	long r = sys1(__NR_close, fd);
	check(r == 0, "close");
}

static void
test_open_enoent(void)
{
	long fd = sys2(__NR_open, (long)"/nonexistent_file", 0);
	check(fd < 0, "open ENOENT");
}

static void
test_read_write(void)
{
	/* Write to stdout. */
	long r = sys3(__NR_write, 1, (long)"    (write ok)\n", 15);
	check(r == 15, "write stdout");

	/* Read from a file. */
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "read (open)");
		return;
	}
	char buf[16];
	r = sys3(__NR_read, fd, (long)buf, 4);
	check(r == 4, "read");
	/* ELF magic: 0x7f 'E' 'L' 'F'. */
	check(buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F',
	      "read ELF magic");
	sys1(__NR_close, fd);
}

static void
test_dup(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "dup (open)");
		return;
	}
	long fd2 = sys1(__NR_dup, fd);
	check(fd2 >= 0 && fd2 != fd, "dup");
	sys1(__NR_close, fd2);
	sys1(__NR_close, fd);
}

static void
test_dup2(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "dup2 (open)");
		return;
	}
	/* Dup2 to a specific fd number (pick 50 which should be free) */
	long r = sys2(__NR_dup2, fd, 50);
	check(r == 50, "dup2");
	sys1(__NR_close, 50);
	sys1(__NR_close, fd);
}

static void
test_lseek(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "lseek (open)");
		return;
	}
	long pos = sys3(__NR_lseek, fd, 10, 0 /* SEEK_SET. */ );
	check(pos == 10, "lseek SET");
	pos = sys3(__NR_lseek, fd, 5, 1 /* SEEK_CUR. */ );
	check(pos == 15, "lseek CUR");
	pos = sys3(__NR_lseek, fd, 0, 2 /* SEEK_END. */ );
	check(pos > 0, "lseek END");
	sys1(__NR_close, fd);
}

static void
test_fstat(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "fstat (open)");
		return;
	}
	struct stat st;
	memset(&st, 0, sizeof(st));
	long r = sys2(__NR_fstat, fd, (long)&st);
	check(r == 0, "fstat ret");
	check(st.st_size > 0, "fstat size");
	sys1(__NR_close, fd);
}

static void
test_ioctl_bad(void)
{
	/* Ioctl on a regular file should fail with ENOTTY or similar. */
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "ioctl (open)");
		return;
	}
	long r = sys3(__NR_ioctl, fd, 0x5401 /* TCGETS. */ , 0);
	/* Not a tty, so should return error. */
	check(r < 0, "ioctl ENOTTY");
	sys1(__NR_close, fd);
}

static void
test_fcntl(void)
{
	long fd = sys2(__NR_open, (long)"/init", 0);
	if (fd < 0) {
		check(0, "fcntl (open)");
		return;
	}
	/* F_GETFD = 1. */
	long r = sys2(__NR_fcntl, fd, 1);
	check(r >= 0, "fcntl F_GETFD");
	sys1(__NR_close, fd);
}

static void
test_pipe(void)
{
	int pipefd[2];
	long r = sys1(__NR_pipe, (long)pipefd);
	check(r == 0, "pipe");
	if (r == 0) {
		r = sys3(__NR_write, pipefd[1], (long)"AB", 2);
		check(r == 2, "pipe write");
		char buf[4] = { 0 };
		r = sys3(__NR_read, pipefd[0], (long)buf, 2);
		check(r == 2 && buf[0] == 'A' && buf[1] == 'B', "pipe read");
		sys1(__NR_close, pipefd[0]);
		sys1(__NR_close, pipefd[1]);
	}
}

static void
test_pipe2(void)
{
	int pipefd[2];
	long r = sys2(__NR_pipe2, (long)pipefd, 0);
	check(r == 0, "pipe2");
	if (r == 0) {
		sys1(__NR_close, pipefd[0]);
		sys1(__NR_close, pipefd[1]);
	}
}

/* ---- FS path ops ---- */

static void
test_stat(void)
{
	struct stat st;
	memset(&st, 0, sizeof(st));
	long r = sys2(__NR_stat, (long)"/init", (long)&st);
	check(r == 0, "stat /init");
	check(st.st_size > 0, "stat size");

	/* Stat nonexistent. */
	r = sys2(__NR_stat, (long)"/no_such_file", (long)&st);
	check(r < 0, "stat ENOENT");
}

static void
test_lstat(void)
{
	struct stat st;
	memset(&st, 0, sizeof(st));
	long r = sys2(__NR_lstat, (long)"/init", (long)&st);
	check(r == 0, "lstat /init");
}

static void
test_access(void)
{
	long r = sys2(__NR_access, (long)"/init", 0 /* F_OK. */ );
	check(r == 0, "access F_OK");
	r = sys2(__NR_access, (long)"/no_such_file", 0);
	check(r < 0, "access ENOENT");
}

static void
test_getcwd(void)
{
	char buf[256];
	memset(buf, 0, sizeof(buf));
	long r = sys2(__NR_getcwd, (long)buf, 256);
	check(r > 0, "getcwd ret");
	check(buf[0] == '/', "getcwd starts with /");
}

static void
test_getdents64(void)
{
	long fd = sys2(__NR_open, (long)"/", 0);
	if (fd < 0) {
		check(0, "getdents64 (open)");
		return;
	}
	char buf[512];
	long n = sys3(__NR_getdents64, fd, (long)buf, 512);
	check(n > 0, "getdents64");
	sys1(__NR_close, fd);
}

static void
test_chdir(void)
{
	long r = sys1(__NR_chdir, (long)"/");
	check(r == 0, "chdir /");
	/* Verify via getcwd. */
	char buf[256];
	memset(buf, 0, sizeof(buf));
	sys2(__NR_getcwd, (long)buf, 256);
	check(buf[0] == '/' && buf[1] == '\0', "chdir verify");
}

/* ---- Proc ops ---- */

static void
test_getpid(void)
{
	long pid = sys0(__NR_getpid);
	check(pid > 0, "getpid");
}

static void
test_getppid(void)
{
	long ppid = sys0(__NR_getppid);
	check(ppid >= 0, "getppid");
}

static void
test_gettid(void)
{
	long tid = sys0(__NR_gettid);
	check(tid > 0, "gettid");
}

static void
test_fork_wait(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "fork");
		return;
	}
	if (pid == 0) {
		/* Child exits with code 7. */
		sys1(__NR_exit, 7);
		__builtin_unreachable();
	}
	/* Parent waits. */
	int status = 0;
	long r = sys4(__NR_wait4, pid, (long)&status, 0, 0);
	check(r == pid, "wait4 ret");
	int code = (status >> 8) & 0xff;
	check(code == 7, "wait4 status");
}

static void
test_fork_exec(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "fork+exec");
		return;
	}
	if (pid == 0) {
		char *argv[] = { "/hello", 0 };
		char *envp[] = { 0 };
		sys3(__NR_execve, (long)"/hello", (long)argv, (long)envp);
		sys1(__NR_exit, 99);
		__builtin_unreachable();
	}
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "fork+exec");
}

static void
test_setsid(void)
{
	long pid = sys0(__NR_fork);
	if (pid < 0) {
		check(0, "setsid");
		return;
	}
	if (pid == 0) {
		long r = sys0(__NR_setsid);
		/* Child becomes session leader, setsid returns new sid == pid. */
		sys1(__NR_exit, r > 0 ? 0 : 1);
		__builtin_unreachable();
	}
	int status = 0;
	sys4(__NR_wait4, pid, (long)&status, 0, 0);
	int code = (status >> 8) & 0xff;
	check(code == 0, "setsid");
}

/* ---- MM ops ---- */

static void
test_brk(void)
{
	long cur = sys1(__NR_brk, 0);
	check(cur > 0, "brk query");
	/* Extend brk by one page. */
	long new_brk = sys1(__NR_brk, cur + 4096);
	check(new_brk >= cur + 4096, "brk extend");
	/* Write to the new page to ensure it's mapped. */
	*(volatile char *)cur = 0x42;
	check(*(volatile char *)cur == 0x42, "brk write");
	/* Restore. */
	sys1(__NR_brk, cur);
}

static void
test_mmap_munmap(void)
{
	/* MAP_ANONYMOUS=0x20, MAP_PRIVATE=0x02, PROT_READ=1, PROT_WRITE=2. */
	long addr =
	    sys6(__NR_mmap, 0, 4096, 3 /* RW. */ , 0x22 /* PRIVATE|ANON */ , -1,
		 0);
	check(addr > 0 && (addr & 0xfff) == 0, "mmap anon");
	if (addr > 0) {
		/* Write and read back. */
		*(volatile int *)addr = 0xDEAD;
		check(*(volatile int *)addr == 0xDEAD, "mmap write");
		long r = sys2(__NR_munmap, addr, 4096);
		check(r == 0, "munmap");
	}
}

static void
test_mprotect(void)
{
	long addr = sys6(__NR_mmap, 0, 4096, 3 /* RW. */ , 0x22, -1, 0);
	if (addr <= 0) {
		check(0, "mprotect (mmap)");
		return;
	}
	/* Change to read-only (PROT_READ=1) */
	long r = sys3(__NR_mprotect, addr, 4096, 1);
	check(r == 0, "mprotect RO");
	/* Change back to RW. */
	r = sys3(__NR_mprotect, addr, 4096, 3);
	check(r == 0, "mprotect RW");
	sys2(__NR_munmap, addr, 4096);
}

static void
test_madvise(void)
{
	long addr = sys6(__NR_mmap, 0, 4096, 3, 0x22, -1, 0);
	if (addr <= 0) {
		check(0, "madvise (mmap)");
		return;
	}
	/* MADV_DONTNEED = 4. */
	long r = sys3(__NR_madvise, addr, 4096, 4);
	check(r == 0, "madvise");
	sys2(__NR_munmap, addr, 4096);
}

/* ---- Signal ops ---- */

static volatile int sig_received;

static void
sig_handler(int sig)
{
	(void)sig;
	sig_received = 1;
}

static void
test_sigaction(void)
{
	/* Struct kernel_sigaction (Linux x86_64) */
	struct {
		void (*handler) (int);
		unsigned long flags;
		void (*restorer) (void);
		unsigned long mask;
	} sa, old_sa;

	memset(&sa, 0, sizeof(sa));
	sa.handler = sig_handler;
	sa.flags = 0;

	/* rt_sigaction(SIGUSR1=10, &sa, &old_sa, sigsetsize=8) */
	long r = sys4(__NR_rt_sigaction, 10, (long)&sa, (long)&old_sa, 8);
	check(r == 0, "rt_sigaction");
}

static void
test_sigprocmask(void)
{
	unsigned long set = 0, old = 0;
	/* SIG_BLOCK=0, get current mask. */
	long r = sys4(__NR_rt_sigprocmask, 0, (long)&set, (long)&old, 8);
	check(r == 0, "rt_sigprocmask");
}

static void
test_kill(void)
{
	/* Set up handler for SIGUSR1 first. */
	struct {
		void (*handler) (int);
		unsigned long flags;
		void (*restorer) (void);
		unsigned long mask;
	} sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = sig_handler;
	sys4(__NR_rt_sigaction, 10 /* SIGUSR1. */ , (long)&sa, 0, 8);

	sig_received = 0;
	long pid = sys0(__NR_getpid);
	long r = sys2(__NR_kill, pid, 10 /* SIGUSR1. */ );
	check(r == 0, "kill ret");
	check(sig_received == 1, "kill delivered");
}

/* ---- Misc ops ---- */

static void
test_uname(void)
{
	struct {
		char sysname[65];
		char nodename[65];
		char release[65];
		char version[65];
		char machine[65];
		char domainname[65];
	} buf;
	memset(&buf, 0, sizeof(buf));
	long r = sys1(__NR_uname, (long)&buf);
	check(r == 0, "uname");
	check(buf.sysname[0] != '\0', "uname sysname");
}

static void
test_clock_gettime(void)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts;
	memset(&ts, 0, sizeof(ts));
	/* CLOCK_MONOTONIC = 1. */
	long r = sys2(__NR_clock_gettime, 1, (long)&ts);
	check(r == 0, "clock_gettime");
}

static void
test_getuid_getgid(void)
{
	long uid = sys0(__NR_getuid);
	check(uid >= 0, "getuid");
	long gid = sys0(__NR_getgid);
	check(gid >= 0, "getgid");
	long euid = sys0(__NR_geteuid);
	check(euid >= 0, "geteuid");
	long egid = sys0(__NR_getegid);
	check(egid >= 0, "getegid");
}

static void
test_umask(void)
{
	long old = sys1(__NR_umask, 0022);
	check(old >= 0, "umask set");
	long cur = sys1(__NR_umask, old);	/* Restore. */
	check(cur == 0022, "umask get");
}

static void
test_sched_yield(void)
{
	long r = sys0(__NR_sched_yield);
	check(r == 0, "sched_yield");
}

static void
test_nanosleep(void)
{
	struct {
		long tv_sec;
		long tv_nsec;
	} ts = {
	0, 1000000};		/* 1Ms. */
	long r = sys2(__NR_nanosleep, (long)&ts, 0);
	check(r == 0, "nanosleep");
}

static void
test_getrandom(void)
{
	char buf[8];
	memset(buf, 0, sizeof(buf));
	/* Getrandom(buf, 8, 0) */
	long r = sys3(__NR_getrandom, (long)buf, 8, 0);
	check(r == 8, "getrandom");
}

static void
test_set_tid_address(void)
{
	int tidptr = 0;
	long r = sys1(__NR_set_tid_address, (long)&tidptr);
	check(r > 0, "set_tid_address");
}

static void
test_arch_prctl(void)
{
	unsigned long val = 0;
	/* ARCH_GET_FS = 0x1003. */
	long r = sys2(__NR_arch_prctl, 0x1003, (long)&val);
	check(r == 0, "arch_prctl GET_FS");
}

static void
test_prlimit64(void)
{
	struct {
		unsigned long rlim_cur;
		unsigned long rlim_max;
	} lim;
	memset(&lim, 0, sizeof(lim));
	/* Prlimit64(0, RLIMIT_STACK=3, NULL, &lim) */
	long r = sys4(__NR_prlimit64, 0, 3, 0, (long)&lim);
	check(r == 0, "prlimit64");
}

static void
test_getpgid(void)
{
	long pgid = sys1(__NR_getpgid, 0);
	check(pgid >= 0, "getpgid");
}

/* ---- FS write ops (skipped on cpio) ---- */

static void
test_fs_write_ops(void)
{
	/* Try to create a file -- this will fail on cpio (read-only) */
	long fd =
	    sys3(__NR_open, (long)"/test_creat", 0x41 /* O_WRONLY|O_CREAT. */ ,
		 0644);
	if (fd < 0) {
		/* Read-only FS, skip write tests. */
		msg("  SKIP mkdir/rmdir/unlink/rename/symlink/readlink/chmod (cpio ro)\n");
		return;
	}
	sys1(__NR_close, fd);

	/* Mkdir. */
	long r = sys2(__NR_mkdir, (long)"/stressdir", 0755);
	check(r == 0, "mkdir");

	/* Symlink. */
	r = sys2(__NR_symlink, (long)"/test_creat", (long)"/test_link");
	check(r == 0, "symlink");

	/* Readlink. */
	char lbuf[256];
	r = sys3(__NR_readlink, (long)"/test_link", (long)lbuf, 256);
	check(r > 0, "readlink");

	/* Chmod. */
	r = sys2(__NR_chmod, (long)"/test_creat", 0600);
	check(r == 0, "chmod");

	/* Rename. */
	r = sys2(__NR_rename, (long)"/test_creat", (long)"/test_renamed");
	check(r == 0, "rename");

	/* Unlink. */
	r = sys1(__NR_unlink, (long)"/test_renamed");
	check(r == 0, "unlink");
	r = sys1(__NR_unlink, (long)"/test_link");
	check(r == 0, "unlink symlink");

	/* Rmdir. */
	r = sys1(__NR_rmdir, (long)"/stressdir");
	check(r == 0, "rmdir");
}

int
main(void)
{
	msg("=== syscall stress test ===\n");

	/* File ops. */
	test_open_close();
	test_open_enoent();
	test_read_write();
	test_dup();
	test_dup2();
	test_lseek();
	test_fstat();
	test_ioctl_bad();
	test_fcntl();
	test_pipe();
	test_pipe2();

	/* FS path ops. */
	test_stat();
	test_lstat();
	test_access();
	test_getcwd();
	test_getdents64();
	test_chdir();

	/* Proc ops. */
	test_getpid();
	test_getppid();
	test_gettid();
	test_fork_wait();
	test_fork_exec();
	test_setsid();

	/* MM ops. */
	test_brk();
	test_mmap_munmap();
	test_mprotect();
	test_madvise();

	/* Signal ops. */
	test_sigaction();
	test_sigprocmask();
	test_kill();

	/* Misc ops. */
	test_uname();
	test_clock_gettime();
	test_getuid_getgid();
	test_umask();
	test_sched_yield();
	test_nanosleep();
	test_getrandom();
	test_set_tid_address();
	test_arch_prctl();
	test_prlimit64();
	test_getpgid();

	/* FS write ops (ext2 only, skipped on cpio) */
	test_fs_write_ops();

	test_done();
	return 0;
}
