// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_MEMORY_ALLOCATOR_H_
#define _XH2A_MEMORY_ALLOCATOR_H_

#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <xh2a_pcie_api.h>
#include <xh2a_memory_allocator_internal.h>
#include "allocator_algorithm.h"

#define XH2A_MEMORY_ALLOCATOR_DEVICE_NAME_LEN 64

/*
 * struct xh2a_memory_allocator_dev - xh2a memory_allocator device
 * @miscdev: misc device
 * @name_buf: name buffer
 * @allocator_mutex: mutex for memory allocator
 * @allocator_mempool: structure for memory pool
 * @file_handle_list: list for file handle
 * @start_addr: start address of memory allocator
 * @total_size: total size of memory allocator
 * @dev_initialized: flag for device initialized
 * @dev_removed: flag for device removed
 * @dev_refcnt: reference count for device
 * @private_data: handle for pcie
 */
struct xh2a_memory_allocator_dev {
	struct miscdevice miscdev;
	char name_buf[XH2A_MEMORY_ALLOCATOR_DEVICE_NAME_LEN];
	struct mutex allocator_mutex;
	struct allocator_mempool allocator_mempool;
	struct list_head file_handle_list;
	uint64_t start_addr;
	uint64_t total_size;
	atomic_t dev_initialized;
	atomic_t dev_removed;
	struct kref dev_refcnt;
	void *private_data;
	struct xh2a_pcie_client client;
};

/*
 * struct xh2a_memory_allocator_buffer_object - xh2a allocator buffer object
 * @node: list_head structure
 * @start: start address of buffer object
 * @size: size of buffer object
 * @pid: pid of buffer object
 * @tgid: tgid of buffer object
 */
struct xh2a_memory_allocator_buffer_object {
	struct list_head node;
	uint64_t start;
	uint64_t size;
	pid_t pid;
	pid_t tgid;
};

/*
 * struct xh2a_memory_allocator_file_handle - file handle
 * @node: list_head structure
 * @allocator: memory allocator device
 * @pid: pid of file handle
 * @tgid: tgid of file handle
 * @buffer_object_list: list for allocated buffer object
 */
struct xh2a_memory_allocator_file_handle {
	struct list_head node;
	struct xh2a_memory_allocator_dev *allocator;
	pid_t pid;
	pid_t tgid;
	struct list_head buffer_object_list;
};

#include <xh2a_address.h>
/*
 * XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_TOTAL_SIZE - total size for compiler
 * XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_START_ADDR - start address for compiler
 */
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_START_ADDR XH2A_DEVICE_DDR_START
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_TOTAL_SIZE XH2A_DEVICE_DDR_SIZE

/*
 * XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_START_ADDR - start address of SPM0
 * XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_TOTAL_SIZE - size of SPM0
 */
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_START_ADDR \
	XH2A_DEVICE_SPM0_POOL_START
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_TOTAL_SIZE XH2A_DEVICE_SPM0_POOL_SIZE

/*
 * XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_START_ADDR - start address of SPM1
 * XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_TOTAL_SIZE - size of SPM1
 */
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_START_ADDR \
	XH2A_DEVICE_SPM1_POOL_START
#define XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_TOTAL_SIZE XH2A_DEVICE_SPM1_POOL_SIZE

#endif
