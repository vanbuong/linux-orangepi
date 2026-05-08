// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <xh2a_pcie_api.h>
#include <xh2a_address.h>

#include "xh2a_ipu_interrupt.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_hw.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_mempool.h"
#include "xh2a_ipu_load.h"

static void xh2a_ipuss_tile_done_work(struct work_struct *work)
{
	uint32_t core_id;
	unsigned long flags;
	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_event *event = NULL;

	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, msi_works[msi_work->msi_id]);

	core_id = msi_work->msi_id % XH2A_IPU_CORE_NUM;

	dev_dbg(ipu_dev->miscdev.this_device,
		"tile_irq: %d, core_id = %d tile done work\n", msi_work->msi_id,
		core_id);

	queue = &ipu_dev->tile_queues[core_id][0];

	event = xh2a_ipu_event_alloc(&ipu_dev->event_pool);
	if (!event) {
		dev_err(ipu_dev->miscdev.this_device,
			"core %u fatal err, event NULL!\n", core_id);
		return;
	}

	event->queue = queue;

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	list_add_tail(&event->node, &ipu_dev->isr_event_list);
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	queue_work(ipu_dev->group_wq, &ipu_dev->ipu_event_work);
}

void xh2a_ipu_group_done_handler(struct xh2a_ipu_device *ipu_dev,
				 struct xh2a_ipu_booter_queue *queue,
				 uint32_t current_rptr,
				 bool update_other_queues)
{
	int old_status, core_id, target_core;
	struct xh2a_ipu_group *group, *group_n;
	uint32_t group_id;
	bool done;

	list_for_each_entry_safe(group, group_n, &queue->group_list,
				 tile_list_node) {
		if (atomic_read(&group->status) == GROUP_DONE)
			continue;

		done = xh2a_ipu_is_group_done(group, current_rptr,
					      queue->last_rptr);

		if (!done) {
			dev_dbg(ipu_dev->miscdev.this_device,
				"group %u %p not done\n", group->id, group);
			break; /* No need to check further groups */
		}
		dev_dbg(ipu_dev->miscdev.this_device, "group %u %p is done\n",
			group->id, group);

		group->load_end = ktime_to_us(ktime_get());

		/* record ipu load from group */
		for (core_id = 0; core_id < group->core_num; core_id++) {
			target_core = group->target[core_id].core_id;
			xh2a_ipu_load_on_done(ipu_dev, target_core,
					      group->load_end);
		}

		/* gets the execution result of the group */
		xh2a_ipu_booter_get_hw_context(ipu_dev, group->exec_result);

		group_id = group->id;

		/* update the read pointer for other tile queues */
		if (update_other_queues)
			xh2a_booter_update_other_queues_rptr(ipu_dev, group);

		if (!list_empty(&group->tile_list_node))
			list_del_init(&group->tile_list_node);

		xh2a_group_host_param_free(ipu_dev, group);

		old_status = atomic_xchg(&group->status, GROUP_DONE);
		if (old_status == GROUP_CANCEL) {
			complete(&group->launch_completion);
			xh2a_ipu_group_destroy(group);
			dev_dbg(ipu_dev->miscdev.this_device,
				"group %u %p cancel done\n", group_id, group);
			continue;
		}

		mutex_lock(&group->mutex);

		if (group->param_type == XH2A_GROUP_PARAM_SPM)
			xh2a_ipu_group_free_spm(group);

		mutex_unlock(&group->mutex);

		complete(&group->launch_completion);
	}
}

void xh2a_ipuss_core_bh_work(struct work_struct *work)
{
	uint32_t core_id;
	unsigned long flags;
	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_event *event = NULL;

	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, core_works[msi_work->msi_id]);

	core_id = msi_work->msi_id % XH2A_IPU_CORE_NUM;

	queue = &ipu_dev->tile_queues[core_id][0];

	if (!xh2a_booter_queue_get_intr_flag(ipu_dev, queue))
		return;

	dev_dbg(ipu_dev->miscdev.this_device,
		"tile_irq: %d, core_id = %d bh work\n", msi_work->msi_id,
		core_id);

	event = xh2a_ipu_event_alloc(&ipu_dev->event_pool);
	if (!event) {
		dev_err(ipu_dev->miscdev.this_device, "fatal err, event "
						      "NULL!\n");
		return;
	}

	event->queue = queue;

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	list_add_tail(&event->node, &ipu_dev->isr_event_list);
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	queue_work(ipu_dev->group_wq, &ipu_dev->ipu_event_work);
}

