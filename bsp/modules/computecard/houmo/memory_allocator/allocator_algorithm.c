// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

/*
 * first-fit memory allocator algorithm
 * mempool's start address is aligned up to 2^SEGMENT_SHIFT
 * mempool's size is aligned up to 2^SEGMENT_SHIFT
 * allocated buffer's start address aligned to
 *     min(ALIGN(buffer_size, SEGMENT_SHIFT),
 *         2^(SEGMENT_SHIFT + SEGMENT_ALIGNMENT))
 * allocated buffer's size is aligned up to 2^SEGMENT_SHIFT
 * SEGMENT_SHIFT and SEGMENT_ALIGNMENT differs based on mempool's size
 */

#include <linux/bitops.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/device.h>

#include "allocator_algorithm.h"

#define SEGMENT_SHIFT_LARGE	12
#define SEGMENT_SHIFT_SMALL	6
#define SEGMENT_ALIGNMENT_LARGE 8
#define SEGMENT_ALIGNMENT_SMALL 6
#define SEGMENT_LARGE_MEMORY	(512 * 1024 * 1024)

#define SEG_DOWN(mempool, x) (((uint64_t)(x) >> (mempool)->segment_shift))

#define PHYS_SEG(mempool, phys) (((uint64_t)(phys) >> (mempool)->segment_shift))

#define SEG_PHYS(mempool, seg) (((uint64_t)(seg) << (mempool)->segment_shift))

#define SEG_ALIGN(mempool, phys) \
	ALIGN((phys), (1ULL << (mempool)->segment_shift))

#define SEG_ALIGN_DOWN(mempool, phys) \
	ALIGN_DOWN((phys), (1ULL << (mempool)->segment_shift))

#define SEG_ALIGN_ORDER(mempool, size) \
	fls64(((size) - 1) >> (mempool)->segment_shift)

#define SEG_ALIGNED_MASK(mempool, align_order) ((1ULL << (align_order)) - 1)

#define SEG_ALIGNED_OFFSET(mempool, align_order) \
	((mempool)->segment_base & (1ULL << (align_order)))

int allocator_core_init(struct allocator_mempool *mem, struct device *dev,
			uint64_t start, uint64_t size)
{
	uint64_t bitmap_size;

	if (!mem || !dev) {
		pr_err("%s: mem is NULL\n", __func__);
		return -EINVAL;
	}

	mem->dev = dev;

	if (!start || !size) {
		dev_err(mem->dev,
			"%s: invalid parameter. start 0x%llx size 0x%llx\n",
			__func__, start, size);
		return -EINVAL;
	}

	if (size >= SEGMENT_LARGE_MEMORY) {
		mem->segment_shift = SEGMENT_SHIFT_LARGE;
		mem->segment_alignment = SEGMENT_ALIGNMENT_LARGE;
	} else {
		mem->segment_shift = SEGMENT_SHIFT_SMALL;
		mem->segment_alignment = SEGMENT_ALIGNMENT_SMALL;
	}

	mem->start = SEG_ALIGN(mem, start);
	mem->end = SEG_ALIGN_DOWN(mem, start + size);
	mem->size = mem->end - mem->start;
	mem->allocated_size = 0;
	mem->free_size = mem->size;
	mem->segment_base = PHYS_SEG(mem, mem->start);
	mem->segment_count = SEG_DOWN(mem, mem->size);

	bitmap_size = BITS_TO_LONGS(mem->segment_count) * sizeof(uint64_t);

	mem->bitmap = kzalloc(bitmap_size, GFP_KERNEL);

	if (!mem->bitmap) {
		dev_err(mem->dev, "%s: kzalloc bitmap failed\n", __func__);
		return -ENOMEM;
	}

	dev_dbg(mem->dev,
		"%s: start 0x%llx end 0x%llx size 0x%llx bitmap_size 0x%llx "
		"segment_base 0x%llx, segment_count 0x%llx\n",
		__func__, mem->start, mem->end, mem->size, bitmap_size,
		mem->segment_base, mem->segment_count);

	return 0;
}

int allocator_core_destroy(struct allocator_mempool *mem)
{
	if (!mem) {
		pr_err("%s: mem is NULL\n", __func__);
		return -1;
	}

	if (mem->bitmap)
		kfree(mem->bitmap);
	return 0;
}

