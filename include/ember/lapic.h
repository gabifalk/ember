/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_LAPIC_H
#define EMBER_LAPIC_H

#include <stdint.h>

void lapic_init(void);
void lapic_eoi(void);
uint32_t lapic_id(void);
void lapic_send_ipi(uint8_t dest, uint8_t vector);
void lapic_send_init(uint8_t dest);
void lapic_send_sipi(uint8_t dest, uint8_t vector);
void lapic_send_init_all(void);
void lapic_send_sipi_all(uint8_t vector);
void lapic_timer_init(uint32_t hz);
void lapic_send_ipi_all_excl_self(uint8_t vector);
uint32_t lapic_get_timer_count(void);
void lapic_timer_init_count(uint32_t count);

extern volatile int lapic_enabled;

#endif				/* EMBER_LAPIC_H. */
