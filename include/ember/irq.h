/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_IRQ_H
#define EMBER_IRQ_H

#include <stdint.h>

typedef void (*irq_handler_t) (int irq);

static inline void
irq_register(int irq, irq_handler_t handler)
{
	(void)irq;
	(void)handler;
}

static inline void
irq_dispatch(int vector)
{
	(void)vector;
}

#endif				/* EMBER_IRQ_H. */
