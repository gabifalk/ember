/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/console.h"
#include "ember/isr.h"
#include "ember/vectors.h"
#include "ember/proc.h"
#include "ember/signal.h"
#include "ember/sched.h"
#include "ember/syscall.h"
#include "ember/mmu.h"
#include "ember/paging.h"
#include "ember/bug.h"
#include "ember/bkl.h"
#include "ember/pmm.h"

/* Defined in syscall_sig.c. */
void signal_deliver_isr(isr_frame_t * frame, int sig, proc_t * cur);

/* Defined in syscall_proc.c. */
void do_exit_from_isr(int sig);

/* Debug: last syscall tracking from syscall.c. */
extern volatile uint64_t g_last_syscall_nr;
extern volatile uint64_t g_last_syscall_ret;
extern volatile uint64_t g_last_syscall_arg0;
extern volatile uint64_t g_last_syscall_arg1;
extern volatile uint64_t g_last_syscall_arg2;

static void
write_hex_u64(uint64_t v)
{
	for (int i = 60; i >= 0; i -= 4) {
		uint8_t nib = (uint8_t) ((v >> i) & 0xF);
		char c =
		    (nib < 10) ? (char)('0' + nib) : (char)('a' + (nib - 10));
		console_putc(c);
	}
}

/* Map exception vector to signal number. */
static int
vector_to_signal(uint64_t vector)
{
	switch (vector) {
	case VEC_DE:
		return SIGFPE;	/* divide error. */
	case VEC_UD:
		return SIGILL;	/* undefined opcode. */
	case VEC_NP:
		return SIGSEGV;	/* segment not present. */
	case VEC_SS:
		return SIGSEGV;	/* stack segment fault. */
	case VEC_GP:
		return SIGSEGV;	/* general protection. */
	case VEC_PF:
		return SIGSEGV;	/* page fault. */
	default:
		return SIGSEGV;
	}
}

