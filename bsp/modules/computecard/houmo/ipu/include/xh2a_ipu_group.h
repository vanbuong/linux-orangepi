// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_GROUP_H_
#define _XH2A_IPU_GROUP_H_

#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/types.h>
#include <xh2a_ipu_internal.h>

#include "xh2a_ipu_file_handle.h"

#define XH2A_SYNC_GROUP_TIMEOUT_MS 1000
#define XH2A_GROUP_RESULT_BUF_SZ   1
#define XH2A_GROUP_RESULT_NUM	   8
#define XH2A_GROUP_MAX_KERNEL_NUM  255
#define GROUP_FAIL_THRESHOLD	   0xFFFF
#define XH2A_GROUP_PARAM_DDR	   BIT(0)
#define XH2A_GROUP_PARAM_SPM	   BIT(1)

/* error code */
enum ipu_err_code {
	XH2A_GROUP_TRIGGER_TIMEOUT = 0xDEAD0001,
	XH2A_GROUP_KERNELS_TIMEOUT,
	XH2A_GROUP_TRANSFER_SPM_FAIL,
	XH2A_GROUP_ALLOC_SPM_FAIL,
	XH2A_GROUP_FREE_SPM_FAIL,
	XH2A_ERR_CODE_NUM,
};

/**
 * group status
 * @GROUP_CREATED: group has been created, but no kernel has been loaded
 * @GROUP_PACKING: group has been created, kernel is loaded one after another
 * @GROUP_PENDING: all kernels already loaded, start executing group,
 *                 but KDs not enqueue hw queue
 * @GROUP_RUNNING: KDs have been enqueued, but group not finished
 * @GROUP_DONE:    group has finished, but resource not released
 * @GROUP_CANCEL : group has been canceled by 'ctrl c'
 * @GROUP_DESTROY: group resource has been released
 */
enum group_status {
	GROUP_CREATED,
	GROUP_PACKING,
	GROUP_PENDING,
	GROUP_RUNNING,
	GROUP_DONE,
	GROUP_CANCEL,
	GROUP_DESTROY,
};

/**
 * struct xh2a_ipu_group - Represents a kernel group for IPU.
 * @id: Unique identifier for the group.
 * @core_num: Number of IPU cores needed for this group.
 * @tile_num: Number of tiles needed for this group on each core.
 * @kernel_num: Number of kernels belong with the group.
 * @mutex: Mutex for synchronizing operations on this group.
 * @status: Current status of the group, defined by the `group_status` enum.
 * @kernel_list: List of kernels associated with this group.
 * @file_handle: Pointer to the file handle associated with this group.
 * @ipu_dev: Pointer to the device associated with this group.
 * @wptr: Write pointer for tracking the group’s launching in the queue.
 * @tile_list_node: List node for linking this group in a
 *   tile-queue-based list.
 * @queue: Pointer to the tile queue launched by this group.
 * @exec_result: Execution results for the group, stored in a buffer.
 * @fail_count: Counter for scheduling failures of the group.
 * @sync_start: Time start when execute xh2a_execute_group function.
 * @load_start: Time when trigger booter.
 * @load_end: Time when group is done.
 * @trigger_timeout: timeout in us from execute interface to trigger booter.
 * @kernels_timeout: Total kernels execution timeout in us of the group.
 * @param_size: Total kernels params size of the group.
 * @param_type: bit0: DDR, bit1: SPM0, bit2: SPM1.
 * @param_addr: SPM physical address of the params for each core.
 * @policy_list_node: List node for linking this group in a policy-based list.
 * @launch_completion: Completion structure used for synchronizing launches.
 * @target: Core and queue allocation details for each core.
 *          - core_id: Identifier for the core.
 *          - queue_id: Identifier for the queue on the core.
 *          - end_wptr: End write pointer for the queue on the core.
 */
struct xh2a_ipu_booter_queue;
struct xh2a_ipu_group {
	uint32_t id;
	uint32_t core_num;
	uint32_t tile_num;
	uint32_t kernel_num;
	struct mutex mutex;
	atomic_t status;
	struct list_head kernel_list;

	struct xh2a_ipu_file_handle *file_handle;
	struct xh2a_ipu_device *ipu_dev;

	struct list_head tile_list_node;
	struct xh2a_ipu_booter_queue *queue;
	uint32_t exec_result[XH2A_GROUP_RESULT_BUF_SZ];

	uint32_t fail_count;
	ktime_t sync_start;
	ktime_t load_start;
	ktime_t load_end;
	uint64_t trigger_timeout;
	uint64_t kernels_timeout;

	uint32_t param_size;
	uint32_t param_type;
	uint64_t param_addr[XH2A_IPU_CORE_NUM];
	struct list_head policy_list_node;
	struct completion launch_completion;
	struct {
		int core_id;
		int queue_id;
		uint32_t end_wptr[XH2A_TILE_NUM_PER_CORE];
	} target[XH2A_IPU_CORE_NUM]; /* Queue allocation for each core */
};

/* group api */
struct xh2a_ipu_group *
xh2a_ipu_group_create(struct xh2a_ipu_file_handle *file_handle);
int xh2a_ipu_group_destroy(struct xh2a_ipu_group *group);
void xh2a_ipu_group_free_spm(struct xh2a_ipu_group *group);
void xh2a_group_host_param_free(struct xh2a_ipu_device *ipu_dev,
				struct xh2a_ipu_group *group);
int xh2a_ipu_group_add_kernel(struct xh2a_ipu_group *group,
			      struct ipu_kernel_launch_data *kld);
int xh2a_ipu_group_execute(struct xh2a_ipu_group *group,
			   uint32_t trigger_timeout);
void xh2a_ipu_group_get_result(struct xh2a_ipu_group *group,
			       struct xh2a_group_result *result,
			       uint32_t *group_num);
bool xh2a_ipu_is_group_done(struct xh2a_ipu_group *group, uint32_t current_rptr,
			    uint32_t last_rptr);
void xh2a_ipu_group_kds_load(struct xh2a_ipu_group *group);
bool priority_resource_enough(struct xh2a_ipu_group *group);
int xh2a_ipu_group_spm_alloc(struct xh2a_ipu_group *group);

#endif /* _XH2A_IPU_GROUP_H_ */