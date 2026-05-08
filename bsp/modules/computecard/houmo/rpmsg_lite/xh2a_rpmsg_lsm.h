// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#ifndef _XH2A_RPMSG_LSM_H
#define _XH2A_RPMSG_LSM_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>

struct xh2a_rpmsg_ept;
struct xh2a_rpmsg_link;

enum xh2a_rpmsg_link_state {
	XH2A_RPMSG_LINK_DOWN = 0,
	XH2A_RPMSG_LINK_PENDING,
	XH2A_RPMSG_LINK_UP,
	XH2A_RPMSG_LINK_ERROR,

	XH2A_RPMSG_LINK_STATE_COUNT,
};

enum xh2a_rpmsg_lsm_flags {
	XH2A_RPMSG_LSM_DOWN_REQ = 0,
	XH2A_RPMSG_LSM_READY,

	XH2A_RPMSG_LSM_FLAG_COUNT,
};

/**
 * struct rpmsg_lsm_msg - xh2a rpmsg link state management message
 * @flags: see xh2a_rpmsg_lsm_flags
 */
struct rpmsg_lsm_msg {
	uint32_t flags;
};

typedef int (*xh2a_rpmsg_link_cb_t)(struct xh2a_rpmsg_link *link);

/**
 * struct xh2a_rpmsg_lsm_ctx - context of xh2a rpmsg link state management
 * @link: pointer to rpmsg link struct
 * @state: link state, see xh2a_rpmsg_link_state
 * @lock: protect message queue
 * @ept: lsm ept to manage link state
 * @queue: queue of lsm message
 * @work: work struct to process lsm msg
 * @link_up_cb: invoked when link up
 * @link_down_cb: invoked when link down
 */
struct xh2a_rpmsg_lsm_ctx {
	struct xh2a_rpmsg_link *link;

	enum xh2a_rpmsg_link_state state;

	struct mutex lock;
	struct xh2a_rpmsg_ept *ept;
	struct list_head queue;
	struct work_struct work;

	xh2a_rpmsg_link_cb_t link_up_cb;
	xh2a_rpmsg_link_cb_t link_down_cb;
};

int xh2a_rpmsg_link_up(struct xh2a_rpmsg_link *link);
int xh2a_rpmsg_link_down(struct xh2a_rpmsg_link *link);

bool xh2a_rpmsg_is_link_up(struct xh2a_rpmsg_link *link);
bool xh2a_rpmsg_is_link_down(struct xh2a_rpmsg_link *link);
bool xh2a_rpmsg_can_send(struct xh2a_rpmsg_link *link);

int xh2a_rpmsg_lsm_init(struct xh2a_rpmsg_link *link,
			xh2a_rpmsg_link_cb_t link_up_cb,
			xh2a_rpmsg_link_cb_t link_down_cb);
void xh2a_rpmsg_lsm_exit(struct xh2a_rpmsg_link *link);

#endif /* _XH2A_RPMSG_LSM_H */
