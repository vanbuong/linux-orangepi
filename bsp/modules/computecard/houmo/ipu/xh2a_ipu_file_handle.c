// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/idr.h>

#include "xh2a_ipu_file_handle.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_ioctl.h"

struct xh2a_ipu_file_handle *
xh2a_ipu_file_handle_create(struct xh2a_ipu_device *ipu_dev)
{
	int ret;
	struct xh2a_ipu_file_handle *file_handle = NULL;
	struct miscdevice *miscdev = &ipu_dev->miscdev;

	file_handle = kzalloc(sizeof(struct xh2a_ipu_file_handle), GFP_KERNEL);

	if (!file_handle) {
		dev_err(miscdev->this_device, "%s: kzalloc failed\n", __func__);
		return NULL;
	}

	ret = mutex_lock_interruptible(&ipu_dev->fh_mutex);

	if (ret != 0) {
		dev_err(miscdev->this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return ERR_PTR(ret);
	}

	atomic_set(&file_handle->drop_flag, 0);
	mutex_init(&file_handle->file_mutex);
	idr_init(&file_handle->group_idr);
	file_handle->ipu_dev = ipu_dev;
	file_handle->min_id = INT_MAX;

	INIT_LIST_HEAD(&file_handle->node);
	list_add_tail(&file_handle->node, &ipu_dev->file_handle_list);

	mutex_unlock(&ipu_dev->fh_mutex);

	return file_handle;
}

static void
file_handle_destroy_all_groups(struct xh2a_ipu_file_handle *file_handle)
{
	int ret, id = file_handle->min_id;
	struct xh2a_ipu_group *group;

	while (true) {
		mutex_lock(&file_handle->file_mutex);

		group = idr_get_next(&file_handle->group_idr, &id);
		if (!group) {
			mutex_unlock(&file_handle->file_mutex);
			break;
		}

		id++;

		mutex_unlock(&file_handle->file_mutex);

		ret = xh2a_ipu_group_destroy(group);
		if (ret == -EBUSY)
			atomic_set(&group->status, GROUP_CANCEL);

		dev_dbg(file_handle->ipu_dev->miscdev.this_device,
			"removing group[%d].\n", id - 1);
	}

	file_handle->min_id = INT_MAX;
}

void xh2a_ipu_file_handle_destroy(struct xh2a_ipu_file_handle *file_handle)
{
	struct miscdevice *miscdev = NULL;
	struct xh2a_ipu_device *ipu_dev = NULL;

	if (!file_handle) {
		pr_err("%s: file_handle is NULL\n", __func__);
		return;
	}

	ipu_dev = file_handle->ipu_dev;

	if (!ipu_dev) {
		pr_err("%s: ipu_dev is NULL\n", __func__);
		return;
	}

	miscdev = &ipu_dev->miscdev;
	dev_dbg(miscdev->this_device, "%s: file_handle=%p\n", __func__,
		file_handle);

	file_handle_destroy_all_groups(file_handle);

	mutex_lock(&file_handle->file_mutex);
	idr_destroy(&file_handle->group_idr);
	mutex_unlock(&file_handle->file_mutex);

	mutex_lock(&ipu_dev->fh_mutex);

	list_del_init(&file_handle->node);

	mutex_unlock(&ipu_dev->fh_mutex);

	kfree(file_handle);

	dev_dbg(miscdev->this_device, "ipu release done\n");
}

struct xh2a_group_result *
xh2a_ipu_file_handle_get_group_result(struct xh2a_ipu_file_handle *file_handle)
{
	int id;
	uint32_t group_num = 0;
	struct xh2a_ipu_group *group = NULL;
	struct xh2a_group_result *group_result = NULL;

	group_result = kzalloc(sizeof(struct xh2a_group_result), GFP_KERNEL);

	if (!group_result) {
		dev_err(file_handle->ipu_dev->miscdev.this_device,
			"%s: kzalloc failed\n", __func__);
		return NULL;
	}

	mutex_lock(&file_handle->file_mutex);
	idr_for_each_entry(&file_handle->group_idr, group, id)
		xh2a_ipu_group_get_result(group, group_result, &group_num);

	mutex_unlock(&file_handle->file_mutex);

	group_result->group_num = group_num;

	return group_result;
}

void xh2a_ipu_file_handle_stop_group(struct xh2a_ipu_file_handle *file_handle)
{
	int id = file_handle->min_id;
	struct xh2a_ipu_group *group;

	while (true) {
		mutex_lock(&file_handle->file_mutex);

		group = idr_get_next(&file_handle->group_idr, &id);
		if (!group) {
			mutex_unlock(&file_handle->file_mutex);
			break;
		}

		id++;

		mutex_unlock(&file_handle->file_mutex);

		complete_all(&group->launch_completion);
	}
}
