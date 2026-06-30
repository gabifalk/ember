/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

#include <stdint.h>

#include "ember/console.h"
#include "ember/cpu.h"
#include "ember/desc.h"
#include "ember/heap.h"

#define TSS64_SIZE 104
#define GDT_ENTRIES 7
#define SEL_TSS 0x28

struct tss64 {
	/*
	 * TCC ignores __attribute__((packed)).
	 */
	uint32_t reserved0;
	uint32_t rsp0_lo, rsp0_hi;
	uint32_t rsp1_lo, rsp1_hi;
	uint32_t rsp2_lo, rsp2_hi;
	uint32_t reserved1, reserved2;
	uint32_t ist1_lo, ist1_hi;
	uint32_t ist2_lo, ist2_hi;
	uint32_t ist3_lo, ist3_hi;
	uint32_t ist4_lo, ist4_hi;
	uint32_t ist5_lo, ist5_hi;
	uint32_t ist6_lo, ist6_hi;
	uint32_t ist7_lo, ist7_hi;
	uint32_t reserved3, reserved4;
	uint32_t iomap_base;
};

static uint64_t *percpu_gdt[MAX_CPUS];
static struct tss64 *percpu_tss[MAX_CPUS];

#define DF_STACK_SIZE 16384
static uint8_t df_stacks[MAX_CPUS][DF_STACK_SIZE] __attribute__ ((aligned(16)));

static void
tss_set_ist1(struct tss64 * t, uint64_t val)
{
	t->ist1_lo = (uint32_t) (val);
	t->ist1_hi = (uint32_t) (val >> 32);
}

static struct tss64 tss __attribute__ ((aligned(16)));
static uint8_t tss_stack[16384] __attribute__ ((aligned(16)));
static uint64_t gdt[GDT_ENTRIES] __attribute__ ((aligned(16)));

uint64_t
bsp_tss_addr(void)
{
	return (uint64_t)&tss;
}

static void
tss_set_rsp0(struct tss64 * t, uint64_t val)
{
	t->rsp0_lo = (uint32_t) (val);
	t->rsp0_hi = (uint32_t) (val >> 32);
}

void
tss_update_rsp0(uint64_t val)
{
	int cpu = this_cpu_id();
	if (percpu_tss[cpu])
		tss_set_rsp0(percpu_tss[cpu], val);
	else
		tss_set_rsp0(&tss, val);
}

void
tss_update_rsp0_cpu(int cpu_id, uint64_t val)
{
	if (percpu_tss[cpu_id])
		tss_set_rsp0(percpu_tss[cpu_id], val);
}

static void
tss_set_iomap(struct tss64 * t, uint16_t base)
{
	t->iomap_base = (uint32_t) base << 16;
}

extern void gdt_load(void *p);
extern void tss_load(uint16_t sel, uint64_t new_rsp);

static uint64_t
gdt_entry(uint32_t base, uint32_t limit, uint16_t flags)
{
	uint64_t desc = 0;
	desc |= (limit & 0xFFFFULL);
	desc |= ((uint64_t) (base & 0xFFFFFF) << 16);
	desc |= ((uint64_t) flags << 40);
	desc |= ((uint64_t) ((limit >> 16) & 0xF) << 48);
	desc |= ((uint64_t) ((base >> 24) & 0xFF) << 56);
	return desc;
}

static void
gdt_populate(uint64_t * g)
{
	g[0] = 0;
	g[1] = gdt_entry(0, 0xFFFFF, GDT_P | GDT_DPL(0) | GDT_S | GDT_EXEC | GDT_RW) | GDT_LONG | GDT_GRAN;	/* kernel code */
	g[2] = gdt_entry(0, 0xFFFFF, GDT_P | GDT_DPL(0) | GDT_S | GDT_RW) | GDT_GRAN;				/* kernel data */
	g[3] = gdt_entry(0, 0xFFFFF, GDT_P | GDT_DPL(3) | GDT_S | GDT_RW) | GDT_GRAN;				/* user data */
	g[4] = gdt_entry(0, 0xFFFFF, GDT_P | GDT_DPL(3) | GDT_S | GDT_EXEC | GDT_RW) | GDT_LONG | GDT_GRAN;	/* user code */
}

static void
gdt_set_tss(uint64_t * g, uint64_t base, uint32_t limit)
{
	g[5] = gdt_entry((uint32_t) base, limit, GDT_P | GDT_DPL(0) | DESC_TSS_AVAIL);
	g[6] = base >> 32;
}

static void
gdt_do_load(uint64_t * g)
{
	uint8_t gdtr[10];
	*(uint16_t *) & gdtr[0] = (uint16_t) (GDT_ENTRIES * 8 - 1);
	*(uint64_t *) & gdtr[2] = (uint64_t) g;
	gdt_load((void *)gdtr);
}

void
gdt_init(void)
{
	gdt_populate(gdt);

	uint8_t *tss_bytes = (uint8_t *) &tss;
	for (uint32_t i = 0; i < sizeof(tss); i++) {
		tss_bytes[i] = 0;
	}

	tss_set_rsp0(&tss, (uint64_t) (tss_stack + sizeof(tss_stack)));
	tss_set_ist1(&tss, (uint64_t) (df_stacks[0] + DF_STACK_SIZE));
	tss_set_iomap(&tss, TSS64_SIZE);
	gdt_set_tss(gdt, (uint64_t) &tss, TSS64_SIZE - 1);

	gdt_do_load(gdt);

	percpu_gdt[0] = gdt;
	percpu_tss[0] = &tss;
}

void
gdt_init_cpu(int cpu_id, uint64_t kstack_top)
{
	uint64_t *g = (uint64_t *) kmalloc(GDT_ENTRIES * 8);
	struct tss64 *t = (struct tss64 *) kzalloc(128);	/* 104 Bytes, overallocate for alignment. */
	if (!g || !t) {
		for (;;)
			__asm__ __volatile__("cli; hlt");
	}

	percpu_gdt[cpu_id] = g;
	percpu_tss[cpu_id] = t;

	gdt_populate(g);

	tss_set_rsp0(t, kstack_top);

	tss_set_ist1(t, (uint64_t) (df_stacks[cpu_id] + DF_STACK_SIZE));

	tss_set_iomap(t, TSS64_SIZE);

	gdt_set_tss(g, (uint64_t) t, TSS64_SIZE - 1);

	gdt_do_load(g);

	tss_load(SEL_TSS, kstack_top);
}

void
tss_init(void)
{
	uint64_t new_rsp = (uint64_t) (tss_stack + sizeof(tss_stack));
	tss_load(SEL_TSS, new_rsp);
}
