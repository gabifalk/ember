/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

/* -- Resource limits (shared by prlimit64 and getrlimit) ----------- */

static void get_resource_limit(uint64_t resource, uint64_t *cur, uint64_t *max)
{
    if (resource == RLIMIT_NOFILE) {
        *cur = *max = MAX_FDS;
    } else if (resource == RLIMIT_STACK) {
        *cur = *max = 8 * 1024 * 1024;
    } else {
        *cur = *max = RLIM_INFINITY;
    }
}

static void write_rlimit_to_user(uint64_t user_ptr, uint64_t resource)
{
    uint64_t cur, max;
    get_resource_limit(resource, &cur, &max);
    USER_ACCESS_BEGIN();
    uint64_t *rlim = (uint64_t *)(uintptr_t)user_ptr;
    rlim[0] = cur;
    rlim[1] = max;
    USER_ACCESS_END();
}

static uint64_t handle_prlimit64(syscall_frame_t *f)
{
    uint64_t resource = f->rsi;
    uint64_t user_new = f->rdx;
    uint64_t user_old = f->r10;
    if (user_old)
        write_rlimit_to_user(user_old, resource);
    (void)user_new;
    f->rax = 0;
    return 0;
}

static uint64_t handle_getrlimit(syscall_frame_t *f)
{
    uint64_t resource = f->rdi;
    uint64_t user_rlim = f->rsi;
    if (user_rlim)
        write_rlimit_to_user(user_rlim, resource);
    f->rax = 0;
    return 0;
}

/* -- Timestamp updates (shared by utime and utimes) ---------------- */

static void update_node_times(vfs_node_t *node, uint32_t actime, uint32_t modtime)
{
    if (node->ext2_ino) {
        ext2_inode_t ei;
        if (ext2_read_inode(node->ext2_ino, &ei) == 0) {
            ei.i_atime = actime;
            ei.i_mtime = modtime;
            ext2_write_inode(node->ext2_ino, &ei);
        }
    }
    node->atime = actime;
    node->mtime = modtime;
}

static vfs_node_t *lookup_path_from_user(const char *user_path)
{
    char pathbuf[EMBER_PATH_MAX];
    char resolved[EMBER_PATH_MAX];
    copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
    resolve_path(pathbuf, resolved, sizeof(resolved));
    return vfs_lookup(resolved);
}

static uint64_t handle_utime(syscall_frame_t *f)
{
    const char *user_path = (const char *)f->rdi;
    uint64_t user_times = f->rsi;
    vfs_node_t *node = lookup_path_from_user(user_path);
    if (!node) {
        f->rax = SYSCALL_ERR(ENOENT);
        return f->rax;
    }
    if (node->ext2_ino) {
        if (user_times) {
            USER_ACCESS_BEGIN();
            /* Struct utimbuf { time_t actime; time_t modtime; }. */
            uint64_t *ut = (uint64_t *)(uintptr_t)user_times;
            uint64_t actime = ut[0];
            uint64_t modtime = ut[1];
            USER_ACCESS_END();
            update_node_times(node, (uint32_t)actime, (uint32_t)modtime);
        } else {
            uint32_t now = (uint32_t)kernel_time_sec();
            update_node_times(node, now, now);
        }
    }
    f->rax = 0;
    return 0;
}

static uint64_t handle_utimes(syscall_frame_t *f)
{
    const char *user_path = (const char *)f->rdi;
    uint64_t user_times = f->rsi;
    vfs_node_t *node = lookup_path_from_user(user_path);
    if (!node) {
        f->rax = SYSCALL_ERR(ENOENT);
        return f->rax;
    }
    if (node->ext2_ino) {
        if (user_times) {
            USER_ACCESS_BEGIN();
            /* Struct timeval { time_t tv_sec; suseconds_t tv_usec; } times[2]. */
            uint64_t *tv = (uint64_t *)(uintptr_t)user_times;
            uint64_t actime = tv[0];   /* tv[0].tv_sec. */
            uint64_t modtime = tv[2];  /* tv[1].tv_sec. */
            USER_ACCESS_END();
            update_node_times(node, (uint32_t)actime, (uint32_t)modtime);
        } else {
            uint32_t now = (uint32_t)kernel_time_sec();
            update_node_times(node, now, now);
        }
    }
    f->rax = 0;
    return 0;
}

/* -- Futex --------------------------------------------------------- */

