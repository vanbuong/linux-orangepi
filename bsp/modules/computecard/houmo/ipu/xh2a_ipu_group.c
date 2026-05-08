// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/idr.h>

#include <xh2a_pcie_api.h>
#include <xh2a_spm_api.h>

#include "xh2a_ipu_file_handle.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_kernel.h"
#include "xh2a_ipu_ioctl.h"

struct xh2a_ipu_group *
xh2a_ipu_group_create(struct xh2a_ipu_file_handle *file_handle)
{
	int ret;
	struct xh2a_ipu_group *group = NULL;

	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;
	struct miscdevice *miscdev = &ipu_dev->miscdev;

	group = kzalloc(sizeof(struct xh2a_ipu_group), GFP_KERNEL);

	if (!group) {
		dev_err(miscdev->this_device, "%s: alloc group failed\n",
			__func__);
		return NULL;
	}

	ret = mutex_lock_interruptible(&file_handle->file_mutex);
	if (ret != 0) {
		dev_err(miscdev->this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return NULL;
	}

	group->id = idr_alloc_cyclic(&file_handle->group_idr, group, 0, 0,
				     GFP_KERNEL);

	if (group->id < 0) {
		mutex_unlock(&file_handle->file_mutex);
		dev_err(miscdev->this_device, "%s: idr_alloc group id failed\n",
			__func__);
		kfree(group);
		group = NULL;
		return NULL;
	}

	/* update file_handle min_id */
	if (group->id < file_handle->min_id)
		file_handle->min_id = group->id;

	mutex_unlock(&file_handle->file_mutex);

	mutex_init(&group->mutex);
	INIT_LIST_HEAD(&group->kernel_list);
	INIT_LIST_HEAD(&group->policy_list_node);
	INIT_LIST_HEAD(&group->tile_list_node);
	init_completion(&group->launch_completion);

	group->kernel_num = 0;
	group->file_handle = file_handle;
	group->ipu_dev = ipu_dev;
	atomic_set(&group->status, GROUP_CREATED);

	dev_dbg(miscdev->this_device, "group %u %p created\n", group->id,
		group);

	return group;
}

void xh2a_ipu_group_free_spm(struct xh2a_ipu_group *group)
{
	int idx, core_id;
	struct xh2a_ipu_device *ipu_dev = NULL;

	if (!group || !group->ipu_dev) {
		pr_err("%s: group is NULL\n", __func__);
		return;
	}

	ipu_dev = group->ipu_dev;

	for (idx = 0; idx < group->core_num; idx++) {
		core_id = group->target[idx].core_id;
		dev_dbg(group->ipu_dev->miscdev.this_device,
			"group %u %p free SPM%d: 0x%llx %u(0x%x)\n", group->id,
			group, core_id, group->param_addr[core_id],
			group->param_size, group->param_size);
		if (group->param_addr[core_id] && group->param_size) {
			xh2a_memory_allocator_kernel_free_spm(
				ipu_dev->private_data, core_id,
				group->param_size, group->param_addr[core_id]);
			group->param_addr[core_id] = 0;
		}
	}

	group->param_type = 0;
}

int xh2a_ipu_group_destroy(struct xh2a_ipu_group *group)
{
	int old_status;
	struct xh2a_ipu_kernel *kernel, *n;
	struct xh2a_ipu_device *ipu_dev = group->ipu_dev;
	struct miscdevice *miscdev = &ipu_dev->miscdev;

	dev_dbg(miscdev->this_device, "group %u %p destroy\n", group->id,
		group);

	old_status = atomic_xchg(&group->status, GROUP_DESTROY);
	if (old_status == GROUP_DESTROY) {
		dev_err(miscdev->this_device,
			"group id %d %p already destroyed\n", group->id, group);
		return -EAGAIN;
	}

	if ((old_status == GROUP_PENDING) || (old_status == GROUP_RUNNING)) {
		atomic_set(&group->status, old_status);
		dev_dbg(miscdev->this_device, "group id %d is in used, %d!\n",
			group->id, old_status);
		return -EBUSY;
	}

	mutex_lock(&group->mutex);

	if (group->param_type == XH2A_GROUP_PARAM_SPM)
		xh2a_ipu_group_free_spm(group);

	list_for_each_entry_safe(kernel, n, &group->kernel_list, node) {
		list_del_init(&kernel->node);
		xh2a_ipu_kernel_destroy(kernel);
	}

	mutex_unlock(&group->mutex);

	kfree(group);
	group = NULL;

	return 0;
}

int xh2a_ipu_group_add_kernel(struct xh2a_ipu_group *group,
			      struct ipu_kernel_launch_data *kld)
{
	struct xh2a_ipu_kernel *kernel = NULL;
	struct xh2a_ipu_file_handle *file_handle = group->file_handle;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;
	struct miscdevice *miscdev = &ipu_dev->miscdev;
	uint32_t index;

	if (group->kernel_num >= XH2A_GROUP_MAX_KERNEL_NUM) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group %u %p unsupport kernel num: %u\n", __func__,
			group->id, group, group->kernel_num);

		return -ENOMEM;
	}

	if ((atomic_read(&group->status) != GROUP_CREATED) &&
	    (atomic_read(&group->status) != GROUP_PACKING)) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group status error %d\n", __func__, group->id);

		return -EACCES;
	}

	kernel = xh2a_ipu_kernel_create(group, kld);
	if (!kernel) {
		dev_err(miscdev->this_device,
			"%s: xh2a_ipu_kernel_create failed\n", __func__);
		return -ENOMEM;
	}

	if (group->param_size + kld->param_size > XH2A_SPM_TOTAL_SIZE) {
		dev_err(miscdev->this_device,
			"%s: param size exceed, group_id %d\n", __func__,
			group->id);
		xh2a_ipu_kernel_destroy(kernel);

		return -EINVAL;
	}

	index = group->kernel_num;
	group->kernel_num++;
	group->kernels_timeout += kld->timeout_us;
	group->param_size += kld->param_size;
	list_add_tail(&kernel->node, &group->kernel_list);

	atomic_set(&group->status, GROUP_PACKING);

	dev_dbg(miscdev->this_device,
		"group %u %p add kernel%u: 0x%llx(0x%x) param: %u "
		"0x%llx(0x%x)\n",
		group->id, group, index, kernel->kld.kernel_addr,
		kernel->kld.kernel_size, kernel->kld.param_type,
		kernel->kld.param_phy_addr, kernel->kld.param_size);

	return 0;
}

