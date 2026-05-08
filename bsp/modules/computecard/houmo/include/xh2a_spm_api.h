// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_SPM_API_H_
#define _XH2A_SPM_API_H_

/*
 * xh2a_memory_allocator_kernel_alloc_spm() - allocate spm in kernel mode
 * @handle: handle for pcie device
 * @spmid: spm id, aka. SPM0 or SPM1
 * @size: size to allocate
 * @start: start address of the allocated memory
 * Return: 0 on success, negative error code on failure
 */
int xh2a_memory_allocator_kernel_alloc_spm(void *handle, int spmid, int size,
					   uint64_t *start);

/*
 * xh2a_memory_allocator_kernel_free_spm() - free spm in kernel mode
 * @handle: handle for pcie device
 * @spmid: spm id, aka. SPM0 or SPM1
 * @size: size to allocate
 * @start: start address of the allocated memory
 * Return: 0 on success, negative error code on failure
 */
int xh2a_memory_allocator_kernel_free_spm(void *handle, int spmid, int size,
					  uint64_t start);

#endif