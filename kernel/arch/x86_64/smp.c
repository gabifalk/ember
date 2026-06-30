/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

#include <stdint.h>
#include "ember/smp.h"
#include "ember/cpu.h"
#include "ember/acpi.h"
#include "ember/lapic.h"
#include "ember/console.h"
#include "ember/pmm.h"
#include "ember/paging.h"
#include "ember/mmu.h"
#include "ember/io.h"
#include "ember/bkl.h"
#include "ember/sched.h"
#include "ember/spinlock.h"
#include "ember/bug.h"
#include "ember/kexec.h"

/* Portable atomic helpers (no __atomic builtins -- tcc lacks them). */
static inline int
atomic_add_fetch_int(volatile int *p, int v)
{
	int old = v;
	__asm__ __volatile__("lock xadd %0, %1"
			     : "+r"(old), "+m"(*p) :: "memory");
	return old + v;
}

static inline int
atomic_load_int(volatile int *p)
{
	int v;
	__asm__ __volatile__("mov %1, %0" : "=r"(v) : "m"(*p) : "memory");
	return v;
}

/* Forward declarations for per-CPU init. */
extern void gdt_init_cpu(int cpu_id, uint64_t kstack_top);
extern uint64_t idt_base_addr(void);
extern void syscall_init_ap(int cpu_id, uint64_t kstack_top);

/*
 * Per-AP boot info array (indexed by trampoline slot order).
 * With unicast SIPI only cpu_count-1 APs wake, so MAX_CPUS is enough.
 * Verified: models/ap_boot_slot_race.pml.
 */
static struct ap_boot_info ap_info[MAX_CPUS];

/*
 * Serialize AP init that touches shared state (heap, PMM).
 * spinlock_t is cli/sti only -- not SMP-safe.  This atomic spinlock
 * protects gdt_init_cpu() which calls kmalloc.
 * Verified: models/ap_boot_race.pml.
 */
static atomic_spinlock_t ap_init_lock = ATOMIC_SPINLOCK_INIT;

static volatile int ap_ready_count = 0;

/* ---- Decimal print helper (kernel has no kprintf) ---- */

static void
print_int(int val)
{
	char buf[16];
	int pos = 0;
	if (val < 0) {
		console_putc('-');
		val = -val;
	}
	if (val == 0) {
		console_putc('0');
		return;
	}
	while (val > 0) {
		buf[pos++] = '0' + (val % 10);
		val /= 10;
	}
	while (pos > 0)
		console_putc(buf[--pos]);
}

/* ---- TSC-based delay ---- */

static uint64_t tsc_per_ms;

static inline uint64_t
rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ __volatile__("rdtsc":"=a"(lo), "=d"(hi));
	return ((uint64_t) hi << 32) | lo;
}

static void
calibrate_tsc(void)
{
	/* Use PIT channel 2 for a ~10ms reference. */
	outb(0x61, (inb(0x61) & 0xFD) | 1);	/* Gate on, speaker off. */
	outb(0x43, 0xB0);	/* Ch2, lobyte/hibyte, mode 0. */
	uint16_t count = 11932;	/* ~10Ms at 1.193182 MHz. */
	outb(0x42, count & 0xFF);
	outb(0x42, count >> 8);

	uint64_t start = rdtsc();
	while (!(inb(0x61) & 0x20))
		__asm__ __volatile__("pause");
	uint64_t end = rdtsc();

	tsc_per_ms = (end - start) / 10;
}

static void
tsc_delay_ms(uint64_t ms)
{
	uint64_t start = rdtsc();
	uint64_t target = start + ms * tsc_per_ms;
	while (rdtsc() < target)
		__asm__ __volatile__("pause");
}

/* ---- AP C entry point ---- */

/*
 * Called from trampoline after mode transition. At this point:
 * - Long mode, paging enabled (BSP's CR3), interrupts disabled
 * - RSP = per-AP kernel stack from ap_info[slot].stack_top
 * - RDI = ap_info[slot].cpu_local_ptr (we stash stack_top here)
 *
 * The AP needs to:
 * 1. Determine its CPU ID from LAPIC
 * 2. Initialize per-CPU GDT+TSS
 * 3. Load shared IDT
 * 4. Initialize syscall MSRs
 * 5. Initialize its LAPIC
 * 6. Signal BSP it's ready
 * 7. Enter idle loop (sti; hlt)
 */
