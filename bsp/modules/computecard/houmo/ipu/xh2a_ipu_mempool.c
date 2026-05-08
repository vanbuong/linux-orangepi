// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/slab.h>
#include <linux/kernel.h>

#include <xh2a_address.h>

#include "xh2a_ipu_device.h"
#include "xh2a_ipu_mempool.h"
#include "xh2a_ipu_config.h"

void xh2a_ipu_event_pool_create(struct xh2a_event_pool *pool)
{
	int i;
	struct xh2a_ipu_event *event;
	struct xh2a_ipu_device *ipu_dev;

	ipu_dev = container_of(pool, struct xh2a_ipu_device, event_pool);

	INIT_LIST_HEAD(&pool->event_list);
	spin_lock_init(&pool->lock);

	/* pre-allocate capacity entries to event_list */
	for (i = 0; i < XH2A_IPU_EVENT_NUM; i++) {
		event = kzalloc(sizeof(struct xh2a_ipu_event),
			GFP_KERNEL);
		if (!event) {
			dev_err(ipu_dev->miscdev.this_device,
				"ipu event init fail, capcacity=%d\n", i);
			break;
		}

		INIT_LIST_HEAD(&event->node);
		list_add_tail(&event->node, &pool->event_list);
	}
}

void xh2a_ipu_event_pool_destroy(struct xh2a_event_pool *pool)
{
	struct xh2a_ipu_event *event, *event_n;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	list_for_each_entry_safe(event, event_n, &pool->event_list, node) {
		list_del_init(&event->node);
		kfree(event);
	}
	spin_unlock_irqrestore(&pool->lock, flags);

}

struct xh2a_ipu_event *xh2a_ipu_event_alloc(struct xh2a_event_pool *pool)
{
	struct xh2a_ipu_event *event = NULL;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	event = list_first_entry_or_null(&pool->event_list,
										struct xh2a_ipu_event, node);
	if (event)
		list_del_init(&event->node);
	spin_unlock_irqrestore(&pool->lock, flags);

	return event;
}

void xh2a_ipu_event_free(struct xh2a_event_pool *pool,
	struct xh2a_ipu_event *event)
{
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	list_add_tail(&event->node, &pool->event_list);
	spin_unlock_irqrestore(&pool->lock, flags);
}
