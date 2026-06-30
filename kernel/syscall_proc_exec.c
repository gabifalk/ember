/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"
#include "ember/bug.h"
#include "ember/bkl.h"

#define EXECVE_ARG_MAX   131072
#define EXECVE_TOTAL_MAX 131072
#define SHEBANG_MAX_DEPTH 4

/* Free execve argument buffers. */
static void
execve_free_args(const char **kargv, const char **kenvp, char *strbuf)
{
	if (kargv)
		kfree((void *)kargv);
	if (kenvp)
		kfree((void *)kenvp);
	if (strbuf)
		kfree(strbuf);
}

/*
 * Parse shebang (#!) line from a VFS node.
 *
 * On success (file starts with #!): writes interpreter path into interp_out,
 * optional argument into opt_out (or '\0' if none), and returns 1.
 * If the file does not start with #!: returns 0.
 * On error: returns negative errno (-ENOEXEC for empty interpreter).
 *
 * interp_out and opt_out must each be at least 256 bytes.
 */
static int
parse_shebang(vfs_node_t * node, char *interp_out, char *opt_out)
{
	char hdr[2];
	uint64_t nr = vfs_read(node, 0, hdr, 2);
	if (nr < 2 || hdr[0] != '#' || hdr[1] != '!')
		return 0;	/* Not a shebang. */

	char line[256];
	uint64_t line_nr = vfs_read(node, 0, line, 255);
	line[line_nr] = '\0';

	/* Find end of first line. */
	int eol = 0;
	while (eol < (int)line_nr && line[eol] != '\n' && line[eol] != '\r')
		eol++;
	line[eol] = '\0';

	/* Skip "#!" and leading whitespace. */
	char *p = line + 2;
	while (*p == ' ' || *p == '\t')
		p++;

	/* Extract interpreter path. */
	char *interp = p;
	while (*p && *p != ' ' && *p != '\t')
		p++;

	/* Extract optional argument. */
	opt_out[0] = '\0';
	if (*p) {
		*p++ = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p) {
			/* Trim trailing whitespace. */
			char *end = p;
			while (*end)
				end++;
			end--;
			while (end > p && (*end == ' ' || *end == '\t'))
				*end-- = '\0';
			/* Copy to output. */
			int i = 0;
			while (p[i] && i < 254) {
				opt_out[i] = p[i];
				i++;
			}
			opt_out[i] = '\0';
		}
	}

	if (!interp[0])
		return -ENOEXEC;

	/* Copy interpreter to output. */
	{
		int i = 0;
		while (interp[i] && i < 254) {
			interp_out[i] = interp[i];
			i++;
		}
		interp_out[i] = '\0';
	}

	return 1;
}

/*
 * Copy a NULL-terminated user string array into kernel buffers.
 *
 * user_ptrs: userspace pointer to array of char* pointers
 * count:     number of strings (not counting NULL terminator)
 * kdst:      output array of kernel string pointers (must hold count entries)
 * strbuf:    shared string buffer for all copied strings
 * stroff:    in/out offset into strbuf
 *
 * Returns 0 on success, -E2BIG if total string data exceeds limits.
 * Caller must hold USER_ACCESS (SMAP disabled) around this call.
 */
static int
copy_user_strings(uint64_t user_ptrs, int count,
		  const char **kdst, char *strbuf, uint64_t * stroff)
{
	uint64_t *ptrs = (uint64_t *) user_ptrs;
	for (int i = 0; i < count; i++) {
		const char *usrc = (const char *)ptrs[i];
		if (*stroff >= EXECVE_TOTAL_MAX - 1)
			return -E2BIG;
		uint64_t j;
		for (j = 0;
		     j < EXECVE_ARG_MAX - 1
		     && *stroff + j < EXECVE_TOTAL_MAX - 1 && usrc[j]; j++)
			strbuf[*stroff + j] = usrc[j];
		if (usrc[j])
			return -E2BIG;
		strbuf[*stroff + j] = '\0';
		kdst[i] = &strbuf[*stroff];
		*stroff += j + 1;
	}
	return 0;
}

