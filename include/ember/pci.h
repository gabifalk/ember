/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_PCI_H
#define EMBER_PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

typedef struct {
	uint8_t bus;
	uint8_t dev;
	uint8_t fn;
	int found;
} pci_dev_t;

uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_cfg_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint16_t val);

pci_dev_t pci_find_by_class(uint8_t class, uint8_t subclass, uint8_t progif);
pci_dev_t pci_find_by_id(uint16_t vendor, uint16_t device);

uint64_t pci_bar(pci_dev_t d, int bar, int *is_mmio);
void pci_enable_bus_master(pci_dev_t d);
uint8_t pci_interrupt_line(pci_dev_t d);
uint8_t pci_interrupt_pin(pci_dev_t d);

#endif				/* EMBER_PCI_H. */
