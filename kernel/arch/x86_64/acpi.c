/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */

#include <stdint.h>
#include "ember/acpi.h"
#include "ember/cpu.h"
#include "ember/mmu.h"
#include "ember/console.h"

int acpi_cpu_count;
uint8_t acpi_lapic_ids[ACPI_MAX_CPUS];
uint64_t acpi_lapic_base;
uint64_t acpi_ioapic_base;
acpi_madt_iso_t acpi_isos[ACPI_MAX_ISOS];
int acpi_iso_count;

static void
serial_dec(int val)
{
	if (val < 0) {
		console_write("-");
		val = -val;
	}
	if (val >= 10)
		serial_dec(val / 10);
	char c = '0' + (val % 10);
	console_write(&c);	/* Relies on console_write reading until NUL; use putc. */
}

static void
print_int(int val)
{
	char buf[12];
	int i = 0;
	if (val == 0) {
		buf[i++] = '0';
	} else {
		/* Reverse digits. */
		int v = val;
		while (v > 0) {
			buf[i++] = '0' + (v % 10);
			v /= 10;
		}
		/* Reverse. */
		for (int a = 0, b = i - 1; a < b; a++, b--) {
			char t = buf[a];
			buf[a] = buf[b];
			buf[b] = t;
		}
	}
	buf[i] = '\0';
	console_write(buf);
}