/* ---- Execve core ---- */
static uint64_t
do_execve_inner(syscall_frame_t * f, const char *resolved_path,
		const char **kargv, int argc,
		const char **kenvp, int envc, char *strbuf, int shebang_depth)
{
	vfs_node_t *node = vfs_lookup(resolved_path);
	if (!node) {
		console_write("execve ENOENT: ");
		console_write(resolved_path);
		console_write("\n");
		execve_free_args(kargv, kenvp, strbuf);
		f->rax = SYSCALL_ERR(ENOENT);
		return f->rax;
	}
	vfs_ref(node);		/* Prevent VFS cache eviction during exec. */
	if (node->type == VFS_NODE_DIR) {
		console_write("execve ISDIR: ");
		console_write(resolved_path);
		console_write("\n");
		vfs_unref(node);
		execve_free_args(kargv, kenvp, strbuf);
		f->rax = SYSCALL_ERR(EISDIR);
		return f->rax;
	}

	/* Check for shebang script. */
	{
		char interp_path[256];
		char opt_arg[256];
		int shebang = parse_shebang(node, interp_path, opt_arg);

		if (shebang < 0) {
			/* Parse error (empty interpreter) */
			vfs_unref(node);
			execve_free_args(kargv, kenvp, strbuf);
			f->rax = (uint64_t) shebang;
			return f->rax;
		}

		if (shebang == 1) {
			if (shebang_depth >= SHEBANG_MAX_DEPTH) {
				vfs_unref(node);
				execve_free_args(kargv, kenvp, strbuf);
				f->rax = SYSCALL_ERR(ELOOP);
				return f->rax;
			}

			int has_opt = opt_arg[0] != '\0';
			int extra = 1 + (has_opt ? 1 : 0) + 1;
			int new_argc = extra + (argc > 1 ? argc - 1 : 0);
			const char **new_kargv =
			    (const char **)kmalloc((uint64_t) (new_argc + 1) *
						   8);
			if (!new_kargv) {
				vfs_unref(node);
				execve_free_args(kargv, kenvp, strbuf);
				f->rax = SYSCALL_ERR(ENOMEM);
				return f->rax;
			}

			int interp_len = 0;
			while (interp_path[interp_len])
				interp_len++;
			int opt_len = 0;
			if (has_opt) {
				while (opt_arg[opt_len])
					opt_len++;
			}
			int script_len = 0;
			while (resolved_path[script_len])
				script_len++;
			int shebang_buf_size =
			    interp_len + 1 + opt_len + 1 + script_len + 1;
			char *shebang_strs =
			    (char *)kmalloc((uint64_t) shebang_buf_size);
			if (!shebang_strs) {
				vfs_unref(node);
				kfree((void *)new_kargv);
				execve_free_args(kargv, kenvp, strbuf);
				f->rax = SYSCALL_ERR(ENOMEM);
				return f->rax;
			}

			int off = 0;
			for (int i = 0; i < interp_len; i++)
				shebang_strs[off + i] = interp_path[i];
			shebang_strs[off + interp_len] = '\0';
			char *k_interp = &shebang_strs[off];
			off += interp_len + 1;

			char *k_opt = 0;
			if (has_opt) {
				for (int i = 0; i < opt_len; i++)
					shebang_strs[off + i] = opt_arg[i];
				shebang_strs[off + opt_len] = '\0';
				k_opt = &shebang_strs[off];
				off += opt_len + 1;
			}

			for (int i = 0; i < script_len; i++)
				shebang_strs[off + i] = resolved_path[i];
			shebang_strs[off + script_len] = '\0';
			char *k_script = &shebang_strs[off];

			int ai = 0;
			new_kargv[ai++] = k_interp;
			if (k_opt)
				new_kargv[ai++] = k_opt;
			new_kargv[ai++] = k_script;
			for (int i = 1; i < argc; i++)
				new_kargv[ai++] = kargv[i];

			if (kargv)
				kfree((void *)kargv);	/* Free old argv only; kenvp/strbuf pass to recursive call. */

			vfs_unref(node);	/* Done with script node, recursive call looks up interp. */

			char interp_resolved[EMBER_PATH_MAX];
			resolve_path(k_interp, interp_resolved,
				     sizeof(interp_resolved));

			return do_execve_inner(f, interp_resolved, new_kargv,
					       new_argc, kenvp, envc, strbuf,
					       shebang_depth + 1);
		}
	}

	/* Create new address space. */
	uint64_t new_pml4 = paging_create_user_pml4();
	if (!new_pml4) {
		console_write("execve ENOMEM(pml4): ");
		console_write(resolved_path);
		console_write("\n");
		vfs_unref(node);
		execve_free_args(kargv, kenvp, strbuf);
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	elf_info_t info;
	if (!elf_load_user(node, new_pml4, &info)) {
		console_write("execve ENOEXEC(elf_load): ");
		console_write(resolved_path);
		console_write(" ino=");
		console_hex64((uint64_t) node->ext2_ino);
		console_write(" size=");
		console_hex64(node->size);
		console_write(" fs=");
		console_hex64((uint64_t) node->fs_type);
		console_write("\n");
		paging_free_user_pml4(new_pml4);
		vfs_unref(node);
		execve_free_args(kargv, kenvp, strbuf);
		f->rax = SYSCALL_ERR(ENOEXEC);
		return f->rax;
	}
	vfs_unref(node);	/* Done reading ELF, release VFS ref. */

	uint64_t rsp =
	    setup_user_stack_argv(new_pml4, &info, kargv, argc, kenvp, envc);

	execve_free_args(kargv, kenvp, strbuf);

	if (!rsp) {
		/* Verified: models/exec_pml4_leak.pml. */
		paging_free_user_pml4(new_pml4);
		f->rax = SYSCALL_ERR(ENOMEM);
		return f->rax;
	}

	/* Update current process. */
	proc_t *cur = current_proc;
	uint64_t old_pml4 = cur->pml4_phys;
	cur->pml4_phys = new_pml4;
	cur->brk = info.brk_base;
	cur->brk_base = info.brk_base;
	cur->mmap_next = 0x7f0000000000ULL;

	/* Save exe_path. */
	{
		int ei;
		for (ei = 0; ei < EMBER_PATH_MAX - 1 && resolved_path[ei]; ei++)
			cur->exe_path[ei] = resolved_path[ei];
		cur->exe_path[ei] = '\0';
	}

	/* Clear VMAs from old address space. */
	for (int i = 0; i < MAX_VMAS; i++)
		cur->vmas[i].used = 0;

	/* Close O_CLOEXEC fds. */
	for (int i = 0; i < MAX_FDS; i++) {
		if (cur->fds[i].desc && (cur->fds[i].fd_flags & FD_CLOEXEC)) {
			file_desc_unref(cur->fds[i].desc);
			cur->fds[i].desc = 0;
			cur->fds[i].fd_flags = 0;
		}
	}

	/* Reset signal handlers (non-ignored -> SIG_DFL) */
	cur->sig_pending = 0;
	cur->sig_mask = 0;
	for (int s = 0; s < NSIG; s++) {
		if (cur->sig_handlers[s] != SIG_IGN)
			cur->sig_handlers[s] = SIG_DFL;
		cur->sig_flags[s] = 0;
		cur->sig_restorer[s] = 0;
		cur->sig_sa_mask[s] = 0;
	}
	cur->alarm_tick = 0;

	/* Clear TLS -- new executable will set up its own via arch_prctl. */
	cur->fs_base = 0;
	wrmsr(IA32_FS_BASE, 0);

	/* Clear child tid pointer -- old address space is gone. */
	cur->clear_child_tid = 0;

	/* Switch to new address space. */
	syscall_set_user_cr3(new_pml4);
	write_cr3(new_pml4);

	/* Only free old page table if not shared with another thread. */
	{
		int shared = 0;
		for (int i = 0; i < MAX_PROCS; i++) {
			if (&procs[i] != cur && procs[i].state != PROC_UNUSED &&
			    procs[i].pml4_phys == old_pml4) {
				shared = 1;
				break;
			}
		}
		if (!shared)
			paging_free_user_pml4(old_pml4);
	}

	/* Wake vfork parent -- exec created a new address space, parent is safe. */
	if (cur->vfork_parent) {
		int wake_chan = cur->pid | 0x80000000;
		cur->vfork_parent = 0;
		sched_wakeup(wake_chan);
	}

	f->rip = info.entry;
	f->rsp = rsp;
	f->rax = 0;
	f->rdi = 0;
	f->rsi = 0;
	f->rdx = 0;

	return 0;
}

static uint64_t
do_execve_path(syscall_frame_t * f, const char *resolved_path,
	       uint64_t user_argv, uint64_t user_envp)
{
	const char **kargv = 0;
	const char **kenvp = 0;
	char *strbuf = 0;
	int argc = 0;
	int envc = 0;

	if (user_argv || user_envp) {
		{
			USER_ACCESS_BEGIN();
			if (user_argv) {
				uint64_t *argv_ptrs = (uint64_t *) user_argv;
				while (argv_ptrs[argc] != 0)
					argc++;
			}
			if (user_envp) {
				uint64_t *envp_ptrs = (uint64_t *) user_envp;
				while (envp_ptrs[envc] != 0)
					envc++;
			}
			USER_ACCESS_END();
		}

		if (argc > 0)
			kargv =
			    (const char **)kmalloc((uint64_t) (argc + 1) * 8);
		if (envc > 0)
			kenvp =
			    (const char **)kmalloc((uint64_t) (envc + 1) * 8);
		strbuf = (char *)kmalloc(EXECVE_TOTAL_MAX);

		if ((argc > 0 && !kargv) || (envc > 0 && !kenvp) || !strbuf) {
			console_write("execve_path ENOMEM: argv=");
			console_hex64((uint64_t) (uintptr_t) kargv);
			console_write(" envp=");
			console_hex64((uint64_t) (uintptr_t) kenvp);
			console_write(" strbuf=");
			console_hex64((uint64_t) (uintptr_t) strbuf);
			console_write(" argc=");
			console_hex64((uint64_t) argc);
			console_write(" envc=");
			console_hex64((uint64_t) envc);
			console_write(" path=");
			console_write(resolved_path);
			console_write("\n");
			if (kargv)
				kfree(kargv);
			if (kenvp)
				kfree(kenvp);
			if (strbuf)
				kfree(strbuf);
			f->rax = SYSCALL_ERR(ENOMEM);
			return f->rax;
		}

		uint64_t stroff = 0;
		int err = 0;

		USER_ACCESS_BEGIN();

		if (argc > 0)
			err =
			    copy_user_strings(user_argv, argc, kargv, strbuf,
					      &stroff);
		if (!err && envc > 0)
			err =
			    copy_user_strings(user_envp, envc, kenvp, strbuf,
					      &stroff);

		USER_ACCESS_END();

		if (err) {
			if (kargv)
				kfree(kargv);
			if (kenvp)
				kfree(kenvp);
			if (strbuf)
				kfree(strbuf);
			f->rax = SYSCALL_ERR(E2BIG);
			return f->rax;
		}
	}

	if (argc == 0) {
		kargv = (const char **)kmalloc(2 * 8);
		if (!kargv) {
			if (strbuf)
				kfree(strbuf);
			if (kenvp)
				kfree(kenvp);
			f->rax = SYSCALL_ERR(ENOMEM);
			return f->rax;
		}
		kargv[0] = resolved_path;
		argc = 1;
	}

	return do_execve_inner(f, resolved_path, kargv, argc, kenvp, envc,
			       strbuf, 0);
}

uint64_t
do_execve(syscall_frame_t * f)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	const char *user_path = (const char *)f->rdi;
	uint64_t user_argv = f->rsi;
	uint64_t user_envp = f->rdx;

	char pathbuf[EMBER_PATH_MAX];
	char resolved_path[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	resolve_path(pathbuf, resolved_path, sizeof(resolved_path));

	return do_execve_path(f, resolved_path, user_argv, user_envp);
}

uint64_t
do_execveat(syscall_frame_t * f)
{
	int dirfd = (int)f->rdi;
	const char *user_path = (const char *)f->rsi;
	uint64_t user_argv = f->rdx;
	uint64_t user_envp = f->r10;
	int flags = (int)f->r8;

	char pathbuf[EMBER_PATH_MAX];
	char resolved[EMBER_PATH_MAX];
	copy_path_from_user(user_path, pathbuf, sizeof(pathbuf));
	if ((flags & AT_EMPTY_PATH) && pathbuf[0] == '\0') {
		fd_entry_t *de = fd_get(dirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		const char *p = de->desc->node->path;
		int i;
		for (i = 0; i < EMBER_PATH_MAX - 1 && p[i]; i++)
			resolved[i] = p[i];
		resolved[i] = '\0';
	} else if (pathbuf[0] == '/' || dirfd == AT_FDCWD) {
		resolve_path(pathbuf, resolved, sizeof(resolved));
	} else {
		fd_entry_t *de = fd_get(dirfd);
		if (!de || !de->desc->node || !de->desc->node->path) {
			f->rax = SYSCALL_ERR(EBADF);
			return f->rax;
		}
		uint64_t bi = 0;
		const char *bp = de->desc->node->path;
		while (bp[bi] && bi < EMBER_PATH_MAX - 2) {
			resolved[bi] = bp[bi];
			bi++;
		}
		resolved[bi++] = '/';
		uint64_t pi = 0;
		while (pathbuf[pi] && bi < EMBER_PATH_MAX - 1) {
			resolved[bi++] = pathbuf[pi++];
		}
		resolved[bi] = '\0';
	}

	return do_execve_path(f, resolved, user_argv, user_envp);
}