int xh2a_ipu_group_execute(struct xh2a_ipu_group *group,
			   uint32_t trigger_timeout)
{
	int ret;
	struct xh2a_ipu_file_handle *file_handle = group->file_handle;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;

	if (!ipu_dev->policy) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: ipu policy is NULL\n", __func__);
		return -ENODEV;
	}

	group->sync_start = ktime_get();
	group->trigger_timeout = trigger_timeout;
	dev_dbg(ipu_dev->miscdev.this_device,
		"group %u %p, %u kernels, %u cores, start = %llu, timeout = "
		"%llu\n",
		group->id, group, group->kernel_num, group->core_num,
		group->sync_start, group->trigger_timeout);

	/* replace by policy api */
	ipu_dev->policy->ops->enqueue_group(ipu_dev->policy, group);
	dev_dbg(ipu_dev->miscdev.this_device,
		"group %u %p enqueue to policy done\n", group->id, group);
	/* wakeup wq to execute group */
	queue_work(ipu_dev->group_wq, &ipu_dev->group_sche_work);

	ret = wait_for_completion_interruptible_timeout(
		&group->launch_completion,
		usecs_to_jiffies(group->trigger_timeout +
				 group->kernels_timeout));

	if (atomic_read(&ipu_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		ret = -ENODEV;
		goto exec_err;
	}
	if (ret == 0) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group %u %p sync timeout\n", __func__, group->id,
			group);
		group->exec_result[0] = XH2A_GROUP_KERNELS_TIMEOUT;

		ret = -ETIMEDOUT;
		goto exec_err;
	} else if (ret < 0) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group sync interrupted\n", __func__);

		ret = -EIO;
		goto exec_err;
	}

	if (group->exec_result[0] == XH2A_GROUP_TRIGGER_TIMEOUT) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group trigger timeout\n", __func__);
		ret = -ETIMEDOUT;
		goto exec_err;
	}

	if (group->exec_result[0] == XH2A_GROUP_ALLOC_SPM_FAIL) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group alloc spm failed\n", __func__);

		ret = -ENOMEM;
		goto exec_err;
	}

	if (group->exec_result[0] == XH2A_GROUP_TRANSFER_SPM_FAIL) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group transfer spm failed\n", __func__);

		ret = -EFAULT;
		goto exec_err;
	}

	dev_dbg(ipu_dev->miscdev.this_device, "group %u %p sync done\n",
		group->id, group);

	return 0;

