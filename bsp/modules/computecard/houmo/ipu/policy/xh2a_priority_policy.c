// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include "xh2a_priority_policy.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_group.h"
#include <linux/slab.h>

/**
 * struct xh2a_target_allocation - Represents the allocation
 * information for a target.
 * This structure indicates the information required to and manage
 * the allocation of resources (cores and queues) for a specific target.
 * @remain_entries: The remaining number of queue entries available
 *                  for allocation.
 * @core_id: The ID of the core where the target is allocated.
 * @queue_id: The starting queue ID for the target's allocation
 *            within the core.
 */
struct xh2a_target_allocation {
	int remain_entries;
	int core_id;
	int queue_id;
};

/**
 * priority_get_best_queues_start_id - Finds the best starting queue for
 * allocating a specified number of tiles and entries on a core.
 *
 * This function searches for the optimal starting queue on a specified
 * core that can allocate the required number of tiles and kernel entries.
 * It iterates through all possible start queues, checking each combination
 * to ensure the required resources (entries) can be allocated.
 * The optimal start queue is chosen based on the maximum of the minimum
 * remaining entries across all combinations. The function returns the index
 * of the best start queue if found, otherwise -1.
 *
 * @ipu_device: A pointer to the xh2a_ipu_device structure
 *              representing the IPU device.
 * @core_id: The ID of the core where the tiles and entries
 *           need to be allocated.
 * @tile_num_needed: The number of tiles required for the group.
 * @entry_num_needed: The number of kernel entries required for the group.
 * @max_min_remain_entries: A pointer to an integer that will store
 * the maximum value of the minimum remaining entries found across
 * the possible start queues.
 *
 * Return:
 * - The index of the best start queue that can allocate the required
 * tiles and entries, or -1 if no valid start queue is found.
 *
 * This function checks for each possible start queue and iterates over
 * the required number of tiles. For each tile, it checks if there are
 * enough entries in the queue to accommodate the kernel entries.
 * The function then selects the start queue that maximizes the
 * minimum remaining entries across all checked combinations.
 */
static int priority_get_best_queues_start_id(struct xh2a_ipu_device *ipu_device,
					     int core_id, int tile_num_needed,
					     int entry_num_needed,
					     int *max_min_remain_entries)
{
	int start_queue, remain_queue_entries, i;
	int best_start_queue = -1;
	struct xh2a_ipu_policy *policy = ipu_device->policy;
	/* to store the maximum value of the minimum remaining entries */
	*max_min_remain_entries = INT_MIN;

	/* Iterate over all possible start_queue */
	for (start_queue = 0;
	     start_queue <= XH2A_TILE_NUM_PER_CORE - tile_num_needed;
	     start_queue++) {
		int min_remain_queue_entries = INT_MAX;

		/* Check the continuous queue at the current start queue
		 * location */
		for (i = 0; i < tile_num_needed; i++) {
			int queue_id = start_queue + i;

			if (ipu_device->tile_queues[core_id][queue_id].flag &
			    XH2A_IPU_TILE_QUEUE_FLAG_BAD)
				return -1;

			if (!policy->ops->can_allocate_entries(
				    policy, core_id, queue_id, entry_num_needed,
				    &remain_queue_entries)) {
				min_remain_queue_entries = INT_MIN;
				/* if one queue cannot be allocated, skip the
				 * combination */
				break;
			}

			if (remain_queue_entries < min_remain_queue_entries)
				min_remain_queue_entries = remain_queue_entries;
		}

		/* Update best_start_queue if the minimum remaining entries
			for the current combination is larger */
		if (min_remain_queue_entries > *max_min_remain_entries) {
			*max_min_remain_entries = min_remain_queue_entries;
			best_start_queue = start_queue;
		}
	}

	dev_dbg(ipu_device->miscdev.this_device,
		"start queue=%d, min_remain_queue_entries=%d\n",
		best_start_queue, *max_min_remain_entries);

	return best_start_queue;
}

/**
 * insert_sort_allocation - Inserts a new allocation into a sorted array
 * of allocations.
 *
 * This function maintains a sorted list of the top allocations based on
 * remaining entries. When a new allocation is added, it shifts lower-priority
 * allocations down and inserts the new allocation at the correct position.
 *
 * @allocation: Array of xh2a_target_allocation structures to update.
 * @remain_entries: Remaining entries for the new allocation.
 * @core_id: Core ID for the new allocation.
 * @queue_id: Start queue ID for the new allocation.
 */
static void
sort_allocation_by_remain_entries(struct xh2a_target_allocation *allocation,
				  int remain_entries, int core_id, int queue_id)
{
	int i, j;

	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		if (remain_entries > allocation[i].remain_entries) {
			/* Shift lower-priority entries downwards */
			for (j = XH2A_IPU_CORE_NUM - 1; j > i; j--)
				allocation[j] = allocation[j - 1];

			/* Insert new result at position i */
			allocation[i].remain_entries = remain_entries;
			allocation[i].core_id = core_id;
			allocation[i].queue_id = queue_id;
			break;
		}
	}
}

