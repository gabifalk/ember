/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_ACPI_H
#define EMBER_ACPI_H

#include <stdint.h>

#define ACPI_MAX_CPUS 256

/* RSDP (ACPI 2.0+) */
typedef struct {
	char signature[8];	/* "RSD PTR ". */
	uint8_t checksum;
	char oem_id[6];
	uint8_t revision;
	uint32_t rsdt_address;
	uint32_t length;
	uint64_t xsdt_address;
	uint8_t ext_checksum;
	uint8_t reserved[3];
} acpi_rsdp_t;

/* Common SDT header. */
typedef struct {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem_id[6];
	char oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} acpi_sdt_header_t;

/* XSDT: 64-bit entry pointers. */
typedef struct {
	acpi_sdt_header_t header;
	/* uint64_t entries[] follow. */
} acpi_xsdt_t;

/* RSDT: 32-bit entry pointers. */
typedef struct {
	acpi_sdt_header_t header;
	/* uint32_t entries[] follow. */
} acpi_rsdt_t;

/* MADT (Multiple APIC Description Table) */
typedef struct {
	acpi_sdt_header_t header;
	uint32_t local_apic_addr;
	uint32_t flags;
	/* Variable-length entries follow. */
} acpi_madt_t;

/* MADT entry header. */
typedef struct {
	uint8_t type;
	uint8_t length;
} acpi_madt_entry_t;

/* Type 0: Processor Local APIC. */
typedef struct {
	uint8_t type;		/* 0. */
	uint8_t length;		/* 8. */
	uint8_t acpi_proc_id;
	uint8_t apic_id;
	uint32_t flags;		/* Bit 0: enabled, bit 1: online capable. */
} acpi_madt_lapic_t;

/* Type 1: I/O APIC. */
typedef struct {
	uint8_t type;		/* 1. */
	uint8_t length;		/* 12. */
	uint8_t ioapic_id;
	uint8_t reserved;
	uint32_t ioapic_addr;
	uint32_t gsi_base;
} acpi_madt_ioapic_t;

/* Type 2: Interrupt Source Override. */
typedef struct {
	uint8_t type;		/* 2. */
	uint8_t length;		/* 10. */
	uint8_t bus;		/* 0 = ISA. */
	uint8_t source;		/* ISA IRQ number. */
	uint32_t gsi;		/* Global System Interrupt. */
	uint16_t flags;		/* Polarity [1:0], trigger [3:2]. */
} __attribute__ ((packed)) acpi_madt_iso_t;

/* Type 9: Processor Local x2APIC. */
typedef struct {
	uint8_t type;		/* 9. */
	uint8_t length;		/* 16. */
	uint16_t reserved;
	uint32_t x2apic_id;
	uint32_t flags;		/* Bit 0: enabled, bit 1: online capable. */
	uint32_t acpi_proc_uid;
} acpi_madt_x2apic_t;

#define ACPI_MAX_ISOS 32

extern int acpi_cpu_count;
extern uint8_t acpi_lapic_ids[ACPI_MAX_CPUS];
extern uint64_t acpi_lapic_base;
extern uint64_t acpi_ioapic_base;
extern acpi_madt_iso_t acpi_isos[ACPI_MAX_ISOS];
extern int acpi_iso_count;

void acpi_init(uint64_t rsdp_phys);

#endif				/* EMBER_ACPI_H. */
