/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include "ember/blkcache.h"
#include "ember/blkdev.h"
#include "ember/spinlock.h"
#include "ember/heap.h"

#define BLKCACHE_ENTRIES 4096
#define HASH_BUCKETS     4096
#define READAHEAD_BLOCKS 0

/* Per-device block sizes. */
static uint32_t dev_block_size[BLKDEV_MAX];
static uint32_t dev_spb[BLKDEV_MAX];	/* Sectors per block. */

/*
 * Metadata is separate from data so find_lru scans a compact array
 * (~24 bytes/entry) instead of striding through 4KB data blocks.
 */
typedef struct {
	uint32_t block_num;
	int dev;
	uint8_t valid;
	uint8_t dirty;
	uint64_t use_count;
	int hash_next;
} blkcache_meta_t;

static blkcache_meta_t cache[BLKCACHE_ENTRIES];
static uint8_t cache_data[BLKCACHE_ENTRIES][BLKCACHE_MAX_BLOCK_SIZE];
static int hash_buckets[HASH_BUCKETS];
static uint64_t access_counter;
static spinlock_t blkcache_lock = SPINLOCK_INIT;

/* Per-device sequential readahead tracking. */
static uint32_t last_miss_block[BLKDEV_MAX];
static int last_miss_dev_valid[BLKDEV_MAX];	/* 0 = No previous miss. */

static inline uint32_t
hash_key(int dev, uint32_t block_num)
{
	return ((uint32_t) dev * 2654435761u + block_num) % HASH_BUCKETS;
}

static void
hash_insert(int idx)
{
	uint32_t h = hash_key(cache[idx].dev, cache[idx].block_num);
	cache[idx].hash_next = hash_buckets[h];
	hash_buckets[h] = idx;
}

static void
hash_remove(int idx)
{
	uint32_t h = hash_key(cache[idx].dev, cache[idx].block_num);
	int *pp = &hash_buckets[h];
	while (*pp != -1) {
		if (*pp == idx) {
			*pp = cache[idx].hash_next;
			cache[idx].hash_next = -1;
			return;
		}
		pp = &cache[*pp].hash_next;
	}
}

static int
hash_lookup(int dev, uint32_t block_num)
{
	uint32_t h = hash_key(dev, block_num);
	int idx = hash_buckets[h];
	while (idx != -1) {
		if (cache[idx].valid && cache[idx].dev == dev
		    && cache[idx].block_num == block_num)
			return idx;
		idx = cache[idx].hash_next;
	}
	return -1;
}

void
blkcache_init(void)
{
	access_counter = 0;
	for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
		cache[i].valid = 0;
		cache[i].dirty = 0;
		cache[i].use_count = 0;
		cache[i].hash_next = -1;
	}
	for (int i = 0; i < HASH_BUCKETS; i++)
		hash_buckets[i] = -1;
	for (int i = 0; i < BLKDEV_MAX; i++) {
		dev_block_size[i] = 512;
		dev_spb[i] = 1;
	}
}

void
blkcache_set_block_size(int dev, uint32_t bs)
{
	if (dev < 0 || dev >= BLKDEV_MAX)
		return;
	spin_lock(&blkcache_lock);
	/* Flush and invalidate entries for this device using OLD spb. */
	for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
		if (cache[i].valid && cache[i].dev == dev) {
			if (cache[i].dirty) {
				uint32_t lba =
				    cache[i].block_num * dev_spb[dev];
				blkdev_write(dev, lba, (uint8_t) dev_spb[dev],
					     cache_data[i]);
			}
			hash_remove(i);
			cache[i].valid = 0;
			cache[i].dirty = 0;
		}
	}
	/* Now set new block size. */
	dev_block_size[dev] = bs;
	dev_spb[dev] = bs / 512;
	spin_unlock(&blkcache_lock);
}

uint32_t
blkcache_get_block_size(int dev)
{
	if (dev < 0 || dev >= BLKDEV_MAX)
		return 512;
	return dev_block_size[dev];
}

/* Write back a dirty entry (caller holds lock) */
static void
writeback_entry(int idx)
{
	if (cache[idx].dirty && cache[idx].valid) {
		uint32_t spb = dev_spb[cache[idx].dev];
		uint32_t lba = cache[idx].block_num * spb;
		int r =
		    blkdev_write(cache[idx].dev, lba, (uint8_t) spb,
				 cache_data[idx]);
		if (r == 0)
			cache[idx].dirty = 0;
		/* If write failed, keep dirty -- will retry on next eviction. */
	}
}

