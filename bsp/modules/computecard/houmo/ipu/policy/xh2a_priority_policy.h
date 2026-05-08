// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_PRIORITY_POLICY_H_
#define _XH2A_PRIORITY_POLICY_H_

#include "xh2a_ipu_policy.h"

#define TOTAL_PRIORITY_LEVELS 2

struct xh2a_priority_policy {
	struct xh2a_ipu_policy base;

	struct list_head priority_list[TOTAL_PRIORITY_LEVELS];
	struct mutex mutex;
};

/* priority policy api */
struct xh2a_ipu_policy *
xh2a_priority_policy_create(struct xh2a_ipu_device *ipu_dev);
void xh2a_priority_policy_destroy(struct xh2a_ipu_policy *policy);

#endif /* _XH2A_PRIORITY_POLICY_H_ */
