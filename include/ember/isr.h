/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_ISR_H
#define EMBER_ISR_H

#include <stdint.h>

typedef struct isr_frame {
	/* Pushed by isr_common (low address first) */
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rdi, rsi, rbp, rdx, rcx, rbx, rax;
	/* Pushed by stub. */
	uint64_t vector;
	uint64_t error_code;
	/* Pushed by CPU. */
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
} isr_frame_t;

int isr_handler(isr_frame_t * frame);

#endif
