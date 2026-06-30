/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

int
syscall_handle_id(syscall_frame_t * f)
{
	switch (f->rax) {

	case SYS_ARCH_PRCTL:{
			uint64_t code = f->rdi;
			uint64_t addr = f->rsi;
			if (code == ARCH_SET_FS) {
				wrmsr(IA32_FS_BASE, addr);
				if (current_proc)
					current_proc->fs_base = addr;
				f->rax = 0;
			} else if (code == ARCH_GET_FS) {
				uint64_t fs_val =
				    current_proc ? current_proc->fs_base : 0;
				USER_ACCESS_BEGIN();
				*(uint64_t *) (uintptr_t) addr = fs_val;
				USER_ACCESS_END();
				f->rax = 0;
			} else {
				f->rax = SYSCALL_ERR(EINVAL);
			}
			return 1;
		}

	case SYS_SET_TID_ADDRESS:{
			if (current_proc)
				current_proc->clear_child_tid = f->rdi;
			f->rax =
			    current_proc ? (uint64_t) current_proc->pid : 1;
			return 1;
		}

	case SYS_UNAME:{
			uint64_t user_buf = f->rdi;
			USER_ACCESS_BEGIN();
			char *p = (char *)(uintptr_t) user_buf;
			kmemzero(p, UTSNAME_SIZE);
			/* Sysname. */
			{
				const char *s = "Linux";
				int i;
				for (i = 0; s[i]; i++)
					p[i] = s[i];
			}
			/* Nodename. */
			{
				const char *s = "ember";
				int i;
				for (i = 0; s[i]; i++)
					p[UTSNAME_FIELD_LEN + i] = s[i];
			}
			/* Release. */
			{
				const char *s = "6.1.0";
				int i;
				for (i = 0; s[i]; i++)
					p[2 * UTSNAME_FIELD_LEN + i] = s[i];
			}
			/* Version. */
			{
				const char *s = "#1";
				int i;
				for (i = 0; s[i]; i++)
					p[3 * UTSNAME_FIELD_LEN + i] = s[i];
			}
			/* Machine. */
			{
				const char *s = "x86_64";
				int i;
				for (i = 0; s[i]; i++)
					p[4 * UTSNAME_FIELD_LEN + i] = s[i];
			}
			/* Domainname at 5 * UTSNAME_FIELD_LEN stays zero. */
			USER_ACCESS_END();
			f->rax = 0;
			return 1;
		}

	case SYS_GETPID:
		f->rax = current_proc ? (uint64_t) current_proc->tgid : 1;
		return 1;

	case SYS_GETPPID:
		f->rax = current_proc ? (uint64_t) current_proc->ppid : 0;
		return 1;

	case SYS_GETTID:
		f->rax = current_proc ? (uint64_t) current_proc->pid : 1;
		return 1;

	case SYS_GETUID:
	case SYS_GETGID:
	case SYS_GETEUID:
	case SYS_GETEGID:
		f->rax = 0;
		return 1;

	case SYS_UMASK:{
			uint32_t new_mask = (uint32_t) f->rdi & S_ACCESSPERMS;
			uint32_t old_mask =
			    current_proc ? current_proc->umask : 022;
			if (current_proc)
				current_proc->umask = new_mask;
			f->rax = (uint64_t) old_mask;
			return 1;
		}

	case SYS_SETPGID:{
			int tpid = (int)f->rdi;
			int tpgid = (int)f->rsi;
			proc_t *target;
			if (tpid == 0)
				target = current_proc;
			else
				target = proc_find(tpid);
			if (!target) {
				f->rax = SYSCALL_ERR(ESRCH);
				return 1;
			}
			if (tpgid == 0)
				tpgid = target->pid;
			target->pgid = tpgid;
			f->rax = 0;
			return 1;
		}

	case SYS_GETPGRP:
		f->rax = current_proc ? (uint64_t) current_proc->pgid : 0;
		return 1;

	case SYS_GETPGID:{
			int tpid = (int)f->rdi;
			proc_t *target;
			if (tpid == 0)
				target = current_proc;
			else
				target = proc_find(tpid);
			if (!target) {
				f->rax = SYSCALL_ERR(ESRCH);
				return 1;
			}
			f->rax = (uint64_t) target->pgid;
			return 1;
		}

	case SYS_SETSID:{
			if (!current_proc) {
				f->rax = SYSCALL_ERR(EPERM);
				return 1;
			}
			current_proc->pgid = current_proc->pid;
			current_proc->sid = current_proc->pid;
			f->rax = (uint64_t) current_proc->pid;
			return 1;
		}

	case SYS_GETSID:{
			int tpid = (int)f->rdi;
			if (tpid == 0) {
				f->rax =
				    current_proc ? (uint64_t) current_proc->
				    sid : 0;
			} else {
				proc_t *target = proc_find(tpid);
				if (!target) {
					f->rax = SYSCALL_ERR(ESRCH);
					return 1;
				}
				f->rax = (uint64_t) target->sid;
			}
			return 1;
		}

		/* Credential stubs -- Ember runs single-user as root. */
	case SYS_SETUID:
	case SYS_SETGID:
	case SYS_SETREUID:
	case SYS_SETREGID:
	case SYS_SETGROUPS:
		f->rax = 0;
		return 1;

	case SYS_SETFSUID:
		/* Returns previous fsuid (always 0 = root) */
		f->rax = 0;
		return 1;

	case SYS_SETFSGID:
		/* Returns previous fsgid (always 0 = root) */
		f->rax = 0;
		return 1;

	case SYS_GETGROUPS:{
			/* Getgroups(size, list): return 0 supplementary groups. */
			f->rax = 0;
			return 1;
		}

		/* Glibc credential syscalls -- Ember runs as root. */
	case SYS_SETRESUID:
	case SYS_SETRESGID:
		f->rax = 0;
		return 1;

	case SYS_GETRESUID:{
			/* Getresuid(ruid*, euid*, suid*) -- all 0 (root) */
			USER_ACCESS_BEGIN();
			if (f->rdi)
				*(int *)(uintptr_t) f->rdi = 0;
			if (f->rsi)
				*(int *)(uintptr_t) f->rsi = 0;
			if (f->rdx)
				*(int *)(uintptr_t) f->rdx = 0;
			USER_ACCESS_END();
			f->rax = 0;
			return 1;
		}

	case SYS_GETRESGID:{
			/* Getresgid(rgid*, egid*, sgid*) -- all 0 (root) */
			USER_ACCESS_BEGIN();
			if (f->rdi)
				*(int *)(uintptr_t) f->rdi = 0;
			if (f->rsi)
				*(int *)(uintptr_t) f->rsi = 0;
			if (f->rdx)
				*(int *)(uintptr_t) f->rdx = 0;
			USER_ACCESS_END();
			f->rax = 0;
			return 1;
		}

	case SYS_PERSONALITY:
		/* Return current personality (always PER_LINUX = 0) */
		f->rax = 0;
		return 1;

	case SYS_PRCTL:
		f->rax = 0;
		return 1;

	default:
		return 0;
	}
}
