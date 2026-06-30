/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include <stdint.h>
#include <stddef.h>

#include "ember/heap.h"
#include "ember/pmm.h"
#include "ember/mmu.h"
#include "ember/spinlock.h"
#include "ember/console.h"

#define HEAP_ALIGN 16

typedef struct heap_block {
	uint64_t size;		/* Payload size. */
	uint8_t free;
	struct heap_block *next;
	struct heap_block *prev;
} heap_block_t;

/* ---- Bucket cache layer ---- */
#define BUCKET_COUNT   7
#define BUCKET_MAX     4096

static const uint64_t bucket_sizes[BUCKET_COUNT] = {
	64, 128, 256, 512, 1024, 2048, 4096
};

/* Free list per bucket.  Free blocks store a next-pointer in their payload. */
typedef struct bucket_free {
	struct bucket_free *next;
} bucket_free_t;

static bucket_free_t *bucket_heads[BUCKET_COUNT];
static int bucket_counts[BUCKET_COUNT];

/* Max cached blocks per bucket.  Excess returns to heap for coalescing.
 * Verified: models/bucket_drain.pml (P1-P2). */
#define BUCKET_CAP     32

/* Return bucket index for a given size, or -1 if too large. */
static int
bucket_index(uint64_t size)
{
	for (int i = 0; i < BUCKET_COUNT; i++) {
		if (size <= bucket_sizes[i])
			return i;
	}
	return -1;
}

/* Check whether a payload size exactly matches a bucket size. */
static int
bucket_index_exact(uint64_t size)
{
	for (int i = 0; i < BUCKET_COUNT; i++) {
		if (size == bucket_sizes[i])
			return i;
	}
	return -1;
}

static spinlock_t heap_lock = SPINLOCK_INIT;
static heap_block_t *heap_head;
static heap_block_t *heap_tail;

static inline uint64_t
align_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static void
heap_insert_block(void *addr, uint64_t total_size)
{
	heap_block_t *b = (heap_block_t *) addr;
	b->size = total_size - sizeof(heap_block_t);
	b->free = 1;
	b->next = NULL;
	b->prev = heap_tail;

	if (!heap_head) {
		heap_head = b;
		heap_tail = b;
		return;
	}

	heap_tail->next = b;
	heap_tail = b;
}

static int
heap_grow(uint64_t min_size)
{
	uint64_t total = align_up(min_size + sizeof(heap_block_t), PAGE_SIZE);
	uint64_t pages = total / PAGE_SIZE;

	uint64_t paddr = pmm_alloc_pages(pages);
	if (paddr == UINT64_MAX)
		return 0;

	void *vaddr = phys_to_virt(paddr);
	heap_insert_block(vaddr, total);
	return 1;
}

void
heap_init(void)
{
	heap_head = NULL;
	heap_tail = NULL;
	/* Initial 16 pages. */
	heap_grow(16 * PAGE_SIZE);
}

static void
split_block(heap_block_t * b, uint64_t size)
{
	uint64_t aligned = align_up(size, HEAP_ALIGN);
	uint64_t remaining = b->size - aligned;
	if (remaining <= sizeof(heap_block_t) + HEAP_ALIGN) {
		return;
	}

	uint8_t *base = (uint8_t *) b;
	heap_block_t *new_blk =
	    (heap_block_t *) (base + sizeof(heap_block_t) + aligned);
	new_blk->size = remaining - sizeof(heap_block_t);
	new_blk->free = 1;
	new_blk->next = b->next;
	new_blk->prev = b;

	if (b->next)
		b->next->prev = new_blk;
	else
		heap_tail = new_blk;

	b->size = aligned;
	b->next = new_blk;
}

/* Internal: allocate from the underlying heap (caller holds heap_lock). */
static void *
heap_alloc_locked(uint64_t aligned)
{
	heap_block_t *cur = heap_head;
	while (cur) {
		if (cur->free && cur->size >= aligned) {
			split_block(cur, aligned);
			cur->free = 0;
			return (uint8_t *) cur + sizeof(heap_block_t);
		}
		cur = cur->next;
	}

	if (!heap_grow(aligned))
		return NULL;

	/* heap_grow succeeded; retry. */
	cur = heap_head;
	while (cur) {
		if (cur->free && cur->size >= aligned) {
			split_block(cur, aligned);
			cur->free = 0;
			return (uint8_t *) cur + sizeof(heap_block_t);
		}
		cur = cur->next;
	}
	return NULL;
}

