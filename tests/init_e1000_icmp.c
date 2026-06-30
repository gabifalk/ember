/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
/* Minimal init: keep PID 1 alive so the kernel keeps servicing the e1000 RX
 * interrupt. The in-kernel net stack answers ARP and ICMP; the harness drives
 * QEMU's lifecycle and the ping round-trip. This init never exits. */

void
_start(void)
{
	for (;;)
		__asm__ __volatile__("pause");
}
