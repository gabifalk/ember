/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_SYSCALL_H
#define EMBER_SYSCALL_H

#include <stdint.h>

#define SYS_READ            0
#define SYS_WRITE           1
#define SYS_OPEN            2
#define SYS_CLOSE           3
#define SYS_FSTAT           5
#define SYS_LSEEK           8
#define SYS_MMAP            9
#define SYS_MPROTECT       10
#define SYS_MUNMAP         11
#define SYS_BRK            12
#define SYS_IOCTL          16
#define SYS_PIPE           22
#define SYS_WRITEV         20
#define SYS_DUP2           33
#define SYS_SENDFILE       40
#define SYS_GETPID         39
#define SYS_CLONE          56
#define SYS_FORK           57
#define SYS_EXECVE         59
#define SYS_EXIT           60
#define SYS_WAIT4          61
#define SYS_GETPPID       110
#define SYS_RT_SIGPROCMASK 14
#define SYS_ARCH_PRCTL    158
#define SYS_GETTID        186
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP    231
#define SYS_OPENAT        257
#define SYS_FSYNC          74
#define SYS_TRUNCATE       76
#define SYS_FTRUNCATE      77
#define SYS_MKDIR          83
#define SYS_RMDIR          84
#define SYS_RENAME         82
#define SYS_UNLINK         87
#define SYS_REBOOT        169
#define SYS_NEWFSTATAT    262
#define SYS_FCNTL          72
#define SYS_GETCWD         79
#define SYS_READLINK       89
#define SYS_GETUID        102
#define SYS_GETGID        104
#define SYS_GETEUID       107
#define SYS_GETEGID       108
#define SYS_SIGACTION      13
#define SYS_CLOCK_GETTIME 228
#define SYS_READLINKAT    267
#define SYS_SET_ROBUST_LIST 273
#define SYS_GETDENTS64    217
#define SYS_MKDIRAT       258
#define SYS_DUP3          292
#define SYS_PIPE2         293
#define SYS_PRLIMIT64     302
#define SYS_GETRANDOM     318
#define SYS_RT_SIGRETURN   15
#define SYS_RSEQ          334

/* New syscalls for live-bootstrap. */
#define SYS_STAT            4
#define SYS_LSTAT           6
#define SYS_POLL            7
#define SYS_ACCESS         21
#define SYS_SELECT         23
#define SYS_DUP            32
#define SYS_NANOSLEEP      35
#define SYS_KILL           62
#define SYS_UNAME          63
#define SYS_CHDIR          80
#define SYS_FCHDIR         81
#define SYS_LINK           86
#define SYS_SYMLINK        88
#define SYS_CHMOD          90
#define SYS_FCHMOD         91
#define SYS_CHOWN          92
#define SYS_FCHOWN         93
#define SYS_LCHOWN         94
#define SYS_UMASK          95
#define SYS_GETTIMEOFDAY   96
#define SYS_SETPGID       109
#define SYS_GETPGRP       111
#define SYS_SETSID        112
#define SYS_GETPGID       121
#define SYS_TIME          201
#define SYS_FCHOWNAT      260
#define SYS_LINKAT        265
#define SYS_SYMLINKAT     266
#define SYS_FCHMODAT      268
#define SYS_FACCESSAT     269
#define SYS_FACCESSAT2    439
#define SYS_CHROOT        161
#define SYS_MKNOD         133
#define SYS_MOUNT         165
#define SYS_UMOUNT2       166
#define SYS_MKNODAT       259
#define SYS_STATFS        137
#define SYS_FSTATFS       138
#define SYS_UTIMENSAT     280
#define SYS_EXECVEAT      322
#define SYS_KEXEC_FILE_LOAD 320
#define SYS_MEMFD_CREATE 319

/* New syscalls for build tool support. */
#define SYS_PREAD64       17
#define SYS_PWRITE64      18
#define SYS_SCHED_YIELD   24
#define SYS_GETITIMER     36
#define SYS_ALARM         37
#define SYS_SETITIMER     38
#define SYS_GETRUSAGE     98
#define SYS_RT_SIGSUSPEND 130
#define SYS_SIGALTSTACK   131
#define SYS_TKILL         200
#define SYS_FUTEX         202
#define SYS_TGKILL        234

/* Batch 2 syscalls. */
#define SYS_READV          19
#define SYS_UNLINKAT      263
#define SYS_RENAMEAT      264
#define SYS_RENAMEAT2     316
#define SYS_PRCTL         157

/* Batch 3 syscalls. */
#define SYS_MADVISE        28
#define SYS_PAUSE          34
#define SYS_FDATASYNC      75
#define SYS_TIMES         100
#define SYS_MREMAP         25

/* Batch 4 syscalls. */
#define SYS_SOCKET         41
#define SYS_CONNECT        42
#define SYS_SENDTO         44
#define SYS_RECVFROM       45
#define SYS_SOCKETPAIR     53
#define SYS_VFORK          58
#define SYS_GETRLIMIT      97
#define SYS_SYSINFO        99
#define SYS_RT_SIGPENDING 127
#define SYS_SETRLIMIT     160
#define SYS_EPOLL_CREATE  213
#define SYS_FADVISE64     221
#define SYS_EPOLL_WAIT    232
#define SYS_EPOLL_CTL     233
#define SYS_PSELECT6      270
#define SYS_PPOLL         271
#define SYS_EPOLL_PWAIT   281
#define SYS_EPOLL_CREATE1 291
#define SYS_STATX         332
#define SYS_CLONE3        435
#define SYS_CLOSE_RANGE   436

