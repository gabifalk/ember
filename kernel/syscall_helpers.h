#ifndef KERNEL_SYSCALL_HELPERS_H
#define KERNEL_SYSCALL_HELPERS_H

#include <stdint.h>
#include "ember/syscall.h"
#include "ember/console.h"
#include "ember/paging.h"
#include "ember/user.h"
#include "ember/pmm.h"
#include "ember/mmu.h"
#include "ember/fd.h"
#include "ember/vfs.h"
#include "ember/proc.h"
#include "ember/signal.h"
#include "ember/pipe.h"
#include "ember/elf.h"
#include "ember/heap.h"
#include "ember/klib.h"
#include "ember/ext2.h"
#include "ember/blkdev.h"
#include "ember/blkcache.h"
#include "ember/sched.h"
#include "ember/kexec.h"
#include "ember/time.h"
#include "ember/acpi.h"

/* Convert a positive errno to the uint64_t negative value returned in rax. */
#define SYSCALL_ERR(e) ((uint64_t)(-(e)))

#define IA32_FS_BASE 0xC0000100u
#define ARCH_SET_FS  0x1002

/* Serial control characters. */
#define CTRL_C          0x03	/* ETX -- interrupt (SIGINT) */
#define CTRL_D          0x04	/* EOT -- end of file. */

/* Terminal ioctl request codes. */
#define TCGETS          0x5401
#define TCSETS          0x5402
#define TCSETSW         0x5403
#define TCSETSF         0x5404
#define TIOCGPGRP       0x540F
#define TIOCSPGRP       0x5410
#define TIOCGWINSZ      0x5413
#define FIONREAD        0x541B

/* Termios c_lflag bits. */
#define TERMIOS_ICANON  0x02
#define TERMIOS_ECHO    0x08

/* Default terminal dimensions. */
#define TERM_ROWS       24
#define TERM_COLS       80

/* Struct utsname field size and offsets. */
#define UTSNAME_FIELD_LEN  65
#define UTSNAME_SIZE       (6 * UTSNAME_FIELD_LEN)	/* 390 Bytes. */

struct iovec {
	uint64_t iov_base;
	uint64_t iov_len;
};

/* Linux x86_64 struct stat layout (144 bytes) */
struct linux_stat {
	uint64_t st_dev;	/* 0. */
	uint64_t st_ino;	/* 8. */
	uint64_t st_nlink;	/* 16. */
	uint32_t st_mode;	/* 24. */
	uint32_t st_uid;	/* 28. */
	uint32_t st_gid;	/* 32. */
	uint32_t __pad0;	/* 36. */
	uint64_t st_rdev;	/* 40. */
	int64_t st_size;	/* 48. */
	int64_t st_blksize;	/* 56. */
	int64_t st_blocks;	/* 64. */
	uint64_t st_atime_sec;	/* 72. */
	uint64_t st_atime_nsec;	/* 80. */
	uint64_t st_mtime_sec;	/* 88. */
	uint64_t st_mtime_nsec;	/* 96. */
	uint64_t st_ctime_sec;	/* 104. */
	uint64_t st_ctime_nsec;	/* 112. */
	int64_t __unused[3];	/* 120. */
};				/* 144 Total. */

/*
 * Validate an fd and return its entry, or set EBADF in the frame.
 * Callers check: fd_entry_t *e = syscall_fd_get(fd, f); if (!e) return f->rax;
 */
static inline fd_entry_t *
syscall_fd_get(int fd, syscall_frame_t * f)
{
	fd_entry_t *e = fd_get(fd);
	if (!e)
		f->rax = SYSCALL_ERR(EBADF);
	return e;
}

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
	uint32_t lo = (uint32_t) val;
	uint32_t hi = (uint32_t) (val >> 32);
	__asm__ __volatile__("wrmsr"::"c"(msr), "a"(lo), "d"(hi));
}

/*
 * Switch to user address space for user memory access.
 * Declares local variables _ua_old and _ua_ucr3.
 */
#define USER_ACCESS_BEGIN() \
    uint64_t _ua_old = read_cr3(); \
    uint64_t _ua_ucr3 = get_user_cr3(); \
    if (_ua_ucr3) write_cr3(_ua_ucr3)

/* Restore kernel address space after user memory access. */
#define USER_ACCESS_END() \
    if (_ua_ucr3) write_cr3(_ua_old)

/* Minimal termios state -- tracks ECHO and ICANON from c_lflag. */
extern volatile int console_echo;
extern volatile int console_icanon;
extern volatile int console_fg_pgid;

/* Helper function declarations (implementations in syscall_helpers.c) */
void console_signal_fg(int sig);
void write_user_buf(const char *buf, uint64_t len);
int copy_path_from_user(const char *user_ptr, char *kbuf, uint64_t kbuf_size);
uint64_t get_user_cr3(void);
void fill_stat(struct linux_stat *st, file_desc_t * entry);
void fill_stat_from_node(struct linux_stat *st, vfs_node_t * node);
void normalize_path(char *path);
void resolve_path(const char *path, char *kbuf, uint64_t kbuf_size);
int is_devnull(const char *path);
void fill_devnull_stat(struct linux_stat *st);
int is_devzero(const char *path);
void fill_devzero_stat(struct linux_stat *st);
int is_devrandom(const char *path);
void fill_devrandom_stat(struct linux_stat *st);
int is_devtty(const char *path);
void fill_devtty_stat(struct linux_stat *st);
int is_devconsole(const char *path);
int is_proc_self_exe(const char *path);
int is_proc_iomem(const char *path);
void fill_chardev_stat(struct linux_stat *st, uint64_t rdev, uint64_t ino,
		       uint32_t mode_bits);
void print_num(uint64_t n);

