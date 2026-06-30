/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_USER_H
#define EMBER_USER_H

#include <stdint.h>
#include "ember/elf.h"

void enter_user(uint64_t rip, uint64_t rsp);
void user_run_init(void);

/* Set up user stack with argc/argv/auxv. Returns new rsp, or 0 on failure. */
uint64_t setup_user_stack(uint64_t pml4, elf_info_t * info);

/* Set up user stack with custom argv/envp strings. Returns new rsp, or 0 on failure. */
uint64_t setup_user_stack_argv(uint64_t pml4, elf_info_t * info,
			       const char **argv, int argc,
			       const char **envp, int envc);

#endif