int
isr_handler(isr_frame_t * frame)
{
	BUG_ON(cpu_count > 1 && !bkl_held_by_this_cpu());
	/* #DB (vector 1): clear DR6 and resume. */
	if (frame->vector == VEC_DB) {
		uint64_t z = 0;
		__asm__ __volatile__("mov %0, %%dr6"::"r"(z));
		return 0;
	}

	/*
	 * CoW page fault handling -- must run for both kernel and user mode,
	 * since the kernel writes to user CoW pages during syscalls (read, etc.)
	 */
	if (frame->vector == VEC_PF) {
		uint64_t fault_addr;
		__asm__ __volatile__("mov %%cr2, %0":"=r"(fault_addr));
		uint64_t err = frame->error_code;
		/* Write fault (bit 1) on present page (bit 0) in user-space address -> possible COW. */
		if ((err & 0x3) == 0x3 && fault_addr < 0x0000800000000000ULL) {
			proc_t *cur = current_proc;
			if (cur
			    && paging_handle_cow(cur->pml4_phys, fault_addr))
				return 0;	/* Handled, resume. */
		}
	}

	uint64_t cpl = frame->cs & 3;

	if (cpl == 3) {
		/* User-mode fault. */
		if (frame->vector == VEC_BP) {
			/* Int3 breakpoint -- non-fatal, resume. */
			return 0;
		}

		proc_t *cur = current_proc;
		int sig = vector_to_signal(frame->vector);

		if (cur) {
			uint64_t handler = cur->sig_handlers[sig];

			if (handler == SIG_IGN) {
				/* Ignored: resume (re-execute faulting instruction) */
				return 0;
			}

			if (handler > SIG_IGN) {
				/* User signal handler: deliver via ISR frame. */
				signal_deliver_isr(frame, sig, cur);
				return 0;	/* Iretq into handler. */
			}

			/* SIG_DFL: terminate -- full register dump. */
			console_write("=== USER FAULT ===\n");
			console_write("vec=");
			write_hex_u64(frame->vector);
			console_write(" err=");
			write_hex_u64(frame->error_code);
			console_write(" rip=");
			write_hex_u64(frame->rip);
			if (frame->vector == VEC_PF) {
				uint64_t cr2;
				__asm__ __volatile__("mov %%cr2, %0":"=r"(cr2));
				console_write(" cr2=");
				write_hex_u64(cr2);
			}
			console_write("\n");
			console_write("rax=");
			write_hex_u64(frame->rax);
			console_write(" rbx=");
			write_hex_u64(frame->rbx);
			console_write(" rcx=");
			write_hex_u64(frame->rcx);
			console_write("\n");
			console_write("rdx=");
			write_hex_u64(frame->rdx);
			console_write(" rsi=");
			write_hex_u64(frame->rsi);
			console_write(" rdi=");
			write_hex_u64(frame->rdi);
			console_write("\n");
			console_write("rbp=");
			write_hex_u64(frame->rbp);
			console_write(" rsp=");
			write_hex_u64(frame->rsp);
			console_write("\n");
			console_write("r8=");
			write_hex_u64(frame->r8);
			console_write(" r9=");
			write_hex_u64(frame->r9);
			console_write(" r10=");
			write_hex_u64(frame->r10);
			console_write("\n");
			console_write("r11=");
			write_hex_u64(frame->r11);
			console_write(" r12=");
			write_hex_u64(frame->r12);
			console_write(" r13=");
			write_hex_u64(frame->r13);
			console_write("\n");
			console_write("r14=");
			write_hex_u64(frame->r14);
			console_write(" r15=");
			write_hex_u64(frame->r15);
			console_write("\n");
			console_write("last_syscall=");
			write_hex_u64(g_last_syscall_nr);
			console_write(" ret=");
			write_hex_u64(g_last_syscall_ret);
			console_write("\n");
			console_write("  arg0=");
			write_hex_u64(g_last_syscall_arg0);
			console_write(" arg1=");
			write_hex_u64(g_last_syscall_arg1);
			console_write(" arg2=");
			write_hex_u64(g_last_syscall_arg2);
			console_write("\n");
			/* Dump instruction bytes at rip. */
			console_write("code@rip: ");
			{
				uint64_t ucr3 = 0;
				if (cur)
					ucr3 = cur->pml4_phys;
				if (ucr3) {
					for (int bi = 0; bi < 16; bi++) {
						uint64_t va =
						    frame->rip + (uint64_t) bi;
						uint64_t *pte =
						    paging_walk_pte(ucr3, va);
						if (pte && (*pte & 1)) {
							uint64_t pa =
							    (*pte &
							     0x000FFFFFFFFFF000ULL)
							    + (va & 0xFFF);
							uint8_t byte =
							    *(uint8_t *)
							    phys_to_virt(pa);
							write_hex_u64((uint64_t)
								      byte);
							console_write(" ");
						} else {
							console_write("?? ");
						}
					}
				}
			}
			console_write("\n");
			/*
			 * Dump code at RCX (sysretq return addr -- where the process
			 * actually returned to before crashing)
			 */
			if (frame->rcx > 0x1000
			    && frame->rcx < 0x800000000000ULL) {
				console_write("code@rcx(");
				write_hex_u64(frame->rcx);
				console_write("): ");
				uint64_t ucr3 = cur ? cur->pml4_phys : 0;
				if (ucr3) {
					for (int bi = 0; bi < 32; bi++) {
						uint64_t va =
						    frame->rcx - 16 +
						    (uint64_t) bi;
						uint64_t *pte =
						    paging_walk_pte(ucr3, va);
						if (pte && (*pte & 1)) {
							uint64_t pa =
							    (*pte &
							     0x000FFFFFFFFFF000ULL)
							    + (va & 0xFFF);
							uint8_t byte =
							    *(uint8_t *)
							    phys_to_virt(pa);
							write_hex_u64((uint64_t)
								      byte);
							console_write(" ");
						} else {
							console_write("?? ");
						}
						if (bi == 15)
							console_write("| ");
					}
				}
				console_write("\n");
			}
			/* Dump user stack (8 qwords at RSP) */
			if (frame->rsp > 0x1000
			    && frame->rsp < 0x800000000000ULL) {
				console_write("stack@rsp:\n");
				uint64_t ucr3 = cur ? cur->pml4_phys : 0;
				if (ucr3) {
					for (int si = 0; si < 8; si++) {
						uint64_t va =
						    frame->rsp +
						    (uint64_t) (si * 8);
						console_write("  ");
						write_hex_u64(va);
						console_write(": ");
						/* Read 8 bytes via page table walk. */
						uint64_t val = 0;
						int ok = 1;
						for (int b = 0; b < 8; b++) {
							uint64_t bva =
							    va + (uint64_t) b;
							uint64_t *pte =
							    paging_walk_pte
							    (ucr3, bva);
							if (pte && (*pte & 1)) {
								uint64_t pa =
								    (*pte &
								     0x000FFFFFFFFFF000ULL)
								    +
								    (bva &
								     0xFFF);
								val |=
								    (uint64_t)
								    (*
								     (uint8_t *)
								     phys_to_virt
								     (pa)) << (b
									       *
									       8);
							} else {
								ok = 0;
								break;
							}
						}
						if (ok)
							write_hex_u64(val);
						else
							console_write
							    ("????????????????");
						console_write("\n");
					}
				}
			}
			/* PID and exe. */
			console_write("pid=");
			{
				uint64_t pid = cur ? (uint64_t) cur->pid : 0;
				write_hex_u64(pid);
			}
			if (cur && cur->exe_path[0]) {
				console_write(" exe=");
				console_write(cur->exe_path);
			}
			console_write("\n");
			/* Dump PTE for stack page and faulting address. */
			{
				uint64_t ucr3 = cur ? cur->pml4_phys : 0;
				if (ucr3) {
					uint64_t addrs[2] =
					    { frame->rsp, frame->rip };
					const char *names[2] = { "rsp", "rip" };
					for (int ai = 0; ai < 2; ai++) {
						uint64_t va = addrs[ai];
						console_write("PTE(");
						console_write(names[ai]);
						console_write("=");
						write_hex_u64(va);
						console_write("): ");
						uint64_t *pte =
						    paging_walk_pte(ucr3, va);
						if (pte) {
							write_hex_u64(*pte);
							if (*pte & 1) {
								uint64_t pa =
								    *pte &
								    0x000FFFFFFFFFF000ULL;
								console_write
								    (" PA=");
								write_hex_u64
								    (pa);
								if (*pte & 2)
									console_write
									    (" W");
								else
									console_write
									    (" R");
								if (*pte & 4)
									console_write
									    (" U");
								if (*pte &
								    (1ULL <<
								     63))
									console_write
									    (" NX");
								if (*pte &
								    (1ULL << 9))
									console_write
									    (" COW");
								/* Check PMM refcount for use-after-free. */
								uint16_t rc =
								    pmm_page_refcount
								    (pa);
								console_write
								    (" rc=");
								write_hex_u64((uint64_t) rc);
							} else {
								console_write
								    (" NOT-PRESENT");
							}
						} else {
							console_write("NO-PTE");
						}
						console_write("\n");
					}
				}
			}
			do_exit_from_isr(sig);
			/*
			 * do_exit_from_isr never returns for the dead process,
			 * but if it does (shouldn't happen), fall through to kill.
			 */
		}

		return 1;	/* Killed. */
	}

	/*
	 * Kernel fault -- panic.  Print vector via direct serial FIRST
	 * in case the frame is on a corrupted stack.
	 */
	{
		uint64_t v = frame->vector;
		serial_putc('P');
		serial_putc('!');
		serial_putc("0123456789abcdef"[(v >> 4) & 0xf]);
		serial_putc("0123456789abcdef"[v & 0xf]);
		serial_putc(' ');
		serial_flush();
	}
	console_write("KERNEL PANIC vec=");
	write_hex_u64(frame->vector);
	console_write(" err=");
	write_hex_u64(frame->error_code);
	console_write(" rip=");
	write_hex_u64(frame->rip);
	if (frame->vector == VEC_PF) {
		uint64_t cr2;
		__asm__ __volatile__("mov %%cr2, %0":"=r"(cr2));
		console_write(" cr2=");
		write_hex_u64(cr2);
		/* Walk page table for faulting address. */
		console_write("\nPTE walk for cr2:\n");
		uint64_t cr3;
		__asm__ __volatile__("mov %%cr3, %0":"=r"(cr3));
		console_write("  cr3=");
		write_hex_u64(cr3);
		uint64_t pml4_phys = cr3 & 0x000FFFFFFFFFF000ULL;
		uint64_t *pml4 =
		    (uint64_t *) (0xffff800000000000ULL + pml4_phys);
		uint64_t i4 = (cr2 >> 39) & 0x1FF;
		console_write(" pml4[");
		write_hex_u64(i4);
		console_write("]=");
		write_hex_u64(pml4[i4]);
		console_write("\n");
		if (pml4[i4] & 1) {
			uint64_t *pdpt =
			    (uint64_t *) (0xffff800000000000ULL +
					  (pml4[i4] & 0x000FFFFFFFFFF000ULL));
			uint64_t i3 = (cr2 >> 30) & 0x1FF;
			console_write("  pdpt[");
			write_hex_u64(i3);
			console_write("]=");
			write_hex_u64(pdpt[i3]);
			console_write("\n");
			if (pdpt[i3] & 1) {
				uint64_t *pd =
				    (uint64_t *) (0xffff800000000000ULL +
						  (pdpt[i3] &
						   0x000FFFFFFFFFF000ULL));
				uint64_t i2 = (cr2 >> 21) & 0x1FF;
				console_write("  pd[");
				write_hex_u64(i2);
				console_write("]=");
				write_hex_u64(pd[i2]);
				console_write("\n");
				if (pd[i2] & 1) {
					uint64_t *pt =
					    (uint64_t *) (0xffff800000000000ULL
							  +
							  (pd[i2] &
							   0x000FFFFFFFFFF000ULL));
					uint64_t i1 = (cr2 >> 12) & 0x1FF;
					console_write("  pt[");
					write_hex_u64(i1);
					console_write("]=");
					write_hex_u64(pt[i1]);
					console_write("\n");
				}
			}
		}
	}
	console_write("\nRegisters:\n");
	console_write("  rax=");
	write_hex_u64(frame->rax);
	console_write(" rbx=");
	write_hex_u64(frame->rbx);
	console_write(" rcx=");
	write_hex_u64(frame->rcx);
	console_write(" rdx=");
	write_hex_u64(frame->rdx);
	console_write("\n");
	console_write("  rsi=");
	write_hex_u64(frame->rsi);
	console_write(" rdi=");
	write_hex_u64(frame->rdi);
	console_write(" rbp=");
	write_hex_u64(frame->rbp);
	console_write(" rsp=");
	write_hex_u64(frame->rsp);
	console_write("\n");
	console_write("  r8=");
	write_hex_u64(frame->r8);
	console_write(" r9=");
	write_hex_u64(frame->r9);
	console_write(" r10=");
	write_hex_u64(frame->r10);
	console_write(" r11=");
	write_hex_u64(frame->r11);
	console_write("\n");
	console_write("  r12=");
	write_hex_u64(frame->r12);
	console_write(" r13=");
	write_hex_u64(frame->r13);
	console_write(" r14=");
	write_hex_u64(frame->r14);
	console_write(" r15=");
	write_hex_u64(frame->r15);
	console_write("\n");
	console_write("  cs=");
	write_hex_u64(frame->cs);
	console_write(" rflags=");
	write_hex_u64(frame->rflags);
	console_write("\nStack dump (from rsp):\n");
	{
		uint64_t *sp = (uint64_t *) frame->rsp;
		for (int i = 0; i < 16; i++) {
			console_write("  ");
			write_hex_u64((uint64_t) (uintptr_t) & sp[i]);
			console_write(": ");
			write_hex_u64(sp[i]);
			console_write("\n");
		}
	}
	console_write("\n");
	for (;;) {
		__asm__ __volatile__("hlt");
	}
}