void
ap_entry_64(void *cpu_local_ptr)
{
	/* cpu_local_ptr carries our stack_top (set in smp_init) */
	uint64_t kstack_top = (uint64_t) cpu_local_ptr;

	BUG_ON(!kstack_top);
	BUG_ON(kstack_top < 0xffff800000000000ULL);	/* Must be HHDM/kernel VA. */

	/* Read our LAPIC ID and look up CPU number. */
	uint32_t my_lapic_id = lapic_id();
	BUG_ON(my_lapic_id > 255);

	int my_cpu = lapic_to_cpu[my_lapic_id];

	if (my_cpu < 0 || my_cpu >= cpu_count) {
		/* Excess AP or unknown LAPIC ID -- halt forever. */
		for (;;)
			__asm__ __volatile__("cli; hlt");
	}

	BUG_ON(my_cpu < 0 || my_cpu >= MAX_CPUS);

	/*
	 * Enable SSE/FPU (required for musl-libc, user-space SSE instructions).
	 * Without this, processes on AP hit #UD on any SSE instruction.
	 */
	{
		uint64_t cr4;
		__asm__ __volatile__("mov %%cr4, %0":"=r"(cr4));
		cr4 |= (1u << 9) | (1u << 10);	/* OSFXSR + OSXMMEXCPT. */
		__asm__ __volatile__("mov %0, %%cr4"::"r"(cr4));
		uint64_t cr0;
		__asm__ __volatile__("mov %%cr0, %0":"=r"(cr0));
		cr0 &= ~(1ULL << 2);
		cr0 |= (1ULL << 1);	/* Clear EM, set MP. */
		__asm__ __volatile__("mov %0, %%cr0"::"r"(cr0));
		__asm__ __volatile__("fninit");
	}

	/*
	 * Initialize per-CPU GDT+TSS.
	 * gdt_init_cpu calls kmalloc which uses cli/sti spinlocks (not SMP-safe).
	 * Serialize with atomic spinlock.  Verified: models/ap_boot_race.pml.
	 */
	atomic_spin_lock(&ap_init_lock);
	gdt_init_cpu(my_cpu, kstack_top);
	atomic_spin_unlock(&ap_init_lock);

	/* Load shared IDT (byte buffer -- tcc ignores __attribute__((packed))). */
	{
		uint8_t idtr[10];
		uint16_t limit = 256 * 16 - 1;
		uint64_t base = idt_base_addr();
		idtr[0] = (uint8_t)(limit & 0xFF);
		idtr[1] = (uint8_t)((limit >> 8) & 0xFF);
		for (int i = 0; i < 8; i++)
			idtr[2 + i] = (uint8_t)((base >> (i * 8)) & 0xFF);
		__asm__ __volatile__("lidt (%0)"::"r"(idtr));
	}

	/*
	 * Initialize per-CPU syscall MSRs with our own cpu_local struct.
	 * MUST be done BEFORE any code that might access gs:0/gs:8
	 * (including interrupt handlers).
	 */
	syscall_init_ap(my_cpu, kstack_top);

	/* Initialize per-CPU idle stack for schedule() */
	sched_init_idle(my_cpu);

	/* Initialize our LAPIC. */
	lapic_init();

	/* Mark this CPU as online (used by smp_flush_tlb wait loop) */
	atomic_add_fetch_int(&cpu_online_count, 1);

	/* Signal BSP we're ready. */
	atomic_add_fetch_int(&ap_ready_count, 1);

	/* Print AP ID under lock to avoid interleaved serial output. */
	atomic_spin_lock(&ap_init_lock);
	{
		static const char hex[] = "0123456789abcdef";
		while (!(inb(0x3F8 + 5) & 0x20)) ;
		outb(0x3F8, hex[(my_cpu >> 4) & 0xF]);
		while (!(inb(0x3F8 + 5) & 0x20)) ;
		outb(0x3F8, hex[my_cpu & 0xF]);
		while (!(inb(0x3F8 + 5) & 0x20)) ;
		outb(0x3F8, ' ');
	}
	atomic_spin_unlock(&ap_init_lock);

	/* Start LAPIC timer (reuse BSP's calibrated count) */
	uint32_t timer_count = lapic_get_timer_count();
	if (timer_count)
		lapic_timer_init_count(timer_count);

	/*
	 * AP idle loop: enable interrupts, then repeatedly try to schedule.
	 * The timer ISR kernel-mode path only does EOI -- scheduling must
	 * happen here. Acquire BKL before schedule(), release after.
	 */
	for (;;) {
		__asm__ __volatile__("sti; hlt; cli":::"memory");
		if (kexec_halting) {
			/* BSP requested kexec shutdown.  Increment the
			 * halted counter and stop forever.  CLI is already
			 * set from the sti;hlt;cli above. */
			atomic_add_fetch_int(&kexec_ap_halted, 1);
			for (;;)
				__asm__ __volatile__("hlt");
		}
		if (!bkl_tryacquire())
			continue;	/* BKL busy -- back to hlt, don't spin. */
		sched_wakeup(SCHED_TICK_CHAN);
		schedule();
		bkl_release();
	}
}

