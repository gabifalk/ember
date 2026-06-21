/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/user.h"
#include "ember/paging.h"
#include "ember/pmm.h"
#include "ember/vfs.h"
#include "ember/elf.h"
#include "ember/console.h"
#include "ember/syscall.h"
#include "ember/mmu.h"
#include "ember/proc.h"
#include "ember/fd.h"
#include "ember/sched.h"
#include "ember/heap.h"

extern int elf_load_user(vfs_node_t * node, uint64_t pml4, elf_info_t * info);

#define USER_STACK_TOP  0x00007fffffffe000ULL
#define USER_STACK_PAGES 512

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_RANDOM 25
#define AT_HWCAP2 26

static uint64_t
map_user_stack(uint64_t pml4)
{
	uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

	for (uint64_t i = 0; i < USER_STACK_PAGES; i++) {
		uint64_t pa = pmm_alloc_page();
		if (pa == UINT64_MAX)
			return 0;
		/*
		 * Zero page via HHDM -- matches mmap/brk behavior and prevents
		 * info leak from recycled pages.
		 */
		uint8_t *kva = (uint8_t *) phys_to_virt(pa);
		kmemzero(kva, PAGE_SIZE);
		paging_map_range(pml4, stack_base + i * PAGE_SIZE, pa,
				 PAGE_SIZE,
				 PTE_PRESENT | PTE_USER | PTE_WRITABLE);
	}

	return USER_STACK_TOP;
}

void
setup_image_vmas(proc_t * p, elf_info_t * info)
{
	for (int i = 0; i < info->nsegs; i++)
		vma_add(p, info->segs[i].vaddr, info->segs[i].len,
			info->segs[i].prot);
	vma_add(p, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
		USER_STACK_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE);
}

static uint64_t
str_len(const char *s)
{
	uint64_t n = 0;
	while (s[n])
		n++;
	return n;
}

uint64_t
setup_user_stack(uint64_t pml4, elf_info_t * info)
{
	const char *argv0 = "/init";
	const char *argv[1];
	argv[0] = argv0;
	return setup_user_stack_argv(pml4, info, argv, 1, (const char **)0, 0);
}

uint64_t
setup_user_stack_argv(uint64_t pml4, elf_info_t * info,
		      const char **argv, int argc, const char **envp, int envc)
{
	uint64_t stack_top = map_user_stack(pml4);
	if (!stack_top)
		return 0;

	uint64_t old_cr3 = read_cr3();
	write_cr3(pml4);

	uint64_t sp = stack_top;

	/* 16 Bytes of pseudo-random data. */
	sp -= 16;
	uint64_t random_addr = sp;
	uint8_t *rnd = (uint8_t *) (uintptr_t) sp;
	for (int i = 0; i < 16; i++)
		rnd[i] = (uint8_t) (0x42 + i);

	/* Place argv strings at top of stack, collect their addresses. */
	int nargs = argc;
	int nenvs = envc;
	uint64_t *argv_addrs =
	    (uint64_t *) kmalloc((uint64_t) (nargs > 0 ? nargs : 1) * 8);
	uint64_t *envp_addrs =
	    (uint64_t *) kmalloc((uint64_t) (nenvs > 0 ? nenvs : 1) * 8);
	if (!argv_addrs || !envp_addrs) {
		if (argv_addrs)
			kfree(argv_addrs);
		if (envp_addrs)
			kfree(envp_addrs);
		write_cr3(old_cr3);
		return 0;
	}

	for (int i = nargs - 1; i >= 0; i--) {
		uint64_t slen = str_len(argv[i]);
		sp -= (slen + 1 + 7) & ~7ULL;	/* Align to 8. */
		argv_addrs[i] = sp;
		char *s = (char *)(uintptr_t) sp;
		for (uint64_t j = 0; j <= slen; j++)
			s[j] = argv[i][j];
	}

	/* Place envp strings, collect their addresses. */
	for (int i = nenvs - 1; i >= 0; i--) {
		uint64_t slen = str_len(envp[i]);
		sp -= (slen + 1 + 7) & ~7ULL;	/* Align to 8. */
		envp_addrs[i] = sp;
		char *s = (char *)(uintptr_t) sp;
		for (uint64_t j = 0; j <= slen; j++)
			s[j] = envp[i][j];
	}

	/*
	 * Compute total structured entries:
	 * argc(1) + argv[n] + NULL(1) + envp[n] + NULL(1) + auxv(13*2+2) = nargs+nenvs+31.
	 */
	int nslots = nargs + nenvs + 31;
	sp -= (uint64_t) nslots *8;
	sp &= ~(uint64_t) 0xF;	/* 16-Byte align. */

	uint64_t *slot = (uint64_t *) (uintptr_t) sp;
	int idx = 0;

	/* Argc. */
	slot[idx++] = (uint64_t) nargs;

	/* Argv pointers. */
	for (int i = 0; i < nargs; i++)
		slot[idx++] = argv_addrs[i];
	/* Argv terminator. */
	slot[idx++] = 0;

	/* Envp pointers. */
	for (int i = 0; i < nenvs; i++)
		slot[idx++] = envp_addrs[i];
	/* Envp terminator. */
	slot[idx++] = 0;

	/* Auxv. */
	slot[idx++] = AT_PAGESZ;
	slot[idx++] = 4096;

	slot[idx++] = AT_PHDR;
	slot[idx++] = info->phdr_vaddr;

	slot[idx++] = AT_PHENT;
	slot[idx++] = info->phentsize;

	slot[idx++] = AT_PHNUM;
	slot[idx++] = info->phnum;

	slot[idx++] = AT_ENTRY;
	slot[idx++] = info->entry;

	slot[idx++] = AT_UID;
	slot[idx++] = 0;

	slot[idx++] = AT_EUID;
	slot[idx++] = 0;

	slot[idx++] = AT_GID;
	slot[idx++] = 0;

	slot[idx++] = AT_EGID;
	slot[idx++] = 0;

	slot[idx++] = AT_CLKTCK;
	slot[idx++] = 100;	/* KERNEL_HZ. */

	slot[idx++] = AT_HWCAP;
	slot[idx++] = 0;

	slot[idx++] = AT_FLAGS;
	slot[idx++] = 0;

	slot[idx++] = AT_RANDOM;
	slot[idx++] = random_addr;

	slot[idx++] = AT_NULL;
	slot[idx++] = 0;

	kfree(argv_addrs);
	kfree(envp_addrs);

	write_cr3(old_cr3);

	return sp;
}