/* Handler prototypes for category files. */
uint64_t syscall_handle_file(syscall_frame_t * f);
uint64_t syscall_handle_fs(syscall_frame_t * f);

/* syscall_file sub-file prototypes (syscall_file_io.c) */
uint64_t sys_read(syscall_frame_t * f);
uint64_t sys_write(syscall_frame_t * f);
uint64_t sys_writev(syscall_frame_t * f);
uint64_t sys_readv(syscall_frame_t * f);
uint64_t sys_pread64(syscall_frame_t * f);
uint64_t sys_pwrite64(syscall_frame_t * f);
uint64_t sys_sendfile(syscall_frame_t * f);

/* syscall_file sub-file prototypes (syscall_file_ops.c) */
uint64_t sys_lseek(syscall_frame_t * f);
uint64_t sys_ioctl(syscall_frame_t * f);
uint64_t sys_fcntl(syscall_frame_t * f);
uint64_t sys_dup(syscall_frame_t * f);

/* syscall_fs sub-file prototypes (syscall_fs_stat.c) */
uint64_t sys_fstat(syscall_frame_t * f);
uint64_t sys_stat(syscall_frame_t * f);
uint64_t sys_lstat(syscall_frame_t * f);
uint64_t sys_newfstatat(syscall_frame_t * f);
uint64_t sys_statfs(syscall_frame_t * f);
uint64_t sys_fstatfs(syscall_frame_t * f);

/* syscall_fs sub-file prototypes (syscall_fs_path.c) */
uint64_t sys_access(syscall_frame_t * f);
uint64_t sys_faccessat(syscall_frame_t * f);
uint64_t sys_getcwd(syscall_frame_t * f);
uint64_t sys_readlink(syscall_frame_t * f);
uint64_t sys_readlinkat(syscall_frame_t * f);
uint64_t sys_chdir(syscall_frame_t * f);
uint64_t sys_fchdir(syscall_frame_t * f);
uint64_t sys_chroot(syscall_frame_t * f);
uint64_t sys_getdents64(syscall_frame_t * f);

/* syscall_fs sub-file prototypes (syscall_fs_mod.c) */
uint64_t sys_unlink(syscall_frame_t * f);
uint64_t sys_rename(syscall_frame_t * f);
uint64_t sys_truncate(syscall_frame_t * f);
uint64_t sys_ftruncate(syscall_frame_t * f);
uint64_t sys_mkdir(syscall_frame_t * f);
uint64_t sys_mkdirat(syscall_frame_t * f);
uint64_t sys_rmdir(syscall_frame_t * f);
uint64_t sys_link(syscall_frame_t * f);
uint64_t sys_linkat(syscall_frame_t * f);
uint64_t sys_symlink(syscall_frame_t * f);
uint64_t sys_symlinkat(syscall_frame_t * f);
uint64_t sys_mknod(syscall_frame_t * f);
uint64_t sys_mknodat(syscall_frame_t * f);
uint64_t sys_fsync(syscall_frame_t * f);
uint64_t sys_utimensat(syscall_frame_t * f);
uint64_t sys_unlinkat(syscall_frame_t * f);
uint64_t sys_renameat(syscall_frame_t * f);

/* syscall_fs sub-file prototypes (syscall_fs_perm.c) */
uint64_t sys_chmod(syscall_frame_t * f);
uint64_t sys_fchmod(syscall_frame_t * f);
uint64_t sys_fchmodat(syscall_frame_t * f);

uint64_t syscall_handle_proc(syscall_frame_t * f);
uint64_t syscall_handle_mm(syscall_frame_t * f);
uint64_t syscall_handle_sig(syscall_frame_t * f);
uint64_t syscall_handle_misc(syscall_frame_t * f);

/* Sub-dispatchers for misc syscalls. */
int syscall_handle_id(syscall_frame_t * f);
int syscall_handle_time(syscall_frame_t * f);
int syscall_handle_poll(syscall_frame_t * f);

/* Shared externs used across category files. */
extern void syscall_return(void);
extern int elf_load_user(vfs_node_t * node, uint64_t pml4, elf_info_t * info);
extern void fork_child_return(void);

/* signal_deliver is used by syscall.c and syscall_proc.c. */
void signal_deliver(syscall_frame_t * f);
void do_exit(syscall_frame_t * f, int exit_code);
void do_exit_from_isr(int sig);

/* Process sub-handlers (syscall_proc_fork.c) */
uint64_t do_fork(syscall_frame_t * f);
uint64_t do_vfork(syscall_frame_t * f);
uint64_t do_clone_thread(syscall_frame_t * f, uint64_t clone_flags);

/* Process sub-handlers (syscall_proc_exec.c) */
uint64_t do_execve(syscall_frame_t * f);
uint64_t do_execveat(syscall_frame_t * f);

/* Process sub-handlers (syscall_proc_wait.c) */
uint64_t do_wait4(syscall_frame_t * f);
uint64_t do_waitid(syscall_frame_t * f);

/* Process sub-handlers (syscall_proc_fd.c) */
uint64_t do_pipe2(syscall_frame_t * f);
uint64_t do_dup2(syscall_frame_t * f);
uint64_t do_dup3(syscall_frame_t * f);

/*
 * Check if first pending signal has SA_RESTART.
 * Defined in syscall_sig.c, used by blocking loops in syscall_file.c / syscall_proc.c.
 */
int pending_signal_restarts(proc_t * cur);

/* Pipe blocking helpers (syscall_pipe.c) */
int pipe_wait_readable(pipe_t * p, file_desc_t * desc);
int pipe_wait_writable(pipe_t * p, file_desc_t * desc);
int pipe_no_readers(pipe_t * p);
int pipe_is_empty(pipe_t * p);

#endif