static uint64_t handle_futex(syscall_frame_t *f)
{
    uint64_t uaddr = f->rdi;
    int op = (int)f->rsi & 0x7f;  /* Strip FUTEX_PRIVATE_FLAG. */
    uint32_t val = (uint32_t)f->rdx;
    int futex_chan = (int)((uaddr >> 2) & 0x7fffffff) | 0x40000000;

    if (op == 0) { /* FUTEX_WAIT. */
        USER_ACCESS_BEGIN();
        uint32_t cur_val = *(volatile uint32_t *)(uintptr_t)uaddr;
        USER_ACCESS_END();
        if (cur_val != val) {
            f->rax = SYSCALL_ERR(EAGAIN);
            return f->rax;
        }
        /* Sleep on futex channel. */
        for (;;) {
            if (current_proc) {
                uint32_t pend = current_proc->sig_pending & ~current_proc->sig_mask;
                if (pend) {
                    f->rax = SYSCALL_ERR(EINTR);
                    return f->rax;
                }
            }
            sched_sleep(futex_chan);
            f->rax = 0;
            return 0;
        }
    }
    if (op == 1 || op == 5) { /* FUTEX_WAKE or FUTEX_WAKE_OP. */
        int woken = sched_wakeup_n(futex_chan, (int)val);
        f->rax = (uint64_t)woken;
        return f->rax;
    }
    f->rax = 0;
    return 0;
}

/* -- Getrandom ----------------------------------------------------- */

static uint64_t handle_getrandom(syscall_frame_t *f)
{
    char *user_buf = (char *)f->rdi;
    uint64_t buflen = f->rsi;
    USER_ACCESS_BEGIN();
    uint64_t seed = kernel_ticks * 6364136223846793005ULL + 0x12345678abcdef01ULL;
    for (uint64_t i = 0; i < buflen; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        user_buf[i] = (char)(seed >> 33);
    }
    USER_ACCESS_END();
    f->rax = buflen;
    return f->rax;
}

/* -- Getrusage ----------------------------------------------------- */

static uint64_t handle_getrusage(syscall_frame_t *f)
{
    uint64_t user_usage = f->rsi;
    if (user_usage) {
        USER_ACCESS_BEGIN();
        uint8_t *p = (uint8_t *)(uintptr_t)user_usage;
        kmemzero(p, 144);
        USER_ACCESS_END();
    }
    f->rax = 0;
    return 0;
}

/* -- Sysinfo ------------------------------------------------------- */

static uint64_t handle_sysinfo(syscall_frame_t *f)
{
    uint64_t user_buf = f->rdi;
    if (user_buf) {
        USER_ACCESS_BEGIN();
        uint8_t *p = (uint8_t *)(uintptr_t)user_buf;
        /* Zero 112 bytes. */
        kmemzero(p, 112);
        /*
         * Struct sysinfo layout (64-bit):
         * offset 0:  long uptime     (8 bytes)
         * offset 8:  unsigned long loads[3] (24 bytes)
         * offset 32: unsigned long totalram  (8 bytes)
         * offset 40: unsigned long freeram   (8 bytes)
         * offset 48: unsigned long sharedram (8 bytes)
         * offset 56: unsigned long bufferram (8 bytes)
         * offset 64: unsigned long totalswap (8 bytes)
         * offset 72: unsigned long freeswap  (8 bytes)
         * offset 80: unsigned short procs    (2 bytes)
         * offset 88: unsigned long totalhigh (8 bytes)
         * offset 96: unsigned long freehigh  (8 bytes)
         * offset 104: unsigned int mem_unit  (4 bytes)
         */
        uint64_t uptime = kernel_time_sec();
        uint64_t total = pmm_get_total_pages() * 4096;
        uint64_t free_mem = pmm_get_free_pages() * 4096;
        *(uint64_t *)(p + 0) = uptime;
        *(uint64_t *)(p + 32) = total;
        *(uint64_t *)(p + 40) = free_mem;
        /* Count running processes. */
        uint16_t nprocs = 0;
        for (int i = 0; i < MAX_PROCS; i++) {
            if (procs[i].state != PROC_UNUSED) nprocs++;
        }
        *(uint16_t *)(p + 80) = nprocs;
        *(uint32_t *)(p + 104) = 1; /* mem_unit. */
        USER_ACCESS_END();
    }
    f->rax = 0;
    return 0;
}

/* -- Close range --------------------------------------------------- */