void
user_run_init(void)
{
	console_write("user_run_init\n");
	vfs_node_t *n = vfs_lookup("/init");
	if (!n) {
		console_write("no /init to run\n");
		return;
	}

	console_write("user pml4 create\n");
	uint64_t pml4 = paging_create_user_pml4();
	if (!pml4) {
		console_write("pml4 create failed\n");
		return;
	}

	console_write("ELF load\n");
	elf_info_t info;
	if (!elf_load_user(n, pml4, &info)) {
		console_write("ELF load failed\n");
		return;
	}

	/* Set up init process (pid 1) */
	proc_t *init = proc_alloc();
	if (!init) {
		console_write("proc_alloc failed\n");
		return;
	}
	init->ppid = 0;
	init->state = PROC_RUNNING;
	init->pml4_phys = pml4;
	init->brk = info.brk_base;
	init->brk_base = info.brk_base;
	init->mmap_next = 0x7f0000000000ULL;
	setup_image_vmas(init, &info);
	set_bsp_current_proc(init);

	/*
	 * Set up per-process fd table: stdin/stdout/stderr as console.
	 * Migrate boot fd table's file_desc pointers to init's fd table.
	 */
	for (int i = 0; i < 3; i++) {
		fd_entry_t *boot_e = fd_get_raw(i);	/* Still points to boot table. */
		if (boot_e && boot_e->desc) {
			init->fds[i].desc = boot_e->desc;
			init->fds[i].fd_flags = 0;
			boot_e->desc = 0;	/* Detach from boot table. */
		} else {
			file_desc_t *d = file_desc_alloc();
			if (d)
				d->type = FD_TYPE_CONSOLE;
			init->fds[i].desc = d;
			init->fds[i].fd_flags = 0;
		}
	}

	console_write("setup user stack\n");
	uint64_t rsp = setup_user_stack(pml4, &info);
	if (!rsp) {
		console_write("stack setup failed\n");
		return;
	}

	/* Point syscall and interrupt infrastructure to this process's kstack. */
	uint64_t kstack_top =
	    (uint64_t) (uintptr_t) (init->kstack + PROC_KSTACK_SIZE);
	syscall_set_kstack(kstack_top);
	extern void tss_update_rsp0(uint64_t val);
	tss_update_rsp0(kstack_top);

	console_write("switching to user\n");
	syscall_set_user_cr3(pml4);
	write_cr3(pml4);
	enter_user(info.entry, rsp);
}