/* ---- BSP SMP initialization ---- */

void
smp_init(void)
{
	if (cpu_count <= 1) {
		console_write("[SMP] single CPU, skipping AP boot\n");
		return;
	}

	console_write("[SMP] booting ");
	print_int(cpu_count - 1);
	console_write(" APs\n");

	calibrate_tsc();

	/*
	 * Allocate 32KB stacks for wanted APs.  With unicast SIPI only
	 * wanted APs wake, so no excess slots to worry about.
	 */
	for (int i = 0; i < cpu_count - 1; i++) {
		uint64_t stack_phys = pmm_alloc_pages(8);
		if (!stack_phys) {
			console_write("[SMP] failed to allocate stack for AP ");
			print_int(i);
			console_write("\n");
			return;
		}
		uint64_t stack_virt = (uint64_t) phys_to_virt(stack_phys);
		ap_info[i].stack_top = stack_virt + 32768;
		/*
		 * Stash stack_top in cpu_local_ptr so ap_entry_64 can retrieve it
		 * (the trampoline loads cpu_local_ptr into RDI as first argument)
		 */
		ap_info[i].cpu_local_ptr = ap_info[i].stack_top;
	}

	/*
	 * Identity-map the trampoline page so APs can execute it
	 * after paging is enabled with BSP's CR3 (which had its
	 * identity map removed early in kmain).
	 */
	uint64_t cr3;
	__asm__ __volatile__("mov %%cr3, %0":"=r"(cr3));
	paging_map_range(cr3 & PTE_ADDR_MASK, 0x8000, 0x8000,
			 PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);

	/* Install trampoline code at physical 0x8000. */
	trampoline_install(0x8000);

	/* Fill trampoline data block. */
	struct ap_trampoline_data *td = trampoline_data(0x8000);
	td->cr3 = cr3 & PTE_ADDR_MASK;
	td->entry_64 = (uint64_t) (uintptr_t) ap_entry_64;
	/*
	 * ap_info is in kernel BSS, accessible via HHDM/kernel mapping
	 * after paging is enabled. Pass the virtual address since the
	 * trampoline dereferences it in 64-bit long mode with BSP's CR3.
	 */
	td->ap_info_phys = (uint64_t) (uintptr_t) ap_info;
	td->wake_flag = 0;
	td->ap_count = 0;
	/*
	 * Send INIT + SIPI to wanted APs only (unicast).
	 * Avoids waking excess APs that consume KVM resources even if
	 * they halt quickly.  With broadcast SIPI + 64 CPUs, the VM exit
	 * storm from 48 excess APs prevents wanted APs from completing init.
	 */
	for (int i = 1; i < cpu_count; i++)
		lapic_send_init(cpu_lapic_ids[i]);
	tsc_delay_ms(10);

	for (int i = 1; i < cpu_count; i++)
		lapic_send_sipi(cpu_lapic_ids[i], 0x08);
	tsc_delay_ms(1);

	/* Second SIPI per Intel spec. */
	for (int i = 1; i < cpu_count; i++)
		lapic_send_sipi(cpu_lapic_ids[i], 0x08);

	/*
	 * Phase 1: wait for wanted APs to reach trampoline park.
	 * With unicast SIPI only wanted APs wake, so ap_count reaches
	 * cpu_count-1.  No excess APs to worry about.
	 */
	{
		int wanted_aps = cpu_count - 1;
		uint64_t park_deadline = rdtsc() + tsc_per_ms * 5000;
		while ((int)td->ap_count < wanted_aps) {
			if (rdtsc() > park_deadline)
				break;
			__asm__ __volatile__("pause":::"memory");
		}
	}

	/* Print prefix via direct serial. */
	{
		const char *msg = "[SMP] APs online: ";
		for (const char *p = msg; *p; p++) {
			while (!(inb(0x3F8 + 5) & 0x20)) ;
			outb(0x3F8, *p);
		}
	}

	/*
	 * Phase 2: release APs from park loop -- all have valid stacks.
	 * Verified: models/ap_boot_twophase.pml (wake_after_park)
	 */
	td->wake_flag = 1;

	/* Wait for wanted APs to complete full initialization. */
	uint64_t deadline = rdtsc() + tsc_per_ms * 10000;
	int expected = cpu_count - 1;

	while (atomic_load_int(&ap_ready_count) < expected) {
		if (rdtsc() > deadline) {
			console_write("[SMP] timeout: ");
			print_int(ap_ready_count);
			console_write("/");
			print_int(expected);
			console_write(" APs ready\n");
			break;
		}
		__asm__ __volatile__("pause");
	}

	/* All APs ready -- they've printed their IDs. Print newline. */
	while (!(inb(0x3F8 + 5) & 0x20)) ;
	outb(0x3F8, '\n');
}

