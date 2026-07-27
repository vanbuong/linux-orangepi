// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/slab.h>
#include <linux/uaccess.h>

#include <xh2a_address.h>

#include "xh2a_ipu_device.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_kernel.h"
#include "xh2a_ipu_config.h"

struct xh2a_ipu_kernel *
xh2a_ipu_kernel_create(struct xh2a_ipu_group *group,
		       struct ipu_kernel_launch_data *kld)
{
	struct xh2a_ipu_kernel *kernel = NULL;
	struct xh2a_ipu_device *ipu_dev = group->ipu_dev;
	struct miscdevice *miscdev = &ipu_dev->miscdev;

	if (!group || !kld) {
		pr_err("%s: group or kld is NULL\n", __func__);
		return NULL;
	}

	kernel = kzalloc(sizeof(struct xh2a_ipu_kernel), GFP_KERNEL);

	if (!kernel) {
		dev_err(miscdev->this_device, "%s: alloc kernel failed\n",
			__func__);

		return NULL;
	}

	memcpy(&kernel->kld, kld, sizeof(struct ipu_kernel_launch_data));
	kernel->group = group;

	dev_dbg(miscdev->this_device,
		"%s: group_id %d, core_num %d, tile_num %d\n", __func__,
		group->id, group->core_num, group->tile_num);

	dev_dbg(miscdev->this_device,
		"%s: group_id %d, param_type %d, timeout = %d\n", __func__,
		group->id, group->param_type, kernel->kld.timeout_us);

	INIT_LIST_HEAD(&kernel->node);

	/* record kernel prarm addr offset */
	kernel->param_spm_offset = group->param_size;

	if (group->param_type == XH2A_GROUP_PARAM_SPM) {
		void __user *param_uaddr;

		param_uaddr =
			(void __user *)(uintptr_t)kernel->kld.param_phy_addr;

		kernel->host_param =
			kzalloc(kernel->kld.param_size, GFP_KERNEL);
		if (kernel->host_param == NULL) {
			dev_err(miscdev->this_device,
				"%s: alloc host_param failed\n", __func__);
			kfree(kernel);
			kernel = NULL;

			return NULL;
		}

		if (copy_from_user(kernel->host_param, param_uaddr,
				   kernel->kld.param_size)) {
			dev_err(miscdev->this_device,
				"%s: param copy_from_user failed\n", __func__);
			kfree(kernel->host_param);
			kernel->host_param = NULL;
			kfree(kernel);
			kernel = NULL;

			return NULL;
		}
	}

	return kernel;
}

void xh2a_ipu_kernel_destroy(struct xh2a_ipu_kernel *kernel)
{
	if (!kernel) {
		pr_err("%s: kernel is NULL\n", __func__);
		return;
	}

	if (kernel->host_param) {
		kfree(kernel->host_param);
		kernel->host_param = NULL;
	}

	kfree(kernel);
	kernel = NULL;
}

void xh2a_kernel_host_param_free(struct xh2a_ipu_kernel *kernel)
{
	if (kernel->host_param) {
		kfree(kernel->host_param);
		kernel->host_param = NULL;
	}
}

int xh2a_kernel_transfer_param2spm(struct xh2a_ipu_kernel *kernel,
				   uint32_t core_id)
{
	struct xh2a_ipu_device *ipu_dev = kernel->group->ipu_dev;

	kernel->param_spm_addr[core_id] =
		kernel->param_spm_offset + kernel->group->param_addr[core_id];

	return xh2a_pcie_pio_write_mem(ipu_dev->private_data,
				       kernel->param_spm_addr[core_id],
				       kernel->host_param,
				       kernel->kld.param_size);
}