void xh2a_ipu_event_work(struct work_struct *work)
{
	unsigned long flags;
	uint32_t current_rptr, core_id, mask;
	struct xh2a_ipu_event *event = NULL;
	struct xh2a_ipu_booter_queue *queue = NULL;
	struct xh2a_ipu_device *ipu_dev =
		container_of(work, struct xh2a_ipu_device, ipu_event_work);

	spin_lock_irqsave(&ipu_dev->event_lock, flags);
	while (!list_empty(&ipu_dev->isr_event_list)) {
		event = list_first_entry_or_null(&ipu_dev->isr_event_list,
						 struct xh2a_ipu_event, node);

		list_del_init(&event->node);
		spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

		queue = event->queue;

		mutex_lock(&queue->tile_mutex);
		/* clear kernel done interrupt */
		xh2a_booter_queue_clear_intr(ipu_dev, queue);

		current_rptr = xh2a_booter_queue_get_rptr(ipu_dev, queue);

		queue->last_rptr = queue->rptr;

		xh2a_ipu_group_done_handler(ipu_dev, queue, current_rptr, true);

		/* update queue rptr */
		queue->rptr = current_rptr;
		mutex_unlock(&queue->tile_mutex);

		/* remain space update, wakeup wq to execute group */
		queue_work(ipu_dev->group_wq, &ipu_dev->group_sche_work);

		xh2a_ipu_event_free(&ipu_dev->event_pool, event);
		spin_lock_irqsave(&ipu_dev->event_lock, flags);
	}
	spin_unlock_irqrestore(&ipu_dev->event_lock, flags);

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		mask = (0xF << (core_id * 4));
		if ((ipu_dev->available_tiles_mask & mask) != mask)
			continue;

		queue_work(ipu_dev->group_wq,
			   &ipu_dev->core_works[core_id].work);
	}
}

static void xh2a_ipuss_exception_work(struct work_struct *work)
{
	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, msi_works[msi_work->msi_id]);

	dev_info(ipu_dev->miscdev.this_device, "%s:\n", __func__);
}

static void xh2a_ctcss_phy_work(struct work_struct *work)
{
	struct xh2a_ipu_msi_work *msi_work =
		container_of(work, struct xh2a_ipu_msi_work, work);

	struct xh2a_ipu_device *ipu_dev = container_of(
		msi_work, struct xh2a_ipu_device, msi_works[msi_work->msi_id]);

	dev_info(ipu_dev->miscdev.this_device, "%s:\n", __func__);
}

static work_func_t xh2a_msi_work_table[] = {
	[XH2A_PCIE_MSI_ID_IPUSS_C0T0] = xh2a_ipuss_tile_done_work,
	[XH2A_PCIE_MSI_ID_IPUSS_C1T0] = xh2a_ipuss_tile_done_work,
	[XH2A_PCIE_MSI_ID_IPUSS_C0C1] = xh2a_ipuss_exception_work,
	[XH2A_PCIE_MSI_ID_CTC] = xh2a_ctcss_phy_work,
};

void xh2a_ipu_msi_work_register(struct xh2a_ipu_device *ipu_dev)
{
	int work_count, i;
	struct xh2a_pcie_client *client = NULL;

	work_count = 0;
	client = &ipu_dev->client;

	for (i = 0; i < ARRAY_SIZE(xh2a_msi_work_table); i++) {
		if (xh2a_msi_work_table[i] == NULL)
			continue;

		INIT_WORK(&ipu_dev->msi_works[i].work, xh2a_msi_work_table[i]);
		ipu_dev->msi_works[i].msi_id = i;
		client->work[work_count].msi_id = i;
		client->work[work_count++].msi_work =
			&ipu_dev->msi_works[i].work;
	}

	client->work_num = work_count;

	ipu_dev->msi_work_inited = true;
}

void xh2a_ipu_msi_work_unregister(struct xh2a_ipu_device *ipu_dev)
{
	int i;
	struct xh2a_pcie_client *client = &ipu_dev->client;

	if (!ipu_dev->msi_work_inited)
		return;

	for (i = 0; i < client->work_num; i++)
		cancel_work_sync(client->work[i].msi_work);

	ipu_dev->msi_work_inited = false;
}