static uint64_t handle_close_range(syscall_frame_t *f)
{
    uint32_t first = (uint32_t)f->rdi;
    uint32_t last = (uint32_t)f->rsi;
    if (last >= MAX_FDS) last = MAX_FDS - 1;
    for (uint32_t i = first; i <= last; i++) {
        fd_entry_t *entry = fd_get((int)i);
        if (entry && entry->desc) {
            file_desc_unref(entry->desc);
            entry->desc = 0;
            entry->fd_flags = 0;
        }
    }
    f->rax = 0;
    return 0;
}

/* -- Sched_getaffinity --------------------------------------------- */

static uint64_t handle_sched_getaffinity(syscall_frame_t *f)
{
    uint64_t cpusetsize = f->rsi;
    uint64_t user_mask = f->rdx;
    if (user_mask && cpusetsize > 0) {
        USER_ACCESS_BEGIN();
        uint8_t *p = (uint8_t *)(uintptr_t)user_mask;
        kmemzero(p, cpusetsize);
        /* Set bit for each online CPU. */
        for (int i = 0; i < cpu_count && (i / 8) < (int)cpusetsize; i++)
            p[i / 8] |= (1u << (i % 8));
        USER_ACCESS_END();
    }
    f->rax = cpusetsize > 0 ? cpusetsize : 8;
    return f->rax;
}

/* -- Mincore ------------------------------------------------------- */

static uint64_t handle_mincore(syscall_frame_t *f)
{
    uint64_t addr = f->rdi;
    uint64_t length = f->rsi;
    uint64_t user_vec = f->rdx;
    if (user_vec && length > 0) {
        uint64_t npages = (length + 4095) / 4096;
        USER_ACCESS_BEGIN();
        uint8_t *vec = (uint8_t *)(uintptr_t)user_vec;
        for (uint64_t i = 0; i < npages; i++) vec[i] = 1;
        USER_ACCESS_END();
    }
    (void)addr;
    f->rax = 0;
    return 0;
}

/* -- Sync / fdatasync ---------------------------------------------- */

static uint64_t handle_sync(syscall_frame_t *f)
{
    if (ext2_is_ready()) {
        blkcache_sync(ext2_get_dev());
        blkdev_flush(ext2_get_dev());
    }
    f->rax = 0;
    return 0;
}

/* -- Main dispatcher ----------------------------------------------- */

uint64_t syscall_handle_misc(syscall_frame_t *f) {
    if (syscall_handle_id(f)) return f->rax;
    if (syscall_handle_time(f)) return f->rax;
    if (syscall_handle_poll(f)) return f->rax;

    switch (f->rax) {

    case SYS_SET_ROBUST_LIST:
        f->rax = 0;
        return 0;

    case SYS_RSEQ:
        f->rax = SYSCALL_ERR(ENOSYS);
        return f->rax;

    case SYS_PRLIMIT64:     return handle_prlimit64(f);
    case SYS_GETRLIMIT:     return handle_getrlimit(f);
    case SYS_GETRANDOM:     return handle_getrandom(f);
    case SYS_FUTEX:         return handle_futex(f);
    case SYS_GETRUSAGE:     return handle_getrusage(f);
    case SYS_SYSINFO:       return handle_sysinfo(f);
    case SYS_UTIME:         return handle_utime(f);
    case SYS_UTIMES:        return handle_utimes(f);
    case SYS_CLOSE_RANGE:   return handle_close_range(f);
    case SYS_SCHED_GETAFFINITY: return handle_sched_getaffinity(f);
    case SYS_MINCORE:       return handle_mincore(f);

    case SYS_SETRLIMIT:
    case SYS_MADVISE:
    case SYS_FADVISE64:
    case SYS_SETHOSTNAME:
    case SYS_SETDOMAINNAME:
    case SYS_SYSLOG:
    case SYS_MLOCK:
    case SYS_MUNLOCK:
    case SYS_MSYNC:
    case SYS_READAHEAD:
    case SYS_UNSHARE:
        f->rax = 0;
        return 0;

    case SYS_FDATASYNC:
    case SYS_SYNC:
        return handle_sync(f);

    case SYS_STATX:
    case SYS_CLONE3:
    case SYS_SOCKET:
    case SYS_CONNECT:
    case SYS_SENDTO:
    case SYS_RECVFROM:
    case SYS_SOCKETPAIR:
    case SYS_MEMBARRIER:
        f->rax = SYSCALL_ERR(ENOSYS);
        return f->rax;

    case SYS_IOPL:
    case SYS_IOPERM:
        f->rax = SYSCALL_ERR(EPERM);
        return f->rax;

    default:
        f->rax = SYSCALL_ERR(ENOSYS);
        return f->rax;
    }
}