static int
sig_match(const char *a, const char *b, int n)
{
	for (int i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

static acpi_sdt_header_t *
acpi_find_table(uint64_t rsdp_phys, const char *sig)
{
	acpi_rsdp_t *rsdp = (acpi_rsdp_t *) phys_to_virt(rsdp_phys);

	/* Validate RSDP signature. */
	if (!sig_match(rsdp->signature, "RSD PTR ", 8)) {
		console_write("ACPI: bad RSDP signature\n");
		return 0;
	}

	/* Prefer XSDT (revision >= 2) over RSDT. */
	if (rsdp->revision >= 2 && rsdp->xsdt_address) {
		acpi_xsdt_t *xsdt =
		    (acpi_xsdt_t *) phys_to_virt(rsdp->xsdt_address);
		if (!sig_match(xsdt->header.signature, "XSDT", 4)) {
			console_write("ACPI: bad XSDT signature\n");
			return 0;
		}
		int entry_count =
		    (xsdt->header.length - sizeof(acpi_sdt_header_t)) / 8;
		uint64_t *entries =
		    (uint64_t *) ((uint8_t *) xsdt + sizeof(acpi_sdt_header_t));
		for (int i = 0; i < entry_count; i++) {
			acpi_sdt_header_t *hdr =
			    (acpi_sdt_header_t *) phys_to_virt(entries[i]);
			if (sig_match(hdr->signature, sig, 4)) {
				return hdr;
			}
		}
	} else if (rsdp->rsdt_address) {
		acpi_rsdt_t *rsdt =
		    (acpi_rsdt_t *) phys_to_virt((uint64_t) rsdp->rsdt_address);
		if (!sig_match(rsdt->header.signature, "RSDT", 4)) {
			console_write("ACPI: bad RSDT signature\n");
			return 0;
		}
		int entry_count =
		    (rsdt->header.length - sizeof(acpi_sdt_header_t)) / 4;
		uint32_t *entries =
		    (uint32_t *) ((uint8_t *) rsdt + sizeof(acpi_sdt_header_t));
		for (int i = 0; i < entry_count; i++) {
			acpi_sdt_header_t *hdr =
			    (acpi_sdt_header_t *) phys_to_virt((uint64_t)
							       entries[i]);
			if (sig_match(hdr->signature, sig, 4)) {
				return hdr;
			}
		}
	}

	return 0;
}

void
acpi_init(uint64_t rsdp_phys)
{
	acpi_cpu_count = 0;
	acpi_lapic_base = 0;
	acpi_ioapic_base = 0;
	acpi_iso_count = 0;

	if (!rsdp_phys) {
		console_write("ACPI: no RSDP, skipping\n");
		return;
	}

	console_write("ACPI: parsing tables\n");

	acpi_sdt_header_t *madt_hdr = acpi_find_table(rsdp_phys, "APIC");
	if (!madt_hdr) {
		console_write("ACPI: MADT not found\n");
		return;
	}

	acpi_madt_t *madt = (acpi_madt_t *) madt_hdr;
	acpi_lapic_base = (uint64_t) madt->local_apic_addr;

	/* Walk MADT entries. */
	uint8_t *ptr = (uint8_t *) madt + sizeof(acpi_madt_t);
	uint8_t *end = (uint8_t *) madt + madt->header.length;

	while (ptr + 2 <= end) {
		acpi_madt_entry_t *entry = (acpi_madt_entry_t *) ptr;
		if (entry->length < 2)
			break;
		if (ptr + entry->length > end)
			break;

		if (entry->type == 0) {
			/* Processor Local APIC. */
			acpi_madt_lapic_t *lapic = (acpi_madt_lapic_t *) entry;
			/* Check enabled or online-capable (flags bit 0 or bit 1) */
			if (lapic->flags & 0x3) {
				if (acpi_cpu_count < ACPI_MAX_CPUS) {
					acpi_lapic_ids[acpi_cpu_count] =
					    lapic->apic_id;
					cpu_lapic_ids[acpi_cpu_count] =
					    lapic->apic_id;
					lapic_to_cpu[lapic->apic_id] =
					    acpi_cpu_count;
					acpi_cpu_count++;
				}
			}
		} else if (entry->type == 1) {
			/* I/O APIC. */
			acpi_madt_ioapic_t *ioapic =
			    (acpi_madt_ioapic_t *) entry;
			if (!acpi_ioapic_base) {
				acpi_ioapic_base =
				    (uint64_t) ioapic->ioapic_addr;
			}
		} else if (entry->type == 2) {
			/* Interrupt Source Override. */
			acpi_madt_iso_t *iso = (acpi_madt_iso_t *) entry;
			if (acpi_iso_count < ACPI_MAX_ISOS) {
				/* Manual copy to avoid memmove (freestanding) */
				{
					uint8_t *d =
					    (uint8_t *) &
					    acpi_isos[acpi_iso_count];
					const uint8_t *s = (const uint8_t *)iso;
					for (int b = 0;
					     b < (int)sizeof(acpi_madt_iso_t);
					     b++)
						d[b] = s[b];
				}
				acpi_iso_count++;
				console_write("ACPI: ISO: IRQ ");
				print_int(iso->source);
				console_write(" -> GSI ");
				print_int(iso->gsi);
				console_write(", flags 0x");
				/* Print flags as 4-digit hex. */
				{
					static const char hex[] =
					    "0123456789abcdef";
					uint16_t f = iso->flags;
					console_putc(hex[(f >> 12) & 0xF]);
					console_putc(hex[(f >> 8) & 0xF]);
					console_putc(hex[(f >> 4) & 0xF]);
					console_putc(hex[f & 0xF]);
				}
				console_write("\n");
			}
		} else if (entry->type == 9) {
			/*
			 * Processor Local x2APIC (32-bit APIC ID).
			 * QEMU uses these instead of type-0 for large CPU counts
			 * or when x2APIC mode is selected.  Without this, APs get
			 * lapic_to_cpu[id] = -1 -> this_cpu_id() = -1 -> percpu_tss[-1]
			 * -> stale TSS.RSP0 -> kstack frame corruption (RIP=0x246 bug).
			 */
			acpi_madt_x2apic_t *x2 = (acpi_madt_x2apic_t *) entry;
			if (x2->flags & 0x3) {
				if (acpi_cpu_count < ACPI_MAX_CPUS) {
					uint32_t id = x2->x2apic_id;
					acpi_lapic_ids[acpi_cpu_count] = (uint8_t) id;	/* Truncated for legacy array. */
					cpu_lapic_ids[acpi_cpu_count] = id;
					if (id < 256)
						lapic_to_cpu[id] =
						    acpi_cpu_count;
					acpi_cpu_count++;
				}
			}
		} else if (entry->type == 5) {
			/* LAPIC Address Override (64-bit) */
			if (entry->length >= 12) {
				uint64_t addr;
				uint8_t *p = (uint8_t *) entry + 4;
				addr = (uint64_t) p[0];
				addr |= (uint64_t) p[1] << 8;
				addr |= (uint64_t) p[2] << 16;
				addr |= (uint64_t) p[3] << 24;
				addr |= (uint64_t) p[4] << 32;
				addr |= (uint64_t) p[5] << 40;
				addr |= (uint64_t) p[6] << 48;
				addr |= (uint64_t) p[7] << 56;
				acpi_lapic_base = addr;
			}
		}

		ptr += entry->length;
	}

	console_write("ACPI: ");
	print_int(acpi_cpu_count);
	console_write(" CPUs detected\n");

	cpu_count = acpi_cpu_count;
	if (cpu_count > MAX_CPUS) {
		console_write("ACPI: clamping to ");
		print_int(MAX_CPUS);
		console_write(" CPUs (MAX_CPUS)\n");
		cpu_count = MAX_CPUS;
	}
}