/* Fiwix-parity syscalls. */
#define SYS_FLOCK          73
#define SYS_GETSID        124
#define SYS_PERSONALITY   135
#define SYS_SYNC          162
#define SYS_UTIME         132
#define SYS_UTIMES        235

/* Fiwix-parity: credentials, hostname, misc. */
#define SYS_SYSLOG        103
#define SYS_SETUID        105
#define SYS_SETGID        106
#define SYS_SETREUID      113
#define SYS_SETREGID      114
#define SYS_GETGROUPS     115
#define SYS_SETGROUPS     116
#define SYS_SETFSUID      122
#define SYS_SETFSGID      123
#define SYS_SETTIMEOFDAY  164
#define SYS_SETHOSTNAME   170
#define SYS_SETDOMAINNAME 171
#define SYS_IOPL          172
#define SYS_IOPERM        173

/* Glibc support syscalls. */
#define SYS_MSYNC          26
#define SYS_MINCORE        27
#define SYS_SETRESUID     117
#define SYS_GETRESUID     118
#define SYS_SETRESGID     119
#define SYS_GETRESGID     120
#define SYS_MLOCK         149
#define SYS_MUNLOCK       150
#define SYS_READAHEAD     187
#define SYS_SCHED_GETAFFINITY 204
#define SYS_CLOCK_GETRES  229
#define SYS_CLOCK_NANOSLEEP 230
#define SYS_WAITID        247
#define SYS_UNSHARE       272
#define SYS_MEMBARRIER    324

/* Rlimit constants. */
#define RLIMIT_STACK     3
#define RLIMIT_NOFILE    7
#define RLIM_INFINITY    (~0ULL)

/* Epoll constants. */
#define EPOLL_CTL_ADD  1
#define EPOLL_CTL_DEL  2
#define EPOLL_CTL_MOD  3

/* Fcntl commands. */
#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4
#define F_DUPFD_CLOEXEC 1030

/* Reboot(2) magic values. */
#define LINUX_REBOOT_MAGIC1         0xfee1dead
#define LINUX_REBOOT_MAGIC2         672274793
#define LINUX_REBOOT_CMD_POWER_OFF  0x4321fedc
#define LINUX_REBOOT_CMD_KEXEC      0x45584543

/* kexec_file_load flags. */
#define KEXEC_FILE_NO_INITRD        0x04

#define EPERM    1
#define ENOENT   2
#define EIO      5
#define EBADF    9
#define ENOMEM  12
#define EEXIST  17
#define EISDIR  21
#define EAGAIN  11
#define ECHILD  10
#define EFAULT  14
#define ENOEXEC  8
#define EPIPE   32
#define ESRCH    3
#define EINVAL  22
#define EMFILE  24
#define ENOTTY  25
#define ENOSPC  28
#define ERANGE  34
#define ENOTDIR 20
#define ENOTEMPTY 39
#define ENOSYS  38
#define ELOOP   40
#define EACCES  13
#define EINTR    4
#define E2BIG    7
#define ESPIPE  29
#define ETIMEDOUT 110
#define ENAMETOOLONG 36
#define ENODEV  19

#define ARCH_GET_FS  0x1003

#define AT_FDCWD  (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000

/* Utimensat special values. */
#define UTIME_NOW   0x3fffffff
#define UTIME_OMIT  0x3ffffffe

/* O_* flags. */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_NOFOLLOW  0400000
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

/* S_IF* mode constants. */
#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFLNK  0120000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000

/* Permission masks. */
#define S_ALLPERMS    07777	/* Setuid|setgid|sticky + rwxrwxrwx. */
#define S_ACCESSPERMS 0777	/* Rwxrwxrwx. */

/* Lseek whence. */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Waitpid options. */
#define WNOHANG      1
#define WUNTRACED    2
#define WCONTINUED   8
#define WEXITED      4
#define WNOWAIT      0x01000000

/* Waitid idtype. */
#define P_ALL  0
#define P_PID  1
#define P_PGID 2

/* Mmap flags. */
#define MAP_ANONYMOUS       0x20
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED          ((void *)(uint64_t)-1)
#define PROT_NONE           0x0
#define PROT_READ           0x1
#define PROT_WRITE          0x2
#define PROT_EXEC           0x4

/* Mremap flags. */
#define MREMAP_MAYMOVE  1

/* Clone flags. */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

typedef struct syscall_frame {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
	uint64_t rip, rflags, rsp;
	uint64_t orig_rax;	/* Original syscall number for SA_RESTART. */
} syscall_frame_t;

void syscall_init(void);
void syscall_init_ap(int cpu_id, uint64_t kstack_top);
uint64_t syscall_dispatch(syscall_frame_t * f);
void syscall_set_user_cr3(uint64_t cr3);
uint64_t cpu0_user_cr3(void);
void syscall_set_kstack(uint64_t kstack_top);
uint64_t cpu0_kstack_top(void);

#endif
