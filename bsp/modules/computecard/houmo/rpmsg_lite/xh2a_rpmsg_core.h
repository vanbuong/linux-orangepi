// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#ifndef _XH2A_RPMSG_CORE_H
#define _XH2A_RPMSG_CORE_H

#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>

#include "xh2a_rpmsg_lsm.h"
#include "xh2a_rpmsg_ioctl.h"

struct xh2a_rpmsg_ept;
struct xh2a_rpmsg_link;

typedef int (*xh2a_rpmsg_rx_cb_t)(struct xh2a_rpmsg_ept *ept, void *buf,
				  int len, void *priv, uint32_t addr);

#define XH2A_RPMSG_LINK_NAME_LEN 64

/**
 * struct xh2a_rpmsg_ept_ops - ept operations, all required
 * @destroy_ept: destroy an existing ept
 * @send_kern: send a message across to the remote processor, use memcpy() to
 *	       copy message from kernel space (eg. rpmsg-tty)
 * @send_user: send a message across to the remote processor, use
 *	       copy_from_user() to copy message from user space
 * @get_mtu: get maximum transmission buffer size for sending message
 */
struct xh2a_rpmsg_ept_ops {
	void (*destroy_ept)(struct xh2a_rpmsg_ept *ept);
	int (*send_kern)(struct xh2a_rpmsg_ept *ept, uint32_t dst, void *data,
			 int len);
	int (*send_user)(struct xh2a_rpmsg_ept *ept, uint32_t dst,
			 void __user *data, int len);
	size_t (*get_mtu)(struct xh2a_rpmsg_ept *ept);
};

/**
 * struct xh2a_rpmsg_link_ops - link operations
 * @create_ept: create a ept on the link
 */
struct xh2a_rpmsg_link_ops {
	struct xh2a_rpmsg_ept *(*create_ept)(
		struct xh2a_rpmsg_link *link, xh2a_rpmsg_rx_cb_t cb, void *priv,
		struct xh2a_rpmsg_ept_info *ept_info);
	int (*increase_refcount)(struct xh2a_rpmsg_link *link);
	int (*decrease_refcount)(struct xh2a_rpmsg_link *link);
	int (*device_removed)(struct xh2a_rpmsg_link *link);
	int (*device_initialized)(struct xh2a_rpmsg_link *link);
};

/**
 * struct xh2a_rpmsg_ept - xh2a rpmsg endpoint
 * @link: link to which ept belongs
 * @name: ept name
 * @addr: local rpmsg address
 * @cb: rx callback handler
 * @priv: private data for rx callback handler
 * @annouance: if set, announce the creation/removal of this ept
 * @ops: ept operastions, see above
 */
struct xh2a_rpmsg_ept {
	struct xh2a_rpmsg_link *link;

	char name[XH2A_RPMSG_EPT_NAME_LENGTH];
	uint32_t addr;
	/* no need lock here, there's a lock inside rpmsg-lite lib */
	xh2a_rpmsg_rx_cb_t cb;
	void *priv;

	bool announce;

	struct xh2a_rpmsg_ept_ops *ops;
};

/**
 * struct xh2a_rpmsg_link - a channel for epts communication between two ends
 * @link_id: link id
 * @endpoints_lock: idr of local endpoints
 * @endpoints_lock: lock of the endpoints set
 * @name: link name
 * @miscdev: misc device
 * @tty_driver: tty driver, to support rpmsg-tty
 * @tty_port: tty port, support only one port
 * @lsm_ctx: link state manage context
 * @ops: link ops, see above
 */
struct xh2a_rpmsg_link {
	uint32_t link_id;

	struct mutex endpoints_lock;
	struct idr endpoints;

	char name[XH2A_RPMSG_LINK_NAME_LEN];
	struct miscdevice miscdev;
	struct list_head ept_ctx_list;
	struct mutex ept_ctx_lock;

	struct xh2a_rpmsg_lsm_ctx lsm_ctx;

	struct xh2a_rpmsg_link_ops *ops;
};

struct xh2a_rpmsg_ept *
xh2a_rpmsg_create_ept(struct xh2a_rpmsg_link *link, xh2a_rpmsg_rx_cb_t cb,
		      void *priv, struct xh2a_rpmsg_ept_info *ept_info);

void xh2a_rpmsg_destroy_ept(struct xh2a_rpmsg_ept *ept);

int xh2a_rpmsg_send_kern(struct xh2a_rpmsg_ept *ept, uint32_t dst, void *data,
			 int len);

int xh2a_rpmsg_send_user(struct xh2a_rpmsg_ept *ept, uint32_t dst,
			 void __user *data, int len);

static inline size_t xh2a_rpmsg_get_mtu(struct xh2a_rpmsg_ept *ept)
{
	return ept->ops->get_mtu(ept);
}

int xh2a_rpmsg_link_register(struct xh2a_rpmsg_link *link, int parent_index,
			     struct device *parent_dev,
			     xh2a_rpmsg_link_cb_t link_up_cb,
			     xh2a_rpmsg_link_cb_t link_down_cb);
void xh2a_rpmsg_link_unregister(struct xh2a_rpmsg_link *link);
#endif /* _XH2A_RPMSG_CORE_H_ */