static void sort_allocations_by_core_id(struct xh2a_target_allocation *alloc,
					int count)
{
	int i, j;

	for (i = 1; i < count; i++) {
		struct xh2a_target_allocation key = alloc[i];
		j = i - 1;

		while (j >= 0 && alloc[j].core_id > key.core_id) {
			alloc[j + 1] = alloc[j];
			j--;
		}
		alloc[j + 1] = key;
	}
}

/**
 * priority_resource_enough - Check if the required tiles and cores can be
 * allocated
 * @group: Pointer to the IPU group requesting resource allocation
 *
 * This function checks whether the required number of tiles, kernel entries,
 * and cores can be allocated for a given group. It uses a heuristic to
 * identify the best core and queue combinations, ensuring optimal resource
 * allocation. If the required resources are available, they are assigned
 * to the group's targets.
 *
 * Returns:
 * true  - Resources can be allocated.
 * false - Resources cannot be allocated.
 */
bool priority_resource_enough(struct xh2a_ipu_group *group)
{
	uint32_t mask;
	int core, i, ret, this_core;
	int min_remain_queue_entries, best_start_queue;

	struct xh2a_target_allocation allocation[XH2A_IPU_CORE_NUM];

	uint32_t tile_num_needed = group->tile_num;
	uint32_t entry_num_needed = group->kernel_num;
	uint32_t core_needed = group->core_num;
	struct xh2a_ipu_device *ipu_device = group->ipu_dev;

	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		allocation[i].remain_entries = -1;
		allocation[i].core_id = -1;
		allocation[i].queue_id = -1;
	}

	/* Iterate each core to find multiple optimal combinations */
	for (core = XH2A_IPU_CORE_NUM - 1; core >= 0; core--) {
		mask = (0x1 << core);
		if ((group->coremask != 0) && (mask & group->coremask) != mask)
			continue;

		best_start_queue = priority_get_best_queues_start_id(
			ipu_device, core, tile_num_needed, entry_num_needed,
			&min_remain_queue_entries);

		/* Check if this combination can be added to the top
		 * XH2A_IPU_CORE_NUM */
		if (min_remain_queue_entries >
		    allocation[XH2A_IPU_CORE_NUM - 1].remain_entries)
			/* Find the correct position to insert this result */
			sort_allocation_by_remain_entries(
				allocation, min_remain_queue_entries, core,
				best_start_queue);
	}

	for (i = 0; i < core_needed; i++) {
		if (allocation[i].queue_id == -1) {
			pr_info("%s, no available queue for group %u %p\n",
				__func__, group->id, group);
			return false;
		}
	}

	/* balance ipu load */
	if (core_needed == 1 &&
	    (group->coremask == 0 || group->coremask == 0x3)) {
		this_core = allocation[0].core_id;

		if (this_core == group->ipu_dev->last_core) {
			for (i = 1; i < XH2A_IPU_CORE_NUM; i++) {
				if (allocation[i].remain_entries ==
					    allocation[0].remain_entries &&
				    allocation[i].core_id != this_core &&
				    allocation[i].queue_id != -1) {
					allocation[0] = allocation[i];
					break;
				}
			}
		}

		group->ipu_dev->last_core = allocation[0].core_id;
	}

	/* sorted in ascending order by core id */
	if (core_needed > 1)
		sort_allocations_by_core_id(allocation, core_needed);

	mutex_lock(&group->mutex);
	/* Assign the optimal combinations to group->target */
	for (i = 0; i < core_needed; i++) {
		group->target[i].core_id = allocation[i].core_id;
		group->target[i].queue_id = allocation[i].queue_id;

		dev_dbg(ipu_device->miscdev.this_device,
			"group %u %p target[%d]: core_id = %d", group->id,
			group, i, allocation[i].core_id);
	}
	mutex_unlock(&group->mutex);

	/* allocate SPM memory if needed. If alloc failed, should not enqueue
	 KDs */

	if (group->param_type & ~XH2A_GROUP_PARAM_DDR) {
		ret = xh2a_ipu_group_spm_alloc(group);
		if (ret) {
			dev_err(ipu_device->miscdev.this_device,
				"%s: group %u %p alloc SPM failed\n", __func__,
				group->id, group);

			return false;
		}
	}

	return true;
}

static void priority_enqueue_group(struct xh2a_ipu_policy *base,
				   struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_device *ipu_dev = base->ipu_dev;
	struct xh2a_priority_policy *policy =
		container_of(base, struct xh2a_priority_policy, base);

	mutex_lock(&policy->mutex);
	xh2a_ipu_group_get(group);
	list_add_tail(&group->policy_list_node,
		      &policy->priority_list[TOTAL_PRIORITY_LEVELS - 1]);
	mutex_unlock(&policy->mutex);

	dev_dbg(ipu_dev->miscdev.this_device,
		"xh2a_priority_enqueue_group, id = %d\n", group->id);
}

