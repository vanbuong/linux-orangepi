// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#include <linux/slab.h>
#include <xh2a_rpmsg_lite_api.h>
#include "xh2a_rpmsg_lite_ext.h"
#include <xh2a_rpmsg_internal.h>

#define XH2A_RPMSG_GENERIC_EPT_NAME "xh2a_rpmsg_generic"
#define XH2A_RPMSG_GENERIC_EPT_ADDR 24

int xh2a_rpmsg_lite_ext_init(struct xh2a_rpmsg_lite_dev *rldev)
{
	struct xh2a_rpmsg_ept *ept;
	struct xh2a_rpmsg_link *link;
	struct xh2a_rpmsg_ept_info ept_info;
	struct xh2a_rpmsg_lite_ext *ext;

	ext = kzalloc(sizeof(struct xh2a_rpmsg_lite_ext), GFP_KERNEL);
	if (!ext) {
		pr_err("%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	sprintf(ept_info.name, "%s0", XH2A_RPMSG_GENERIC_EPT_NAME);
	ept_info.addr = XH2A_RPMSG_GENERIC_EPT_ADDR;

	link = &rldev->links[0].link;
	ept = xh2a_rpmsg_create_ept(link, NULL, NULL, &ept_info);
	if (!ept) {
		pr_err("%s: create generic ept failed\n", __func__);
		return -EFAULT;
	}

	ext->ept = ept;
	rldev->ext = ext;
	mutex_init(&rldev->ext_lock);

	return 0;
}

void xh2a_rpmsg_lite_ext_deinit(struct xh2a_rpmsg_lite_dev *rldev)
{
	struct xh2a_rpmsg_lite_ext *ext;

	if (rldev->ext == NULL)
		return;

	mutex_lock(&rldev->ext_lock);

	ext = rldev->ext;
	if (ext) {
		xh2a_rpmsg_destroy_ept(ext->ept);
		rldev->ext = NULL;
		kfree(ext);
	}

	mutex_unlock(&rldev->ext_lock);
}

int xh2a_rpmsg_lite_ext_send(void *handle, uint32_t dst, void *data, int len)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_rpmsg_lite_dev *rldev;
	struct xh2a_rpmsg_lite_ext *ext;

	xh2a_pcie_get_client(handle, &client, XH2A_RPMSG_DEVICE_NAME);
	if (client == NULL) {
		pr_err("%s: client not found\n", __func__);
		return -EINVAL;
	}

	rldev = client->client_data;

	if (rldev->ext == NULL)
		return -EFAULT;

	mutex_lock(&rldev->ext_lock);

	ext = rldev->ext;
	if (!ext) {
		pr_err("%s: NULL pointer of ext\n", __func__);
		ret = -EFAULT;
		goto unlock;
	}

	ret = xh2a_rpmsg_send_kern(ext->ept, dst, data, len);

unlock:
	mutex_unlock(&rldev->ext_lock);

	return ret;
}
EXPORT_SYMBOL(xh2a_rpmsg_lite_ext_send);
