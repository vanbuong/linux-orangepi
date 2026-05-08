// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/slab.h>
#include "xh2a_ipu_policy.h"
#include "xh2a_ipu_group.h"
#include "xh2a_priority_policy.h"

struct xh2a_ipu_policy *xh2a_ipu_policy_create(struct xh2a_ipu_device *ipu_dev)
{
	/* priority policy default */
	return xh2a_priority_policy_create(ipu_dev);
}

void xh2a_ipu_policy_destroy(struct xh2a_ipu_policy *policy)
{
	if (!policy) {
		pr_err("%s: policy is NULL\n", __func__);
		return;
	}

	/* priority policy default */
	xh2a_priority_policy_destroy(policy);
	policy = NULL;
}

void xh2a_ipu_policy_delete_group(struct xh2a_ipu_group *group)
{
	struct xh2a_ipu_policy *policy;

	if (!group || !group->ipu_dev) {
		pr_err("%s: group, file_handle or device is NULL\n", __func__);
		return;
	}

	policy = group->ipu_dev->policy;
	if (!policy || !policy->ops || !policy->ops->remove_group) {
		pr_err("%s: policy or ops is NULL\n", __func__);
		return;
	}

	policy->ops->remove_group(policy, group);
}