static struct xh2a_ipu_group *
priority_pick_runnable_group(struct xh2a_ipu_policy *base)
{
	int i;
	struct xh2a_priority_policy *policy =
		container_of(base, struct xh2a_priority_policy, base);
	struct xh2a_ipu_group *group, *tmp;
	struct xh2a_ipu_device *ipu_dev = base->ipu_dev;

	mutex_lock(&policy->mutex);
	/* Iterate over all priority levels list */
	for (i = 0; i < TOTAL_PRIORITY_LEVELS; i++) {
		list_for_each_entry_safe(group, tmp, &policy->priority_list[i],
					 policy_list_node) {
			int status = atomic_read(&group->status);
			if (status == GROUP_CANCEL || status == GROUP_DESTROY ||
			    status == GROUP_DONE) {
				list_del_init(&group->policy_list_node);
				xh2a_ipu_group_put(group);
				continue;
			}
			/* Check if resources are sufficient for the group */
			if (priority_resource_enough(group)) {
				list_del_init(&group->policy_list_node);
				mutex_unlock(&policy->mutex);
				/* Found a runnable group */
				return group;
			} else {
				dev_dbg(ipu_dev->miscdev.this_device,
					"group %u %p is not runnable\n",
					group->id, group);
				group->fail_count++;
				/* Keep the groups executing in sequence,
					the priority policy has been reduced
					to the FIFO policy. */
				break;
				/* Promote the group to a higher priority list
				if fail count threshold is exceeded */
				if (group->fail_count > GROUP_FAIL_THRESHOLD &&
				    i > 0) {
					list_del_init(&group->policy_list_node);
					list_add_tail(
						&group->policy_list_node,
						&policy->priority_list[i - 1]);
				}
			}
		}

		/* If there are still groups in the current priority level */
		if (!list_empty(&policy->priority_list[i]))
			break;
	}
	mutex_unlock(&policy->mutex);

	return NULL;
}

static void priority_remove_group(struct xh2a_ipu_policy *base,
				  struct xh2a_ipu_group *group)
{
	struct xh2a_priority_policy *policy =
		container_of(base, struct xh2a_priority_policy, base);

	mutex_lock(&policy->mutex);

	if (!list_empty(&group->policy_list_node)) {
		list_del_init(&group->policy_list_node);
		xh2a_ipu_group_put(group);
	}

	mutex_unlock(&policy->mutex);
}

/* Checks whether the specified core and queue can allocate
 a specified number of entries */
static bool priority_can_allocate_entries(struct xh2a_ipu_policy *base,
					  int core_id, int queue_id,
					  int entry_num_needed,
					  int *remain_queue_entries)
{
	int queue_remain_entries;
	struct xh2a_priority_policy *policy =
		container_of(base, struct xh2a_priority_policy, base);
	struct xh2a_ipu_device *ipu_dev = policy->base.ipu_dev;

	queue_remain_entries = xh2a_booter_queue_remain_space(
		&ipu_dev->tile_queues[core_id][queue_id]);

	*remain_queue_entries = queue_remain_entries - entry_num_needed;

	dev_dbg(ipu_dev->miscdev.this_device,
		"remain = %d, needed = %d, wptr = %d, rptr = %d\n",
		queue_remain_entries, entry_num_needed,
		ipu_dev->tile_queues[core_id][queue_id].wptr,
		ipu_dev->tile_queues[core_id][queue_id].rptr);

	return *remain_queue_entries >= 0;
}

static const struct xh2a_ipu_policy_ops priority_policy_ops = {
	.enqueue_group = priority_enqueue_group,
	.pick_runnable_group = priority_pick_runnable_group,
	.resource_enough = priority_resource_enough,
	.remove_group = priority_remove_group,
	.can_allocate_entries = priority_can_allocate_entries,
};

struct xh2a_ipu_policy *
xh2a_priority_policy_create(struct xh2a_ipu_device *ipu_dev)
{
	int i;
	struct xh2a_priority_policy *policy;
	struct miscdevice *miscdev = &ipu_dev->miscdev;

	policy = kzalloc(sizeof(struct xh2a_priority_policy), GFP_KERNEL);
	if (!policy) {
		dev_err(miscdev->this_device,
			"%s: alloc priority_policy failed\n", __func__);
		return NULL;
	}

	mutex_init(&policy->mutex);

	for (i = 0; i < TOTAL_PRIORITY_LEVELS; i++)
		INIT_LIST_HEAD(&policy->priority_list[i]);

	policy->base.ipu_dev = ipu_dev;
	policy->base.ops = &priority_policy_ops;

	return &policy->base;
}

void xh2a_priority_policy_destroy(struct xh2a_ipu_policy *base)
{
	int i;
	struct xh2a_ipu_group *group, *tmp;
	struct xh2a_priority_policy *policy;

	if (!base)
		return;

	policy = container_of(base, struct xh2a_priority_policy, base);

	mutex_lock(&policy->mutex);
	for (i = 0; i < TOTAL_PRIORITY_LEVELS; i++) {
		list_for_each_entry_safe(group, tmp, &policy->priority_list[i],
					 policy_list_node) {
			list_del_init(&group->policy_list_node);
			xh2a_ipu_group_put(group);
		}
	}
	mutex_unlock(&policy->mutex);

	kfree(policy);
}
