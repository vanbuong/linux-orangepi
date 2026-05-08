// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef __ALLOCATOR_ALGORITHM_H__
#define __ALLOCATOR_ALGORITHM_H__

#include <linux/types.h>

/*
 * struct allocator_mempool - allocator memory pool
 * @dev: device pointer
 * @start: start address of memory pool in byte
 * @end: end address of memory pool in byte
 * @size: size of memory pool in byte
 * @allocated_size: allocated size of memory pool in byte
 * @free_size: free size of memory pool in byte
 * @segment_shift: segment shift of memory pool
 * @segment_alignment: segment alignment of memory pool
 * @segment_base: start address of memory pool in segments
 * @segment_count: number of segments in memory pool
 * @bitmap: bitmap of memory pool
 */
struct allocator_mempool {
	struct device *dev;
	uint64_t start;
	uint64_t end;
	uint64_t size;
	uint64_t allocated_size;
	uint64_t free_size;
	uint64_t segment_shift;
	uint64_t segment_alignment;
	uint64_t segment_base;
	uint64_t segment_count;
	unsigned long *bitmap;
};

/*
 * allocator_core_init() - initialize allocator memory pool
 * @mem: allocator memory pool
 * @dev: device handle
 * @start: start address of memory pool
 * @size: size of memory pool
 * Return: 0 on success, negative error code on failure
 */
int allocator_core_init(struct allocator_mempool *mem, struct device *dev,
			uint64_t start, uint64_t size);

/*
 * allocator_core_destroy() - destroy allocator memory pool
 * @mem: allocator memory pool
 * Return: 0 on success, negative error code on failure
 */
int allocator_core_destroy(struct allocator_mempool *mem);

/*
 * allocator_core_alloc() - allocate memory from allocator memory pool
 * @mem: allocator memory pool
 * @size: size of memory to allocate
 * Return: start address of allocated memory on success, 0 on failure
 */
uint64_t allocator_core_alloc(struct allocator_mempool *mem, uint64_t size);

/*
 * allocator_core_free() - free memory from allocator memory pool
 * @mem: allocator memory pool
 * @start: start address of memory to free
 * @size: size of memory to free
 * Return: 0 on success, negative error code on failure
 */
int allocator_core_free(struct allocator_mempool *mem, uint64_t start,
			uint64_t size);

/*
 * allocator_core_get_max_free_buffer_size() - get max free buffer size
 * @mem: allocator memory pool
 * Return: max free buffer size in bytes
 */
uint64_t allocator_core_get_max_free_buffer_size(struct allocator_mempool *mem);

#endif
