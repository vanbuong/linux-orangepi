// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_POLICY_H_
#define _XH2A_IPU_POLICY_H_

#include "xh2a_ipu_device.h"

struct xh2a_ipu_policy;
struct xh2a_ipu_group;

struct xh2a_ipu_policy_ops {
	void (*enqueue_group)(struct xh2a_ipu_policy *,
			      struct xh2a_ipu_group *);
	struct xh2a_ipu_group *(*pick_runnable_group)(struct xh2a_ipu_policy *);
	bool (*resource_enough)(struct xh2a_ipu_group *);
	void (*remove_group)(struct xh2a_ipu_policy *, struct xh2a_ipu_group *);
	bool (*can_allocate_entries)(struct xh2a_ipu_policy *, int core_id,
				     int queue_id, int entry_num_needed,
				     int *remain_queue_entries);
};

struct xh2a_ipu_policy {
	struct xh2a_ipu_device *ipu_dev;
	const struct xh2a_ipu_policy_ops *ops;
};

/* policy api */
struct xh2a_ipu_policy *xh2a_ipu_policy_create(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_policy_destroy(struct xh2a_ipu_policy *policy);
void xh2a_ipu_policy_delete_group(struct xh2a_ipu_group *group);

#endif /* _XH2A_IPU_POLICY_H_ */