void *
kmalloc(uint64_t size)
{
	if (size == 0)
		return NULL;

	int bi = bucket_index(size);
	if (bi >= 0) {
		uint64_t bsize = bucket_sizes[bi];
		uint64_t flags = spin_lock_irqsave(&heap_lock);

		/* Fast path: pop from bucket free list. */
		bucket_free_t *f = bucket_heads[bi];
		if (f) {
			bucket_heads[bi] = f->next;
			bucket_counts[bi]--;
			spin_unlock_irqrestore(&heap_lock, flags);
			return (void *)f;
		}

		/* Slow path: allocate from underlying heap with exact bucket size. */
		void *ptr = heap_alloc_locked(bsize);
		spin_unlock_irqrestore(&heap_lock, flags);
		return ptr;
	}

	/* Large allocation: fall through to heap. */
	uint64_t aligned = align_up(size, HEAP_ALIGN);
	uint64_t flags = spin_lock_irqsave(&heap_lock);
	void *ptr = heap_alloc_locked(aligned);
	spin_unlock_irqrestore(&heap_lock, flags);
	return ptr;
}

void *
kzalloc(uint64_t size)
{
	void *p = kmalloc(size);
	if (!p)
		return NULL;
	kmemzero(p, size);
	return p;
}

void *
kmemcpy(void *dst, const void *src, uint64_t n)
{
	uint8_t *d = (uint8_t *) dst;
	const uint8_t *s = (const uint8_t *)src;
	/* Word-sized bulk copy. */
	uint64_t words = n / 8;
	uint64_t *dw = (uint64_t *) d;
	const uint64_t *sw = (const uint64_t *)s;
	for (uint64_t i = 0; i < words; i++)
		dw[i] = sw[i];
	/* Tail bytes. */
	for (uint64_t i = words * 8; i < n; i++)
		d[i] = s[i];
	return dst;
}


void *
kmemzero(void *dst, uint64_t n)
{
	uint8_t *d = (uint8_t *) dst;
	for (uint64_t i = 0; i < n; i++)
		d[i] = 0;
	return dst;
}

void
kfree(void *ptr)
{
	if (!ptr)
		return;
	uint64_t flags = spin_lock_irqsave(&heap_lock);
	heap_block_t *b =
	    (heap_block_t *) ((uint8_t *) ptr - sizeof(heap_block_t));

	/* If this block matches a bucket size and the bucket isn't full,
	 * cache it for fast reuse.  Otherwise fall through to normal free
	 * so the block can be coalesced with neighbors. */
	int bi = bucket_index_exact(b->size);
	if (bi >= 0 && bucket_counts[bi] < BUCKET_CAP) {
		bucket_free_t *f = (bucket_free_t *) ptr;
		f->next = bucket_heads[bi];
		bucket_heads[bi] = f;
		bucket_counts[bi]++;
		spin_unlock_irqrestore(&heap_lock, flags);
		return;
	}

	b->free = 1;

	/*
	 * O(1) coalescing: merge with adjacent neighbors only.
	 * Verified: models/heap_coalesce.pml (P1-P4).
	 */

	/* Coalesce with next block if adjacent and free. */
	if (b->next && b->next->free) {
		uint8_t *b_end =
		    (uint8_t *) b + sizeof(heap_block_t) + b->size;
		if (b_end == (uint8_t *) b->next) {
			heap_block_t *absorbed = b->next;
			b->size += sizeof(heap_block_t) + absorbed->size;
			b->next = absorbed->next;
			if (absorbed->next)
				absorbed->next->prev = b;
			else
				heap_tail = b;
		}
	}

	/* Coalesce with prev block if adjacent and free. */
	if (b->prev && b->prev->free) {
		uint8_t *prev_end =
		    (uint8_t *) b->prev + sizeof(heap_block_t) + b->prev->size;
		if (prev_end == (uint8_t *) b) {
			heap_block_t *p = b->prev;
			p->size += sizeof(heap_block_t) + b->size;
			p->next = b->next;
			if (b->next)
				b->next->prev = p;
			else
				heap_tail = p;
		}
	}

	spin_unlock_irqrestore(&heap_lock, flags);
}
