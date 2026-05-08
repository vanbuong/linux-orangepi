// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef __XH2A_MEMORY_TRANSFER_SYSDMA_H__
#define __XH2A_MEMORY_TRANSFER_SYSDMA_H__

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#define XH2A_MEMORY_TRANSFER_SYSDMA_CHANNEL_NUM	     4
#define XH2A_MEMORY_TRANSFER_SYSDMA_FAST_CHANNEL_NUM 4

struct xh2a_memory_transfer_sysdma_handle {
	struct device *dev;
	struct mutex sysdma_mutex;
	unsigned long chan_bitmap[BITS_TO_LONGS(
		XH2A_MEMORY_TRANSFER_SYSDMA_CHANNEL_NUM)];
	struct wait_queue_head chan_avail_wq;
	struct completion
		chan_completion[XH2A_MEMORY_TRANSFER_SYSDMA_CHANNEL_NUM];
	atomic_t chan_removed;
	struct work_struct sysdma_work;
	void *private_data;
};

int xh2a_memory_transfer_sysdma_init(
	struct xh2a_memory_transfer_sysdma_handle *handle, struct device *dev,
	void *private_data);

int xh2a_memory_transfer_sysdma_deinit(
	struct xh2a_memory_transfer_sysdma_handle *handle);

int xh2a_memory_transfer_sysdma_memcpy(
	struct xh2a_memory_transfer_sysdma_handle *handle, uint64_t src_addr,
	uint64_t dst_addr, uint64_t size);

int xh2a_memory_transfer_sysdma_pm_prepare(
	struct xh2a_memory_transfer_sysdma_handle *handle);

int xh2a_memory_transfer_sysdma_pm_complete(
	struct xh2a_memory_transfer_sysdma_handle *handle);

#endif