/* Find LRU slot, writing back dirty entry if evicting. */
static int
find_lru(void)
{
	int lru = 0;
	uint64_t min_use = cache[0].use_count;
	for (int i = 1; i < BLKCACHE_ENTRIES; i++) {
		if (!cache[i].valid)
			return i;
		if (cache[i].use_count < min_use) {
			min_use = cache[i].use_count;
			lru = i;
		}
	}
	/* Write back dirty data before evicting. */
	writeback_entry(lru);
	hash_remove(lru);
	return lru;
}

int
blkcache_read(int dev, uint32_t block_num, void *out_buf)
{
	if (dev < 0 || dev >= BLKDEV_MAX)
		return -1;
	uint32_t spb = dev_spb[dev];
	uint32_t bs = dev_block_size[dev];

	spin_lock(&blkcache_lock);
	access_counter++;

	int i = hash_lookup(dev, block_num);
	if (i >= 0) {
		cache[i].use_count = access_counter;
		/* Copy while lock is held. */
		kmemcpy(out_buf, cache_data[i], bs);
		spin_unlock(&blkcache_lock);
		return 0;
	}

	/* Cache miss -- find LRU slot, read from disk. */
	int lru = find_lru();
	uint32_t lba = block_num * spb;
	if (blkdev_read(dev, lba, (uint8_t) spb, cache_data[lru]) < 0) {
		spin_unlock(&blkcache_lock);
		return -1;
	}

	cache[lru].block_num = block_num;
	cache[lru].dev = dev;
	cache[lru].valid = 1;
	cache[lru].dirty = 0;
	cache[lru].use_count = access_counter;
	hash_insert(lru);

	/* Copy to caller buffer while lock held. */
	kmemcpy(out_buf, cache_data[lru], bs);

	/* Determine if this miss is sequential -- trigger readahead if so. */
	int do_readahead = last_miss_dev_valid[dev] &&
	    (block_num == last_miss_block[dev] + 1);
	last_miss_block[dev] = block_num;
	last_miss_dev_valid[dev] = 1;

	spin_unlock(&blkcache_lock);

	/* Prefetch next blocks outside the lock. */
	if (do_readahead) {
		for (uint32_t ra = 1; ra <= READAHEAD_BLOCKS; ra++) {
			uint32_t ra_block = block_num + ra;
			/* Overflow guard. */
			if (ra_block <= block_num)
				break;

			spin_lock(&blkcache_lock);
			/* Already cached? Skip. */
			if (hash_lookup(dev, ra_block) >= 0) {
				spin_unlock(&blkcache_lock);
				continue;
			}
			/*
			 * Find slot and read from disk (lock held during I/O for
			 * consistency with existing single-block read path)
			 */
			int ra_lru = find_lru();
			uint32_t ra_lba = ra_block * spb;
			if (blkdev_read(dev, ra_lba, (uint8_t) spb,
					cache_data[ra_lru]) < 0) {
				spin_unlock(&blkcache_lock);
				break;	/* Stop readahead on I/O error. */
			}
			access_counter++;
			cache[ra_lru].block_num = ra_block;
			cache[ra_lru].dev = dev;
			cache[ra_lru].valid = 1;
			cache[ra_lru].dirty = 0;
			cache[ra_lru].use_count = access_counter;
			hash_insert(ra_lru);
			spin_unlock(&blkcache_lock);
		}
	}

	return 0;
}

void
blkcache_invalidate(void)
{
	spin_lock(&blkcache_lock);
	/* Flush all dirty entries before invalidating. */
	for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
		writeback_entry(i);
		cache[i].valid = 0;
		cache[i].dirty = 0;
	}
	for (int i = 0; i < HASH_BUCKETS; i++)
		hash_buckets[i] = -1;
	for (int i = 0; i < BLKCACHE_ENTRIES; i++)
		cache[i].hash_next = -1;
	spin_unlock(&blkcache_lock);
}

/*
 * Discard a single block's cache entry (no writeback).
 * Used when a block is freed -- stale dirty data must not be written back.
 */
void
blkcache_discard(int dev, uint32_t block_num)
{
	spin_lock(&blkcache_lock);
	int idx = hash_lookup(dev, block_num);
	if (idx >= 0) {
		hash_remove(idx);
		cache[idx].valid = 0;
		cache[idx].dirty = 0;
	}
	spin_unlock(&blkcache_lock);
}

