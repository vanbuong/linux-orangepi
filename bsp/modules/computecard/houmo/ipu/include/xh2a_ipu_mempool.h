// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_MEMPOOL_H_
#define _XH2A_IPU_MEMPOOL_H_

#include <linux/list.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include "xh2a_ipu_config.h"

struct xh2a_ipu_booter_queue;
struct xh2a_ipu_event {
	struct list_head node;
	struct xh2a_ipu_booter_queue* queue;
};

struct xh2a_event_pool {
	struct list_head event_list;
	spinlock_t lock;
};

/* mempool api */
void xh2a_ipu_event_pool_create(struct xh2a_event_pool *pool);
void xh2a_ipu_event_pool_destroy(struct xh2a_event_pool *pool);

struct xh2a_ipu_event *xh2a_ipu_event_alloc(struct xh2a_event_pool *pool);
void xh2a_ipu_event_free(struct xh2a_event_pool *pool,
	struct xh2a_ipu_event *event);

#endif /* _XH2A_IPU_MEMPOOL_H_ */