/*
 * -- TLB shootdown IPI ----------------------------------------------
 * Flush TLB on all other CPUs.  Called after modifying PTEs that may
 * be cached on remote CPUs running the same process in user mode.
 * The IPI handler (isr_tlb_shootdown, vector 0x40) does EOI + mov cr3,cr3
 * and increments tlb_ack_count.  smp_flush_tlb waits for all online CPUs
 * to acknowledge before returning, ensuring no stale TLB entries remain.
 *
 * Without the wait, there is a race window (found by cow_phys.pml):
 * 1. CPU A: munmap -> clear PTE -> free page -> send IPI (no wait)
 * 2. CPU B: still has stale TLB entry -> reads/writes freed page
 * 3. CPU C: allocates freed page -> zeroes it for mmap
 * 4. CPU B: reads zeros through stale TLB -> user stack corruption
 *
 * Verified: models/cow_phys.pml.
 */
#define TLB_SHOOTDOWN_VECTOR 0x41	/* Separate from sched_kick (0x40) */

volatile int tlb_ack_count;

void
tlb_shootdown_ack(void)
{
	__asm__ __volatile__("lock incl %0":"+m"(tlb_ack_count)::"memory");
}

void
smp_flush_tlb(void)
{
	if (cpu_count <= 1)
		return;
	if (!lapic_enabled)
		return;

	int expected = cpu_online_count - 1;
	tlb_ack_count = 0;
	__asm__ __volatile__("":::"memory");	/* Compiler barrier. */

	lapic_send_ipi_all_excl_self(TLB_SHOOTDOWN_VECTOR);

	/*
	 * Wait for all other CPUs to acknowledge the flush.
	 * sti;pause;cli: allow interrupts during wait so that:
	 * 1. Timer ticks are processed (keeps kernel_ticks advancing)
	 * 2. This CPU can respond to IPIs from other CPUs if needed
	 * 3. QEMU/KVM can schedule vCPUs that need to process our IPI.
	 */
	{
		uint64_t spins = 0;
		while (tlb_ack_count < expected) {
			__asm__ __volatile__("sti; pause; cli":::"memory");
			if (++spins % 500000000ULL == 0) {
				extern void console_hex64(uint64_t v);
				console_write("TLB wait: ");
				console_hex64((uint64_t) tlb_ack_count);
				console_write("/");
				console_hex64((uint64_t) expected);
				console_write("\n");
			}
		}
	}
}
