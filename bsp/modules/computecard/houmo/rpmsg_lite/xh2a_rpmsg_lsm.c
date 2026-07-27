// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#include <linux/slab.h>
#include <linux/delay.h>

#include "xh2a_rpmsg_core.h"

#define XH2A_RPMSG_LSM_NAME	"xh2a_rpmsg_lsm"
#define XH2A_RPMSG_LSM_EPT_ADDR 42

#define XH2A_RPMSG_LSM_SEND_RETRY_COUNT 3
#define XH2A_RPMSG_LSM_SEND_RETRY_DELAY 10 /* ms */

static const char *lsm_flag_name[XH2A_RPMSG_LSM_FLAG_COUNT] = {
	"DOWN_REQ",
	"READY",
};

struct xh2a_rpmsg_lsm_msg {
	struct list_head node;
	struct rpmsg_lsm_msg msg;
};

static int xh2a_rpmsg_lsm_send(struct xh2a_rpmsg_lsm_ctx *lsm_ctx,
			       enum xh2a_rpmsg_lsm_flags flags)
{
	int ret;
	struct rpmsg_lsm_msg msg;
	struct xh2a_rpmsg_ept *lsm_ept = lsm_ctx->ept;

	msg.flags = flags;

	ret = xh2a_rpmsg_send_kern(lsm_ept, XH2A_RPMSG_LSM_EPT_ADDR, &msg,
				   sizeof(struct rpmsg_lsm_msg));
	return ret;
}

static void xh2a_rpmsg_lsm_handle_msg(struct xh2a_rpmsg_lsm_ctx *lsm_ctx,
				      struct rpmsg_lsm_msg *msg)
{
	uint32_t flags = msg->flags;

	(void)lsm_ctx;

	if (flags >= XH2A_RPMSG_LSM_FLAG_COUNT) {
		pr_err("Received unexpected lsm msg, flags: 0x%x\n", flags);
		return;
	}

	pr_info("Received lsm msg, flags: %s\n", lsm_flag_name[flags]);
}

static void xh2a_rpmsg_lsm_work_handler(struct work_struct *work)
{
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx;
	struct xh2a_rpmsg_lsm_msg *msg, *n;

	lsm_ctx = container_of(work, struct xh2a_rpmsg_lsm_ctx, work);

	mutex_lock(&lsm_ctx->lock);

	list_for_each_entry_safe(msg, n, &lsm_ctx->queue, node) {
		list_del(&msg->node);
		xh2a_rpmsg_lsm_handle_msg(lsm_ctx, &msg->msg);
		kfree(msg);
	}

	mutex_unlock(&lsm_ctx->lock);
}

static int xh2a_rpmsg_lsm_cb(struct xh2a_rpmsg_ept *ept, void *buf, int len,
			     void *priv, uint32_t addr)
{
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = priv;
	struct xh2a_rpmsg_lsm_msg *lsm_msg;

	if (len != sizeof(struct rpmsg_lsm_msg)) {
		pr_err("Unexpected lsm msg length %d\n", len);
		return -EINVAL;
	}

	if (addr != XH2A_RPMSG_LSM_EPT_ADDR) {
		pr_err("Unexpected lsm msg from 0x%x\n", addr);
		return -EINVAL;
	}

	lsm_msg = kzalloc(sizeof(struct xh2a_rpmsg_lsm_msg), GFP_KERNEL);
	if (!lsm_msg) {
		pr_err("Failed to allocate memory for lsm msg\n");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&lsm_msg->node);
	memcpy(&lsm_msg->msg, buf, len);

	mutex_lock(&lsm_ctx->lock);
	list_add_tail(&lsm_msg->node, &lsm_ctx->queue);
	mutex_unlock(&lsm_ctx->lock);

	schedule_work(&lsm_ctx->work);

	return 0;
}

bool xh2a_rpmsg_is_link_up(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = &link->lsm_ctx;

	return lsm_ctx->state == XH2A_RPMSG_LINK_UP;
}

bool xh2a_rpmsg_is_link_down(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = &link->lsm_ctx;

	return lsm_ctx->state == XH2A_RPMSG_LINK_DOWN;
}

