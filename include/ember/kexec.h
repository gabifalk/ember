/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_KEXEC_H
#define EMBER_KEXEC_H

#include <stdint.h>

/* Two-phase API for kexec_file_load syscall. */
int kexec_load_from_fds(int kernel_fd, int initrd_fd,
			const char *cmdline, uint64_t cmdline_len);
int kexec_execute(void);	/* Does not return on success. */
int kexec_is_loaded(void);

/* AP halt flag for kexec shutdown. Set by BSP, checked by APs. */
extern volatile int kexec_halting;
extern volatile int kexec_ap_halted;

/* Generate /proc/iomem text. Caller must kfree() returned buffer. */
char *kexec_generate_iomem(uint64_t * out_len);

#endif