exec_err:
	if (ret == -ETIMEDOUT)
		xh2a_dump_debug_regs(ipu_dev, group);

	if (ret != -EIO) {
		if (group->queue) {
			mutex_lock(&group->queue->tile_mutex);
			if (!list_empty(&group->tile_list_node))
				list_del_init(&group->tile_list_node);
			mutex_unlock(&group->queue->tile_mutex);
		}

		atomic_set(&group->status, GROUP_DONE);
	}

	return ret;
}

bool xh2a_ipu_is_group_done(struct xh2a_ipu_group *group, uint32_t current_rptr,
			    uint32_t last_rptr)
{
	uint32_t group_wptr = group->target[0].end_wptr[0];
	uint32_t done_off = xh2a_ipu_calc_queue_delta(last_rptr, group_wptr);
	uint32_t curr_off = xh2a_ipu_calc_queue_delta(last_rptr, current_rptr);

	return done_off <= curr_off;
}

static int xh2a_group_transfer_param2spm(struct xh2a_ipu_device *ipu_dev,
					 struct xh2a_ipu_group *group,
					 uint32_t core_id)
{
	int ret;
	struct xh2a_ipu_kernel *kernel, *kernel_n;

	list_for_each_entry_safe(kernel, kernel_n, &group->kernel_list, node) {
		dev_dbg(ipu_dev->miscdev.this_device,
			"group %u %p kernel->host_param 0x%llx, size=0x%x\n",
			group->id, group, (uint64_t)kernel->host_param,
			kernel->kld.param_size);

		ret = xh2a_kernel_transfer_param2spm(kernel, core_id);
		if (ret) {
			dev_err(ipu_dev->miscdev.this_device,
				"%s: group %u %p write kernel param failed\n",
				__func__, group->id, group);
			return ret;
		}
	}

	return 0;
}

void xh2a_group_host_param_free(struct xh2a_ipu_device *ipu_dev,
				struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_kernel *kernel, *kernel_n;

	list_for_each_entry_safe(kernel, kernel_n, &group->kernel_list, node) {
		dev_dbg(ipu_dev->miscdev.this_device,
			"group %u %p free host_param 0x%llx, size=0x%x\n",
			group->id, group, (uint64_t)kernel->host_param,
			kernel->kld.param_size);

		xh2a_kernel_host_param_free(kernel);
	}
}

static int
xh2a_ipu_group_transfer_param_to_spm(struct xh2a_ipu_device *ipu_device,
				     struct xh2a_ipu_group *group)
{
	int i, ret, core_id, free_core_id;

	mutex_lock(&group->mutex);
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		ret = xh2a_group_transfer_param2spm(ipu_device, group, core_id);
		if (ret) {
			dev_err(ipu_device->miscdev.this_device,
				"%s: %d: group %u %p transfer param2spm "
				"coreid%d failed\n",
				__func__, i, group->id, group, core_id);

			goto free_spm;
		}
	}
	mutex_unlock(&group->mutex);

	return 0;

free_spm:
	for (i = 0; i <= core_id; i++) {
		free_core_id = group->target[i].core_id;

		xh2a_memory_allocator_kernel_free_spm(
			ipu_device->private_data, free_core_id,
			group->param_size, group->param_addr[free_core_id]);

		group->param_addr[free_core_id] = 0;
	}

	group->exec_result[0] = XH2A_GROUP_TRANSFER_SPM_FAIL;
	xh2a_group_host_param_free(ipu_device, group);
	mutex_unlock(&group->mutex);
	complete(&group->launch_completion);

	return ret;
}

