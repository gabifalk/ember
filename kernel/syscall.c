/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/bug.h"
#include "ember/bkl.h"

static uint64_t
syscall_dispatch_inner(syscall_frame_t * f)
{
	switch (f->rax) {

		/* File I/O. */
	case SYS_READ:
	case SYS_WRITE:
	case SYS_CLOSE:
	case SYS_LSEEK:
	case SYS_WRITEV:
	case SYS_READV:
	case SYS_OPEN:
	case SYS_OPENAT:
	case SYS_FCNTL:
	case SYS_DUP:
	case SYS_IOCTL:
	case SYS_PREAD64:
	case SYS_PWRITE64:
	case SYS_FLOCK:
	case SYS_SENDFILE:
	case SYS_MEMFD_CREATE:
		return syscall_handle_file(f);

		/* FS path ops. */
	case SYS_FSTAT:
	case SYS_STAT:
	case SYS_LSTAT:
	case SYS_NEWFSTATAT:
	case SYS_ACCESS:
	case SYS_FACCESSAT:
	case SYS_FACCESSAT2:
	case SYS_GETCWD:
	case SYS_READLINK:
	case SYS_READLINKAT:
	case SYS_UNLINK:
	case SYS_RENAME:
	case SYS_TRUNCATE:
	case SYS_FTRUNCATE:
	case SYS_MKDIR:
	case SYS_MKDIRAT:
	case SYS_RMDIR:
	case SYS_CHDIR:
	case SYS_FCHDIR:
	case SYS_CHROOT:
	case SYS_LINK:
	case SYS_LINKAT:
	case SYS_SYMLINK:
	case SYS_SYMLINKAT:
	case SYS_MKNOD:
	case SYS_MKNODAT:
	case SYS_CHMOD:
	case SYS_FCHMOD:
	case SYS_FCHMODAT:
	case SYS_CHOWN:
	case SYS_FCHOWN:
	case SYS_LCHOWN:
	case SYS_FCHOWNAT:
	case SYS_GETDENTS64:
	case SYS_FSYNC:
	case SYS_MOUNT:
	case SYS_UMOUNT2:
	case SYS_STATFS:
	case SYS_FSTATFS:
	case SYS_UTIMENSAT:
	case SYS_UNLINKAT:
	case SYS_RENAMEAT:
	case SYS_RENAMEAT2:
		return syscall_handle_fs(f);

		/* Process management. */
	case SYS_FORK:
	case SYS_VFORK:
	case SYS_CLONE:
	case SYS_EXECVE:
	case SYS_EXECVEAT:
	case SYS_EXIT:
	case SYS_EXIT_GROUP:
	case SYS_WAIT4:
	case SYS_WAITID:
	case SYS_PIPE:
	case SYS_PIPE2:
	case SYS_DUP2:
	case SYS_DUP3:
	case SYS_REBOOT:
	case SYS_KEXEC_FILE_LOAD:
		return syscall_handle_proc(f);

		/* Memory management. */
	case SYS_BRK:
	case SYS_MMAP:
	case SYS_MPROTECT:
	case SYS_MUNMAP:
	case SYS_MREMAP:
		return syscall_handle_mm(f);

		/* Signals. */
	case SYS_RT_SIGPROCMASK:
	case SYS_SIGACTION:
	case SYS_RT_SIGRETURN:
	case SYS_KILL:
	case SYS_TKILL:
	case SYS_TGKILL:
	case SYS_RT_SIGSUSPEND:
	case SYS_SIGALTSTACK:
	case SYS_RT_SIGPENDING:
		return syscall_handle_sig(f);

		/* Misc. */
	case SYS_ARCH_PRCTL:
	case SYS_SET_TID_ADDRESS:
	case SYS_UNAME:
	case SYS_GETPID:
	case SYS_GETPPID:
	case SYS_GETTID:
	case SYS_GETUID:
	case SYS_GETGID:
	case SYS_GETEUID:
	case SYS_GETEGID:
	case SYS_UMASK:
	case SYS_SETPGID:
	case SYS_GETPGRP:
	case SYS_GETPGID:
	case SYS_SETSID:
	case SYS_CLOCK_GETTIME:
	case SYS_GETTIMEOFDAY:
	case SYS_TIME:
	case SYS_NANOSLEEP:
	case SYS_SET_ROBUST_LIST:
	case SYS_RSEQ:
	case SYS_PRLIMIT64:
	case SYS_GETRANDOM:
	case SYS_POLL:
	case SYS_SELECT:
	case SYS_FUTEX:
	case SYS_SCHED_YIELD:
	case SYS_ALARM:
	case SYS_SETITIMER:
	case SYS_GETITIMER:
	case SYS_GETRUSAGE:
	case SYS_PRCTL:
	case SYS_MADVISE:
	case SYS_PAUSE:
	case SYS_FDATASYNC:
	case SYS_TIMES:
	case SYS_GETRLIMIT:
	case SYS_SETRLIMIT:
	case SYS_PPOLL:
	case SYS_PSELECT6:
	case SYS_EPOLL_CREATE:
	case SYS_EPOLL_CREATE1:
	case SYS_EPOLL_CTL:
	case SYS_EPOLL_WAIT:
	case SYS_EPOLL_PWAIT:
	case SYS_FADVISE64:
	case SYS_STATX:
	case SYS_CLONE3:
	case SYS_SOCKET:
	case SYS_CONNECT:
	case SYS_SENDTO:
	case SYS_RECVFROM:
	case SYS_SOCKETPAIR:
	case SYS_CLOSE_RANGE:
	case SYS_SYSINFO:
	case SYS_GETSID:
	case SYS_PERSONALITY:
	case SYS_SYNC:
	case SYS_UTIME:
	case SYS_UTIMES:
	case SYS_SETUID:
	case SYS_SETGID:
	case SYS_SETREUID:
	case SYS_SETREGID:
	case SYS_SETFSUID:
	case SYS_SETFSGID:
	case SYS_GETGROUPS:
	case SYS_SETGROUPS:
	case SYS_SETHOSTNAME:
	case SYS_SETDOMAINNAME:
	case SYS_SETTIMEOFDAY:
	case SYS_SYSLOG:
	case SYS_IOPL:
	case SYS_IOPERM:
	case SYS_CLOCK_GETRES:
	case SYS_CLOCK_NANOSLEEP:
	case SYS_SCHED_GETAFFINITY:
	case SYS_SETRESUID:
	case SYS_GETRESUID:
	case SYS_SETRESGID:
	case SYS_GETRESGID:
	case SYS_MEMBARRIER:
	case SYS_MLOCK:
	case SYS_MUNLOCK:
	case SYS_MSYNC:
	case SYS_MINCORE:
	case SYS_READAHEAD:
	case SYS_UNSHARE:
		return syscall_handle_misc(f);

	default:
		console_write("unknown syscall: ");
		print_num(f->rax);
		f->rax = SYSCALL_ERR(ENOSYS);
		return f->rax;
	}
}

