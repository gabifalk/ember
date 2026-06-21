/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_PROC_H
#define EMBER_PROC_H

#include <stdint.h>
#include "ember/fd.h"
#include "ember/signal.h"

#define MAX_PROCS 256
#define PROC_KSTACK_SIZE 65536	/* Temporarily doubled for diagnostic stack usage. */
#define KSTACK_CANARY    0xDEAD57ACu	/* "DEAD STACK". */
#define MAX_VMAS 512
#define EMBER_PATH_MAX 1024

#define PROC_UNUSED    0
#define PROC_RUNNING   1
#define PROC_ZOMBIE    2
#define PROC_READY     3
#define PROC_SLEEPING  4
#define PROC_STOPPED   5

typedef struct vma {
	uint64_t start;
	uint64_t length;
	uint8_t prot;
	uint8_t used;
} vma_t;

typedef struct proc {
	int pid;
	int ppid;
	int state;
	int exit_code;
	int exit_signal;	/* 0 = Normal exit, >0 = killed by this signal. */
	uint64_t last_sc;	/* Last syscall number (for trace) */
	int64_t last_ret;	/* Last syscall return value (for trace) */
	uint64_t pml4_phys;	/* User page table. */
	uint64_t brk;		/* User brk. */
	uint64_t brk_base;	/* Minimum brk (end of BSS) */
	uint64_t mmap_next;	/* Next mmap VA. */
	uint64_t fs_base;	/* Saved FS base (TLS) */
	fd_entry_t fds[MAX_FDS];	/* Per-process fd table. */

	uint32_t sig_pending;	/* Bitmask of pending signals. */
	uint32_t sig_mask;	/* Bitmask of blocked signals. */
	uint64_t sig_handlers[NSIG];	/* SIG_DFL, SIG_IGN, or user handler addr. */
	uint64_t sig_flags[NSIG];	/* sa_flags per signal. */
	uint64_t sig_restorer[NSIG];	/* sa_restorer per signal (for SA_RESTORER) */
	uint32_t sig_sa_mask[NSIG];	/* sa_mask per signal. */
	uint64_t alarm_tick;	/* kernel_ticks deadline for SIGALRM (0=none) */
	uint64_t itimer_interval_ticks;	/* Repeating interval for ITIMER_REAL (0=one-shot) */
	uint8_t reported_stopped;	/* 1 If WUNTRACED already reported this stop. */

	vma_t vmas[MAX_VMAS];

	char cwd[EMBER_PATH_MAX];	/* Current working directory. */
	char exe_path[EMBER_PATH_MAX];	/* Path of currently executing binary. */
	char root_path[EMBER_PATH_MAX];	/* Chroot path (empty = no chroot) */
	uint32_t umask;		/* File creation mask. */
	int pgid;		/* Process group ID. */
	int sid;		/* Session ID. */

	int wait_chan;		/* Sleep/wakeup channel identifier. */
	uint64_t clear_child_tid;	/* Address to zero + futex wake on exit. */
	int tgid;		/* Thread group ID (== pid for main thread) */
	int vfork_parent;	/* Parent pid if vfork child (0 = not vfork) */
	int free_next;		/* Next index in proc free list (-1 = end) */
	int pid_hash_next;	/* Next index in PID hash chain (-1 = end) */

	/* Saved kernel RSP for context switching between processes. */
	uint64_t saved_ksp;

	/*
	 * FPU/SSE state saved/restored on context_switch.
	 * fxsave requires 512 bytes, 16-byte aligned.
	 * Over-allocate by 16 and align at runtime via FXSAVE_PTR(),
	 * because tcc ignores __attribute__((aligned)) on struct members.
	 */
	uint8_t fxsave_area[512 + 16];

	uint8_t kstack[PROC_KSTACK_SIZE] __attribute__ ((aligned(16)));
} proc_t;

int vma_add(proc_t *p, uint64_t start, uint64_t length, uint8_t prot);
int vma_addr_writable(proc_t *p, uint64_t addr);

/* Return 16-byte aligned pointer within p->fxsave_area. */
static inline uint8_t *fxsave_ptr(proc_t *p)
{
	uintptr_t a = (uintptr_t)p->fxsave_area;
	return (uint8_t *)((a + 15) & ~(uintptr_t)15);
}

extern proc_t procs[MAX_PROCS];

#include "ember/percpu.h"
/* current_proc is provided by percpu.h as extern proc_t *current_proc. */

void proc_init(void);
proc_t *proc_alloc(void);
proc_t *proc_find(int pid);
void proc_free_slot(int idx);
void proc_check_stack(proc_t * p);

#endif
