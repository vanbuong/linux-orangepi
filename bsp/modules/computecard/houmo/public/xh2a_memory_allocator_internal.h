// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_memory_allocator_internal.h

Abstract:

	This module contains the common declarations shared by driver
	and HAL for Memory Allocator module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/
#ifndef _XH2A_MEMORY_ALLOCATOR_INTERNAL_H_
#define _XH2A_MEMORY_ALLOCATOR_INTERNAL_H_

/*
 * XH2A_MEMORY_ALLOCATOR_DEVICE_NAME - unique name of xh2a memory allocator
 */
#define XH2A_MEMORY_ALLOCATOR_DEVICE_NAME "xh2a_memory_allocator_device"

 /*
  * XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_NAME - memory pool name for DDR
  * XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_NAME - memory pool name for SPM0
  * XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_NAME - memory pool name for SPM1
  */
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_NAME	"ddr"
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_NAME "spm0"
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_NAME "spm1"

  /*
   * struct xh2a_memory_allocator_request_arg - ioctl arg for alloc/free
   * @start: start address of request
   * @size: size of request
   */
struct xh2a_memory_allocator_request_arg {
	uint64_t start;
	uint64_t size;
};

/*
 * struct xh2a_memory_allocator_info_mem_arg - ioctl arg for info_mem
 * @start: start address of memory allocator
 * @total_size: total size of memory allocator
 * @free_size: free size of memory allocator
 * @max_free_buffer_size: max free contiguous buffer size
 */
struct xh2a_memory_allocator_info_mem_arg {
	uint64_t start;
	uint64_t total_size;
	uint64_t free_size;
	uint64_t max_free_buffer_size;
};

/*
 * xh2a_memory_allocator_ops - memory allocator operations
 */
enum xh2a_memory_allocator_ops {
	XH2A_MEMORY_ALLOCATOR_ALLOC = 0,
	XH2A_MEMORY_ALLOCATOR_FREE = 1,
	XH2A_MEMORY_ALLOCATOR_GET_MEM_INFO = 2,
};

#define XH2A_MEMORY_ALLOCATOR_IOC_MAGIC 'A'

#define IOCTL_XH2A_MEMORY_ALLOCATOR_ALLOC                                     \
	_IOWR(XH2A_MEMORY_ALLOCATOR_IOC_MAGIC, XH2A_MEMORY_ALLOCATOR_ALLOC, \
	      struct xh2a_memory_allocator_request_arg)

#define IOCTL_XH2A_MEMORY_ALLOCATOR_FREE                                    \
	_IOW(XH2A_MEMORY_ALLOCATOR_IOC_MAGIC, XH2A_MEMORY_ALLOCATOR_FREE, \
	     struct xh2a_memory_allocator_request_arg)

#define IOCTL_XH2A_MEMORY_ALLOCATOR_GET_MEM_INFO   \
	_IOR(XH2A_MEMORY_ALLOCATOR_IOC_MAGIC,    \
	     XH2A_MEMORY_ALLOCATOR_GET_MEM_INFO, \
	     struct xh2a_memory_allocator_info_mem_arg)

#endif // !_XH2A_MEMORY_ALLOCATOR_INTERNAL_H_
