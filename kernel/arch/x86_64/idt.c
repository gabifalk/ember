/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/console.h"
#include "ember/desc.h"
#include "ember/vectors.h"

struct idt_entry {
	uint16_t off_low;
	uint16_t sel;
	uint8_t ist;
	uint8_t type_attr;
	uint16_t off_mid;
	uint32_t off_high;
	uint32_t zero;
} __attribute__ ((packed));

static struct idt_entry idt[256];

extern void isr_de(void);
extern void isr_db(void);
extern void isr_ud(void);
extern void isr_bp(void);
extern void isr_np(void);
extern void isr_ss(void);
extern void isr_gp(void);
extern void isr_pf(void);
extern void isr_df(void);
extern void isr_timer(void);
extern void isr_sched_kick(void);

static uint16_t
idt_cs_sel(void)
{
	uint16_t cs = 0;
	__asm__ __volatile__("mov %%cs, %0":"=r"(cs));
	return cs;
}

static void
idt_write_gate(int vec, void (*isr) (void), uint8_t ist)
{
	uint64_t addr = (uint64_t) (uintptr_t) isr;
	idt[vec].off_low = (uint16_t) (addr & 0xFFFF);
	idt[vec].sel = idt_cs_sel();
	idt[vec].ist = ist;
	if (vec == 3) {
		idt[vec].type_attr = GDT_P | GDT_DPL(3) | DESC_INT_GATE;	/* int3 from ring 3 */
	} else {
		idt[vec].type_attr = GDT_P | GDT_DPL(0) | DESC_INT_GATE;
	}
	idt[vec].off_mid = (uint16_t) ((addr >> 16) & 0xFFFF);
	idt[vec].off_high = (uint32_t) (addr >> 32);
	idt[vec].zero = 0;
}

static void
idt_set_gate(int vec, void (*isr) (void))
{
	idt_write_gate(vec, isr, 0);
}

static void
idt_set_gate_ist(int vec, void (*isr) (void))
{
	idt_write_gate(vec, isr, 1);
}

uint64_t
idt_base_addr(void)
{
	return (uint64_t) (uintptr_t) idt;
}

uint64_t
idt_ptr_size(void)
{
	return 10;
}

uint64_t
idt_vec_addr(int vec)
{
	if (vec < 0 || vec >= 256)
		return 0;
	struct idt_entry *e = &idt[vec];
	uint64_t addr = 0;
	addr |= (uint64_t) e->off_low;
	addr |= (uint64_t) e->off_mid << 16;
	addr |= (uint64_t) e->off_high << 32;
	return addr;
}

void
idt_init(void)
{
	for (int i = 0; i < 256; i++) {
		idt[i].off_low = 0;
		idt[i].sel = 0;
		idt[i].ist = 0;
		idt[i].type_attr = 0;
		idt[i].off_mid = 0;
		idt[i].off_high = 0;
		idt[i].zero = 0;
	}

	idt_set_gate(VEC_DE, isr_de);
	idt_set_gate(VEC_DB, isr_db);
	idt_set_gate(VEC_UD, isr_ud);
	idt_set_gate(VEC_BP, isr_bp);
	idt_set_gate(VEC_NP, isr_np);
	idt_set_gate(VEC_SS, isr_ss);
	idt_set_gate(VEC_GP, isr_gp);
	idt_set_gate_ist(VEC_DF, isr_df);	/* IST1: dedicated stack for #DF. */
	idt_set_gate(VEC_PF, isr_pf);
	idt_set_gate(VEC_TIMER, isr_timer);
	idt_set_gate(VEC_SCHED_KICK, isr_sched_kick);
	extern void isr_tlb_shootdown(void);
	idt_set_gate(VEC_TLB_SHOOTDOWN, isr_tlb_shootdown);

	uint8_t idtr[10];
	uint16_t limit = (uint16_t) (sizeof(idt) - 1);
	uint64_t base = (uint64_t) (uintptr_t) idt;
	idtr[0] = (uint8_t) (limit & 0xFF);
	idtr[1] = (uint8_t) ((limit >> 8) & 0xFF);
	for (int i = 0; i < 8; i++) {
		idtr[2 + i] = (uint8_t) ((base >> (i * 8)) & 0xFF);
	}
	__asm__ __volatile__("lidt (%0)"::"r"(idtr));
}
