/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/proc.h"
#include "ember/sched.h"
#include "ember/spinlock.h"
#include "ember/console.h"
#include <ember/cpu.h>

proc_t procs[MAX_PROCS] __attribute__((aligned(16)));

/* Per-CPU state. */
struct proc *percpu_current[MAX_CPUS];
uint32_t cpu_lapic_ids[MAX_CPUS];
int cpu_count = 1;
int cpu_online_count = 1;
int lapic_to_cpu[256];
volatile uint32_t *lapic_base = 0;

void
cpu_init(void)
{
	for (int i = 0; i < 256; i++)
		lapic_to_cpu[i] = -1;
	cpu_count = 1;
	cpu_online_count = 1;
}

static spinlock_t proc_lock = SPINLOCK_INIT;
static int next_pid = 1;
static int free_head = -1;

/* PID hash table: O(1) lookup by PID. */
#define PID_HASH_BUCKETS 256
static int pid_hash[PID_HASH_BUCKETS];

static inline int
pid_hash_key(int pid)
{
	return pid & (PID_HASH_BUCKETS - 1);
}

static void
pid_hash_insert(int idx)
{
	int h = pid_hash_key(procs[idx].pid);
	procs[idx].pid_hash_next = pid_hash[h];
	pid_hash[h] = idx;
}

static void
pid_hash_remove(int idx)
{
	int h = pid_hash_key(procs[idx].pid);
	int *pp = &pid_hash[h];
	while (*pp >= 0) {
		if (*pp == idx) {
			*pp = procs[idx].pid_hash_next;
			return;
		}
		pp = &procs[*pp].pid_hash_next;
	}
}

void
proc_init(void)
{
	for (int h = 0; h < PID_HASH_BUCKETS; h++)
		pid_hash[h] = -1;
	for (int i = 0; i < MAX_PROCS; i++) {
		procs[i].pid = 0;
		procs[i].ppid = 0;
		procs[i].state = PROC_UNUSED;
		procs[i].exit_code = 0;
		procs[i].pml4_phys = 0;
		procs[i].brk = 0;
		procs[i].mmap_next = 0;
		procs[i].fs_base = 0;
		procs[i].saved_ksp = 0;
		procs[i].sig_pending = 0;
		procs[i].sig_mask = 0;
		for (int s = 0; s < NSIG; s++)
			procs[i].sig_handlers[s] = SIG_DFL;
		procs[i].cwd[0] = '/';
		procs[i].cwd[1] = '\0';
		procs[i].exe_path[0] = '\0';
		procs[i].root_path[0] = '\0';
		procs[i].umask = 022;
		procs[i].pgid = 0;
		procs[i].sid = 0;
		procs[i].clear_child_tid = 0;
		procs[i].tgid = 0;
		procs[i].vfork_parent = 0;
		procs[i].free_next = (i + 1 < MAX_PROCS) ? i + 1 : -1;
		procs[i].pid_hash_next = -1;
		for (int j = 0; j < MAX_FDS; j++) {
			procs[i].fds[j].desc = 0;
			procs[i].fds[j].fd_flags = 0;
		}
		for (int v = 0; v < MAX_VMAS; v++)
			procs[i].vmas[v].used = 0;
		*(uint32_t *) procs[i].kstack = KSTACK_CANARY;
	}
	free_head = 0;
	set_bsp_current_proc(0);
}

proc_t *
proc_alloc(void)
{
	spin_lock(&proc_lock);
	if (free_head < 0) {
		spin_unlock(&proc_lock);
		return 0;
	}
	int i = free_head;
	free_head = procs[i].free_next;
	procs[i].pid = next_pid++;
	procs[i].state = PROC_SLEEPING;	/* Not READY -- caller sets READY after full setup. */
	procs[i].exit_code = 0;
	procs[i].exit_signal = 0;
	procs[i].fs_base = 0;
	procs[i].saved_ksp = 0;
	procs[i].sig_pending = 0;
	procs[i].sig_mask = 0;
	for (int s = 0; s < NSIG; s++)
		procs[i].sig_handlers[s] = SIG_DFL;
	procs[i].cwd[0] = '/';
	procs[i].cwd[1] = '\0';
	procs[i].exe_path[0] = '\0';
	procs[i].root_path[0] = '\0';
	procs[i].umask = 022;
	procs[i].pgid = procs[i].pid;
	procs[i].sid = procs[i].pid;
	procs[i].clear_child_tid = 0;
	procs[i].tgid = procs[i].pid;
	procs[i].vfork_parent = 0;
	for (int j = 0; j < MAX_FDS; j++) {
		procs[i].fds[j].desc = 0;
		procs[i].fds[j].fd_flags = 0;
	}
	for (int v = 0; v < MAX_VMAS; v++)
		procs[i].vmas[v].used = 0;
	*(uint32_t *) procs[i].kstack = KSTACK_CANARY;
	pid_hash_insert(i);
	spin_unlock(&proc_lock);
	sched_note_slot(i);
	return &procs[i];
}

proc_t *
proc_find(int pid)
{
	spin_lock(&proc_lock);
	int idx = pid_hash[pid_hash_key(pid)];
	while (idx >= 0) {
		if (procs[idx].pid == pid && procs[idx].state != PROC_UNUSED) {
			spin_unlock(&proc_lock);
			return &procs[idx];
		}
		idx = procs[idx].pid_hash_next;
	}
	spin_unlock(&proc_lock);
	return 0;
}

void
proc_free_slot(int idx)
{
	spin_lock(&proc_lock);
	pid_hash_remove(idx);
	procs[idx].free_next = free_head;
	free_head = idx;
	spin_unlock(&proc_lock);
}

void
proc_check_stack(proc_t * p)
{
	if (*(uint32_t *) p->kstack != KSTACK_CANARY) {
		char buf[32];
		int pid = p->pid;
		int pos = 0;
		/* Format PID as decimal. */
		if (pid == 0) {
			buf[pos++] = '0';
		} else {
			char tmp[10];
			int n = 0;
			while (pid > 0) {
				tmp[n++] = '0' + (pid % 10);
				pid /= 10;
			}
			while (n > 0)
				buf[pos++] = tmp[--n];
		}
		buf[pos] = '\0';
		console_write("\n*** KERNEL STACK OVERFLOW (pid ");
		console_write(buf);
		console_write(") ***\n");
		for (;;)
			__asm__ __volatile__("hlt");
	}
}