uint64_t allocator_core_alloc(struct allocator_mempool *mem, uint64_t size)
{
	uint64_t alloc_size = 0;
	uint64_t align_order = 0;
	uint64_t align_mask = 0;
	uint64_t align_offset = 0;
	uint64_t bitmap_maxno = 0;
	uint64_t bitmap_no = 0;
	uint64_t bitmap_count = 0;
	uint64_t segn = 0;
	uint64_t allocated_paddr = 0;

	if (!mem) {
		pr_err("%s: mem is NULL\n", __func__);
		return 0;
	}

	if (size == 0 || size > mem->free_size) {
		dev_err(mem->dev, "%s: invalid size 0x%llx, free_size 0x%llx\n",
			__func__, size, mem->free_size);
		return 0;
	}

	/*
	 * mempool is sliced into contiguous segments.
	 * each segment is 2^segment_shift bytes.
	 * use bitmap to represent segments, one bit for one segment.
	 * alloc size is aligned up to the segment size.
	 * align_mask is used to mask contiguous segments.
	 * align_offset is used to mask the first segment.
	 */
	alloc_size = SEG_ALIGN(mem, size);
	align_order = min_t(uint64_t, SEG_ALIGN_ORDER(mem, alloc_size),
			    mem->segment_alignment);
	align_mask = SEG_ALIGNED_MASK(mem, align_order);
	align_offset = SEG_ALIGNED_OFFSET(mem, align_order);
	bitmap_maxno = mem->segment_count;
	bitmap_count = SEG_DOWN(mem, alloc_size);

	/*
	 * find a contiguous aligned zero area from align_offset.
	 * -bitmap_maxno: the max number of bits to search
	 * -bitmap_count: the number of bits to search
	 * -align_mask: the mask to align the found area
	 * -align_offset: the offset to align the found area
	 */
	bitmap_no = bitmap_find_next_zero_area_off(mem->bitmap, bitmap_maxno, 0,
						   bitmap_count, align_mask,
						   align_offset);

	if (bitmap_no >= bitmap_maxno) {
		dev_err(mem->dev,
			"%s: can't find enough continuous memory 0x%llx\n",
			__func__, alloc_size);
		return 0;
	}

	bitmap_set(mem->bitmap, bitmap_no, bitmap_count);

	mem->allocated_size += alloc_size;
	mem->free_size -= alloc_size;
	segn = mem->segment_base + bitmap_no;
	allocated_paddr = SEG_PHYS(mem, segn);

	return allocated_paddr;
}

int allocator_core_free(struct allocator_mempool *mem, uint64_t start,
			uint64_t size)
{
	uint64_t alloc_size = 0;
	uint64_t segn = 0;
	uint64_t bitmap_no = 0;
	uint64_t bitmap_count = 0;

	if (!mem) {
		pr_err("%s: mem is NULL\n", __func__);
		return -1;
	}

	if (start < mem->start || start > mem->end) {
		dev_err(mem->dev, "%s: invalid start 0x%llx\n", __func__,
			start);
		return -1;
	}

	if (size == 0 || size > mem->allocated_size) {
		dev_err(mem->dev,
			"%s: invalid size 0x%llx, allocated_size 0x%llx\n",
			__func__, size, mem->allocated_size);
		return -1;
	}

	alloc_size = SEG_ALIGN(mem, size);
	segn = PHYS_SEG(mem, start);
	bitmap_no = segn - mem->segment_base;
	bitmap_count = SEG_DOWN(mem, alloc_size);
	bitmap_clear(mem->bitmap, bitmap_no, bitmap_count);

	mem->allocated_size -= alloc_size;
	mem->free_size += alloc_size;

	return 0;
}

uint64_t allocator_core_get_max_free_buffer_size(struct allocator_mempool *mem)
{
	uint64_t max_free_size, align_order;
	uint64_t max_zero_segs, zero_segs;
	uint64_t pos, pos1, zero_start, zero_start_align, free_size;

	if (!mem) {
		pr_err("%s: mem is NULL\n", __func__);
		return 0;
	}

	max_zero_segs = 0;
	zero_start = 0;
	pos = 0;
	pos1 = 0;

	do {
		pos = find_next_zero_bit(mem->bitmap, mem->segment_count, pos1);
		pos1 = find_next_bit(mem->bitmap, mem->segment_count, pos);
		zero_segs = pos1 - pos;
		zero_start = pos;

		if (zero_segs > 0) {
			free_size = SEG_PHYS(mem, zero_segs);
			align_order = SEG_ALIGN_ORDER(mem, free_size);
			if (align_order > mem->segment_alignment)
				align_order = mem->segment_alignment;
			else if (align_order > 0)
				align_order--;
			zero_start_align =
				ALIGN(zero_start, (1 << align_order));
			zero_segs -= (zero_start_align - zero_start);
		}

		if (zero_segs > max_zero_segs)
			max_zero_segs = zero_segs;
	} while ((pos < mem->segment_count) || (pos1 < mem->segment_count));

	max_free_size = SEG_PHYS(mem, max_zero_segs);

	return max_free_size;
}