/* Debug: track last syscall for crash diagnostics. */
volatile uint64_t g_last_syscall_nr;
volatile uint64_t g_last_syscall_ret;
volatile uint64_t g_last_syscall_arg0;
volatile uint64_t g_last_syscall_arg1;
volatile uint64_t g_last_syscall_arg2;

uint64_t
syscall_dispatch(syscall_frame_t * f)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	/*
	 * Debug: the f != gs:0-152 check was a false positive.
	 * gs:0 can legitimately differ from f if schedule() updated gs:0
	 * after a timer preempt between sti and call syscall_dispatch.
	 * The process resumes with the original f but updated gs:0.
	 */
	uint64_t nr = f->rax;
	f->orig_rax = nr;
	g_last_syscall_nr = nr;
	g_last_syscall_arg0 = f->rdi;
	g_last_syscall_arg1 = f->rsi;
	g_last_syscall_arg2 = f->rdx;
	uint64_t ret = syscall_dispatch_inner(f);
	g_last_syscall_ret = f->rax;
	if (current_proc) {
		current_proc->last_sc = nr;
		current_proc->last_ret = (int64_t) f->rax;
	}
	/*
	 * Deliver pending signals before returning to user space.
	 * Skip for rt_sigreturn (avoid re-delivery on restored frame).
	 */
	if (nr != SYS_RT_SIGRETURN)
		signal_deliver(f);
	/*
	 * Syscall trace: check on every syscall return (BKL held).
	 * trace_dump is lightweight -- just compares kernel_ticks to a threshold.
	 * On SMP the timer_handler path rarely fires (user-mode timers are rare
	 * under heavy BKL contention), so this is the reliable trace path.
	 */
	{
		extern void trace_dump(void);
		trace_dump();
	}
	return f->rax;
}