/* Write all dirty entries for a device to disk. */
void
blkcache_sync(int dev)
{
	spin_lock(&blkcache_lock);
	for (int i = 0; i < BLKCACHE_ENTRIES; i++) {
		if (cache[i].valid && cache[i].dirty && cache[i].dev == dev) {
			writeback_entry(i);
		}
	}
	spin_unlock(&blkcache_lock);
}

void
blkcache_prefetch(int dev, const uint32_t * blocks, uint32_t count)
{
	if (dev < 0 || dev >= BLKDEV_MAX || count == 0)
		return;
	uint32_t spb = dev_spb[dev];
	uint32_t bs = dev_block_size[dev];

#define PREFETCH_MAX 16
	uint8_t *tmp = (uint8_t *) kmalloc(PREFETCH_MAX * bs);
	if (!tmp)
		return;

	uint32_t i = 0;
	while (i < count) {
		uint32_t blk = blocks[i];
		if (blk == 0) {
			i++;
			continue;
		}

		/* Sparse -- skip. */
		/* Check cache for this block. */
		spin_lock(&blkcache_lock);
		int hit = hash_lookup(dev, blk);
		if (hit >= 0) {
			cache[hit].use_count = ++access_counter;
			spin_unlock(&blkcache_lock);
			i++;
			continue;
		}
		spin_unlock(&blkcache_lock);

		/* Cache miss -- find the longest contiguous-on-disk run of misses. */
		uint32_t run = 1;
		while (run < PREFETCH_MAX && i + run < count) {
			uint32_t next = blocks[i + run];
			if (next == 0 || next != blk + run)
				break;

			spin_lock(&blkcache_lock);
			int h = hash_lookup(dev, next);
			spin_unlock(&blkcache_lock);
			if (h >= 0)
				break;	/* Already cached -- stop run. */

			run++;
		}

		/* Read the whole run from disk in one I/O. */
		uint32_t lba = blk * spb;
		uint32_t total_sectors = run * spb;
		/* blkdev_read count is uint8_t, so cap at 255 sectors. */
		if (total_sectors > 255) {
			run = 255 / spb;
			total_sectors = run * spb;
		}

		if (blkdev_read(dev, lba, (uint8_t) total_sectors, tmp) < 0) {
			/* On I/O error, skip this run and continue. */
			i += run;
			continue;
		}

		/* Insert each block of the run into the cache. */
		spin_lock(&blkcache_lock);
		for (uint32_t r = 0; r < run; r++) {
			uint32_t b = blk + r;
			/* Re-check: another CPU may have inserted it while we did I/O. */
			if (hash_lookup(dev, b) >= 0)
				continue;

			int slot = find_lru();
			access_counter++;
			cache[slot].block_num = b;
			cache[slot].dev = dev;
			cache[slot].valid = 1;
			cache[slot].dirty = 0;
			cache[slot].use_count = access_counter;
			/* Copy from tmp buffer. */
			kmemcpy(cache_data[slot], tmp + r * bs, bs);
			hash_insert(slot);
		}
		spin_unlock(&blkcache_lock);

		i += run;
	}
	kfree(tmp);
#undef PREFETCH_MAX
}

int
blkcache_write(int dev, uint32_t block_num, const void *data)
{
	if (dev < 0 || dev >= BLKDEV_MAX)
		return -1;
	uint32_t bs = dev_block_size[dev];
	const uint8_t *src = (const uint8_t *)data;

	spin_lock(&blkcache_lock);
	access_counter++;

	int i = hash_lookup(dev, block_num);
	if (i >= 0) {
		kmemcpy(cache_data[i], src, bs);
		cache[i].use_count = access_counter;
		cache[i].dirty = 1;	/* Write-back: defer disk write. */
		spin_unlock(&blkcache_lock);
		return 0;
	}

	int lru = find_lru();
	kmemcpy(cache_data[lru], src, bs);
	cache[lru].block_num = block_num;
	cache[lru].dev = dev;
	cache[lru].valid = 1;
	cache[lru].dirty = 1;	/* Write-back: defer disk write. */
	cache[lru].use_count = access_counter;
	hash_insert(lru);
	spin_unlock(&blkcache_lock);
	return 0;
}
