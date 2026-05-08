// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_FAST_MEMORY_H_
#define _XH2A_FAST_MEMORY_H_

#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <xh2a_pcie_api.h>
#include <xh2a_fast_memory_internal.h>
#include "../memory_allocator/allocator_algorithm.h"

#define XH2A_FAST_MEMORY_DEVICE_NAME_LEN 64
#define XH2A_FAST_MEMORY_DMA_SIZE	 0x200000
#define XH2A_FAST_MEMORY_POOL_START	 0x10000000
/*
 * struct xh2a_fast_memory_dev - xh2a fast memory device
 * @miscdev: misc device
 * @name_buf: name buffer
 * @fast_memory_mutex: mutex for fast memory
 * @mempool: memory pool for device ddr
 * @dma_vaddr: dma virtual address on host
 * @dma_paddr: dma physical address on host
 * @dma_size: dma size
 * @file_handle_list: list for file handle
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @dev_initialized: flag for device initialized
 * @dev_removed: flag for device removed
 * @dev_refcnt: reference count for device
 * @private_data: handle for pcie
 */
struct xh2a_fast_memory_dev {
	struct miscdevice miscdev;
	char name_buf[XH2A_FAST_MEMORY_DEVICE_NAME_LEN];
	struct mutex fast_memory_mutex;
	struct allocator_mempool mempool;
	void *dma_vaddr;
	dma_addr_t dma_paddr;
	size_t dma_size;
	struct list_head file_handle_list;
	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;
	atomic_t dev_initialized;
	atomic_t dev_removed;
	struct kref dev_refcnt;
	void *private_data;
	struct xh2a_pcie_client client;
};

struct xh2a_fast_memory_buffer_object {
	struct list_head node;
	uint64_t start;
	uint64_t size;
	pid_t pid;
};

struct xh2a_fast_memory_file_handle {
	struct list_head node;
	struct xh2a_fast_memory_dev *fast_memory;
	struct list_head buffer_object_list;
	unsigned long user_vaddr;
	pid_t pid;
};

#include <xh2a_address.h>
/*
 * XH2A_FAST_MEMORY_DDR_TOTAL_SIZE - total size for compiler
 * XH2A_FAST_MEMORY_DDR_START_ADDR - start address for compiler
 */
#define XH2A_FAST_MEMORY_DDR_START_ADDR XH2A_DEVICE_DDR_START
#define XH2A_FAST_MEMORY_DDR_TOTAL_SIZE XH2A_DEVICE_DDR_SIZE

/*
 * XH2A_FAST_MEMORY_SPM0_START_ADDR - start address of SPM0
 * XH2A_FAST_MEMORY_SPM0_TOTAL_SIZE - size of SPM0
 */
#define XH2A_FAST_MEMORY_SPM0_START_ADDR XH2A_DEVICE_SPM0_START
#define XH2A_FAST_MEMORY_SPM0_TOTAL_SIZE XH2A_DEVICE_SPM0_SIZE

/*
 * XH2A_FAST_MEMORY_SPM1_START_ADDR - start address of SPM1
 * XH2A_FAST_MEMORY_SPM1_TOTAL_SIZE - size of SPM1
 */
#define XH2A_FAST_MEMORY_SPM1_START_ADDR XH2A_DEVICE_SPM1_START
#define XH2A_FAST_MEMORY_SPM1_TOTAL_SIZE XH2A_DEVICE_SPM1_SIZE

/*
 * XH2A_FAST_MEMORY_TIM1_START_ADDR - start address of TIM1
 * XH2A_FAST_MEMORY_TIM1_TOTAL_SIZE - size of TIM1
 */
#define XH2A_FAST_MEMORY_TIM1_START_ADDR XH2A_DEVICE_TIM1_START
#define XH2A_FAST_MEMORY_TIM1_TOTAL_SIZE XH2A_DEVICE_TIM1_SIZE

#endif
