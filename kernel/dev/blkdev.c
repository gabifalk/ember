/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>

#include "ember/blkdev.h"
#include "ember/ahci.h"
#include "ember/ata.h"
#include "ember/spinlock.h"

static spinlock_t blkdev_lock = SPINLOCK_INIT;

/* Driver templates (to be probed) */
static const blkdev_ops_t *templates[4];
static int template_count;

/* Active devices (probed + directly registered) */
static const blkdev_ops_t *devices[BLKDEV_MAX];
static int device_count;

static int
ata_probe_adapter(void)
{
	return ata_init();
}

static int
ata_read_adapter(uint32_t lba, uint8_t count, void *buf)
{
	return ata_read_sectors(lba, count, buf);
}

static int
ata_write_adapter(uint32_t lba, uint8_t count, const void *buf)
{
	return ata_write_sectors(lba, count, buf);
}

static const blkdev_ops_t ata_legacy_ops = {
	.name = "ata-legacy",
	.probe = ata_probe_adapter,
	.read_blocks = ata_read_adapter,
	.write_blocks = ata_write_adapter,
	.flush = ata_flush,
};

static const blkdev_ops_t ahci_ops = {
	.name = "ahci",
	.probe = ahci_probe,
	.read_blocks = ahci_read_blocks,
	.write_blocks = ahci_write_blocks,
	.flush = ahci_flush,
};

static void
register_template(const blkdev_ops_t * ops)
{
	if (!ops || template_count >= 4)
		return;
	templates[template_count++] = ops;
}

void
blkdev_init(void)
{
	template_count = 0;
	device_count = 0;

	/* Register built-ins in preference order. */
	register_template(&ahci_ops);
	register_template(&ata_legacy_ops);
}

int
blkdev_probe_all(void)
{
	for (int i = 0; i < template_count; i++) {
		const blkdev_ops_t *ops = templates[i];
		/* Skip legacy ATA if AHCI already found a disk. */
		if (device_count > 0 && ops->probe == ata_probe_adapter)
			continue;
		if (ops->probe() == 0) {
			if (device_count < BLKDEV_MAX)
				devices[device_count++] = ops;
		}
	}
	return device_count;
}

int
blkdev_register_device(const blkdev_ops_t * ops)
{
	if (!ops || device_count >= BLKDEV_MAX)
		return -1;
	int idx = device_count++;
	devices[idx] = ops;
	return idx;
}

int
blkdev_read(int dev, uint32_t lba, uint8_t count, void *buf)
{
	if (dev < 0 || dev >= device_count)
		return -1;
	uint64_t flags = spin_lock_irqsave(&blkdev_lock);
	int r = devices[dev]->read_blocks(lba, count, buf);
	spin_unlock_irqrestore(&blkdev_lock, flags);
	return r;
}

int
blkdev_write(int dev, uint32_t lba, uint8_t count, const void *buf)
{
	if (dev < 0 || dev >= device_count)
		return -1;
	uint64_t flags = spin_lock_irqsave(&blkdev_lock);
	int r = devices[dev]->write_blocks(lba, count, buf);
	spin_unlock_irqrestore(&blkdev_lock, flags);
	return r;
}

int
blkdev_flush(int dev)
{
	if (dev < 0 || dev >= device_count)
		return -1;
	if (!devices[dev]->flush)
		return 0;
	uint64_t flags = spin_lock_irqsave(&blkdev_lock);
	int r = devices[dev]->flush();
	spin_unlock_irqrestore(&blkdev_lock, flags);
	return r;
}

int
blkdev_count(void)
{
	return device_count;
}

const char *
blkdev_name(int dev)
{
	if (dev < 0 || dev >= device_count)
		return "none";
	return devices[dev]->name ? devices[dev]->name : "unknown";
}