void xh2a_ipu_group_kds_load(struct xh2a_ipu_group *group)
{
	int i, j, ret, core_id, queue_id;

	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_device *ipu_device = group->ipu_dev;

	if (ktime_before(ktime_add_ns(group->sync_start,
				      group->trigger_timeout * NSEC_PER_USEC),
			 ktime_get())) {
		dev_err(ipu_device->miscdev.this_device,
			"group %u %p timeout: %llu\n", group->id, group,
			ktime_get());

		group->exec_result[0] = XH2A_GROUP_TRIGGER_TIMEOUT;
		complete(&group->launch_completion);

		return;
	}

	if (group->param_type & XH2A_GROUP_PARAM_SPM) {
		ret = xh2a_ipu_group_transfer_param_to_spm(ipu_device, group);
		if (ret) {
			dev_err(ipu_device->miscdev.this_device,
				"%s: group %u %p write param to spm failed\n",
				__func__, group->id, group);
			return;
		}
	}
	/* enqueue KDs, set first queue interrupt flag */
	mutex_lock(&group->mutex);
	atomic_set(&group->status, GROUP_RUNNING);
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		for (j = 0; j < group->tile_num; j++) {
			queue_id = group->target[i].queue_id + j;
			queue = &ipu_device->tile_queues[core_id][queue_id];

			mutex_lock(&queue->tile_mutex);
			/* set interrupt flag to the first launched queue */
			if (i == 0 && j == 0)
				queue->flag |=
					XH2A_IPU_TILE_QUEUE_FLAG_INTERRUPT;

			xh2a_booter_queue_enqueue_group(
				ipu_device,
				&ipu_device->tile_queues[core_id][queue_id],
				group);
			group->target[i].end_wptr[j] = queue->wptr;

			mutex_unlock(&queue->tile_mutex);
		}
	}

	/* trigger booter execute kernels */
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		for (j = 0; j < group->tile_num; j++) {
			queue_id = group->target[i].queue_id + j;
			queue = &ipu_device->tile_queues[core_id][queue_id];

			mutex_lock(&queue->tile_mutex);
			xh2a_booter_queue_trigger(ipu_device, queue, group);
			mutex_unlock(&queue->tile_mutex);
		}
	}

	group->load_start = ktime_to_us(ktime_get());
	/* record ipu load on enqueue */
	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;
		xh2a_ipu_load_on_enqueue(ipu_device, core_id,
					 group->load_start);
	}

	mutex_unlock(&group->mutex);
}

int xh2a_ipu_group_spm_alloc(struct xh2a_ipu_group *group)
{
	int ret, i, j, core_id, free_core_id;
	uint64_t param_addr;
	struct xh2a_ipu_device *ipu_device = group->ipu_dev;

	mutex_lock(&group->mutex);

	for (i = 0; i < group->core_num; i++) {
		core_id = group->target[i].core_id;

		ret = xh2a_memory_allocator_kernel_alloc_spm(
			ipu_device->private_data, core_id, group->param_size,
			&param_addr);
		if (ret) {
			dev_err(ipu_device->miscdev.this_device,
				"%s: group %u %p alloc SPM%d failed\n",
				__func__, group->id, group, core_id);
			mutex_unlock(&group->mutex);
			goto err_spm_alloc;
		}

		group->param_addr[core_id] = param_addr;
		dev_dbg(group->ipu_dev->miscdev.this_device,
			"group %u %p alloc SPM%d: 0x%llx %u(0x%x)\n", group->id,
			group, core_id, param_addr, group->param_size,
			group->param_size);
	}
	mutex_unlock(&group->mutex);

	return 0;

err_spm_alloc:
	/* free SPM for all cores */
	for (j = 0; j < i; j++) {
		free_core_id = group->target[j].core_id;

		xh2a_memory_allocator_kernel_free_spm(
			ipu_device->private_data, free_core_id,
			group->param_size, group->param_addr[free_core_id]);

		group->param_addr[free_core_id] = 0;
	}

	return ret;
}

void xh2a_ipu_group_get_result(struct xh2a_ipu_group *group,
			       struct xh2a_group_result *result,
			       uint32_t *group_num)
{
	if (atomic_read(&group->status) == GROUP_DONE) {
		result->result[*group_num].group_id = group->id;
		memcpy(result->result[*group_num].buf, group->exec_result,
		       sizeof(group->exec_result));

		(*group_num)++;
	}
}
