/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_UEFI_CALL_H
#define EMBER_UEFI_CALL_H

#include <stdint.h>

uint64_t uefi_call2(void *fn, uint64_t a1, uint64_t a2);
uint64_t uefi_call3(void *fn, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t uefi_call4(void *fn, uint64_t a1, uint64_t a2, uint64_t a3,
		    uint64_t a4);
uint64_t uefi_call5(void *fn, uint64_t a1, uint64_t a2, uint64_t a3,
		    uint64_t a4, uint64_t a5);

#endif