bool xh2a_rpmsg_can_send(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = &link->lsm_ctx;

	/* For the Host, messages can be sent when the link state is PENDING */
	return lsm_ctx->state == XH2A_RPMSG_LINK_PENDING ||
	       lsm_ctx->state == XH2A_RPMSG_LINK_UP;
}

int xh2a_rpmsg_link_up(struct xh2a_rpmsg_link *link)
{
	int ret = 0;
	int retry;
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = &link->lsm_ctx;

	link->lsm_ctx.state = XH2A_RPMSG_LINK_PENDING;

	for (retry = 0; retry < XH2A_RPMSG_LSM_SEND_RETRY_COUNT; retry++) {
		ret = xh2a_rpmsg_lsm_send(lsm_ctx, XH2A_RPMSG_LSM_READY);
		if (!ret)
			break;

		mdelay(XH2A_RPMSG_LSM_SEND_RETRY_DELAY);
	}

	if (ret) {
		pr_err("Failed to send READY message\n");
		return ret;
	}

	if (lsm_ctx->link_up_cb)
		ret = lsm_ctx->link_up_cb(lsm_ctx->link);

	if (!ret)
		link->lsm_ctx.state = XH2A_RPMSG_LINK_UP;

	return ret;
}

int xh2a_rpmsg_link_down(struct xh2a_rpmsg_link *link)
{
	int ret = 0;
	int retry;
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = &link->lsm_ctx;

	for (retry = 0; retry < XH2A_RPMSG_LSM_SEND_RETRY_COUNT; retry++) {
		ret = xh2a_rpmsg_lsm_send(lsm_ctx, XH2A_RPMSG_LSM_DOWN_REQ);
		if (!ret)
			break;

		mdelay(XH2A_RPMSG_LSM_SEND_RETRY_DELAY);
	}

	if (ret)
		pr_err("Failed to send DOWN_REQ message\n");

	if (lsm_ctx->link_down_cb)
		ret = lsm_ctx->link_down_cb(lsm_ctx->link);

	lsm_ctx->state = XH2A_RPMSG_LINK_DOWN;

	return ret;
}

int xh2a_rpmsg_lsm_init(struct xh2a_rpmsg_link *link,
			xh2a_rpmsg_link_cb_t link_up_cb,
			xh2a_rpmsg_link_cb_t link_down_cb)
{
	struct xh2a_rpmsg_ept *ept;
	struct xh2a_rpmsg_ept_info ept_info;
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = &link->lsm_ctx;

	sprintf(ept_info.name, "%s%u", XH2A_RPMSG_LSM_NAME, link->link_id);
	ept_info.addr = XH2A_RPMSG_LSM_EPT_ADDR;

	ept = xh2a_rpmsg_create_ept(link, xh2a_rpmsg_lsm_cb, lsm_ctx,
				    &ept_info);
	if (!ept) {
		pr_err("Create lsm ept failed\n");
		return -EFAULT;
	}

	lsm_ctx->ept = ept;
	lsm_ctx->link = link;
	lsm_ctx->state = XH2A_RPMSG_LINK_DOWN;
	mutex_init(&lsm_ctx->lock);
	INIT_LIST_HEAD(&lsm_ctx->queue);
	INIT_WORK(&lsm_ctx->work, xh2a_rpmsg_lsm_work_handler);
	lsm_ctx->link_up_cb = link_up_cb;
	lsm_ctx->link_down_cb = link_down_cb;

	return 0;
}

void xh2a_rpmsg_lsm_exit(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lsm_msg *msg, *n;
	struct xh2a_rpmsg_lsm_ctx *lsm_ctx = &link->lsm_ctx;

	xh2a_rpmsg_destroy_ept(lsm_ctx->ept);

	mutex_lock(&lsm_ctx->lock);

	list_for_each_entry_safe(msg, n, &lsm_ctx->queue, node) {
		list_del(&msg->node);
		kfree(msg);
	}

	mutex_unlock(&lsm_ctx->lock);

	cancel_work_sync(&lsm_ctx->work);

	lsm_ctx->state = XH2A_RPMSG_LINK_DOWN;
}
