// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_HW_H_
#define _XH2A_IPU_HW_H_

#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/types.h>

#define XH2A_IPU_TILE_ENTRY_NUM 1024

/**
 * struct xh2a_ipu_booter_queue - Represents a tile queue in the IPU subsystem
 * @core_id: The identifier for the core associated with this tile queue
 * @tile_id: The identifier for the tile within the IPU
 * @reg_base: Base address of the tile's memory-mapped I/O registers
 * @wptr: Write pointer for the queue, indicating the next write position
 * @rptr: Read pointer for the queue, indicating the next read position
 * @last_rptr: Last read pointer for the queue, used for tracking reads
 * @queue_addr: Physical address of the queue buffer in memory
 * @entry_capacity: Maximum number of KD entries the queue can hold
 * @flag: Status or configuration flags associated with the tile queue
 * @group_list: Linked list for managing group associations of this queue
 * @tile_mutex: Mutex for synchronizing access to the tile queue structure
 */
struct xh2a_ipu_booter_queue {
	uint16_t core_id;
	uint16_t tile_id;

	void __iomem *reg_base;
	uint32_t wptr;
	uint32_t rptr;
	uint32_t last_rptr;
	uint64_t *queue_addr;
	uint32_t entry_capacity;

	uint32_t flag;
	struct list_head group_list;
	struct mutex tile_mutex;
};

/* booter api */
struct xh2a_ipu_device;
struct xh2a_ipu_group;

void xh2a_ipu_booter_pre_startup(struct xh2a_ipu_device *ipu_dev);

void xh2a_ipu_hw_startup(struct xh2a_ipu_device *ipu_dev);

void xh2a_ipu_hw_shutdown(struct xh2a_ipu_device *ipu_dev);

int xh2a_ipu_get_efuse_info(struct xh2a_ipu_device *ipu_dev);

void xh2a_ipu_booter_get_hw_context(struct xh2a_ipu_device *ipu_device,
				    uint32_t *result);

void xh2a_booter_queue_enqueue_group(struct xh2a_ipu_device *ipu_dev,
				     struct xh2a_ipu_booter_queue *queue,
				     struct xh2a_ipu_group *group);

void xh2a_booter_queue_trigger(struct xh2a_ipu_device *ipu_dev,
			       struct xh2a_ipu_booter_queue *queue,
			       struct xh2a_ipu_group *group);

void xh2a_booter_queue_clear_intr(struct xh2a_ipu_device *ipu_dev,
				  struct xh2a_ipu_booter_queue *queue);

bool xh2a_booter_queue_get_intr_flag(struct xh2a_ipu_device *ipu_device,
				     struct xh2a_ipu_booter_queue *queue);

bool xh2a_booter_queue_is_full(struct xh2a_ipu_device *ipu_dev,
			       struct xh2a_ipu_booter_queue *queue);

uint32_t xh2a_booter_queue_get_rptr(struct xh2a_ipu_device *ipu_dev,
				    struct xh2a_ipu_booter_queue *queue);

void xh2a_booter_update_other_queues_rptr(struct xh2a_ipu_device *ipu_dev,
					  struct xh2a_ipu_group *group);

uint32_t xh2a_booter_queue_remain_space(struct xh2a_ipu_booter_queue *queue);

void xh2a_dump_debug_regs(struct xh2a_ipu_device *ipu_dev,
			  struct xh2a_ipu_group *group);

void xh2a_ipu_update_load(struct xh2a_ipu_device *ipu_device,
			  uint32_t load_value);

static inline uint32_t xh2a_ipu_calc_queue_delta(uint32_t left, uint32_t right)
{
	return (XH2A_IPU_TILE_ENTRY_NUM + right - left) %
	       XH2A_IPU_TILE_ENTRY_NUM;
}

#endif /* _XH2A_IPU_HW_H_ */