// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_CTL_CTX_H_
#define _XH2A_CTL_CTX_H_

#include <linux/fs.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pid.h>
#include <linux/uaccess.h>
#include <linux/sched.h>

#include "../pcie/xh2a_pcie.h"

#define XH2A_MAX_PCIE_CARDS 64

#define XH2A_LOCK_F_NONBLOCK (1u << 0)

/**
 * struct xh2a_ctl_dev_ctx - Per-device lock context managed by the ctl.
 * @dev_id:     Logical device identifier, typically the PCIe device minor.
 * @pcie_dev:   Pointer to the corresponding PCIe device driver context.
 * @lock:       Protects owner_tgid, refcnt and online fields for this lock.
 * @wq:         Wait queue used by threads blocking on this device lock.
 * @refcnt:     Lock hold count for the current owner_tgid, it is either
 *              0 (unlocked) or 1 (locked).
 * @owner_tgid: Thread group ID of the lock owner; 0 when the lock is free.
 * @kref:       Reference counter for safe lifetime management of this
 *              lock context.
 * @online:     True if the device is present and usable, false if removed
 *              or otherwise unavailable.
 * @node:       List node linking this context into ctl->ctl_list.
 */
struct xh2a_ctl_dev_ctx {
	uint32_t dev_id;
	struct xh2a_pcie_dev *pcie_dev;

	struct mutex lock;
	wait_queue_head_t wq;

	uint32_t refcnt;
	pid_t owner_tgid;
	struct kref kref;

	bool online;

	struct list_head node;
};

/* Lock-side helpers implemented in xh2a_ctl_lock.c */
long xh2a_ctl_lock_dev(struct file *filp, const struct xh2a_ctl_lock_req *req);
long xh2a_ctl_unlock_dev(struct file *filp,
			 const struct xh2a_ctl_lock_req *req);

struct xh2a_ctl_dev_ctx *xh2a_ctl_create_lock(struct xh2a_ctl_ctx *ctl,
					      uint32_t dev_id, void *handle);
void xh2a_ctl_destroy_lock(struct xh2a_ctl_ctx *ctl, uint32_t dev_id);

void xh2a_ctl_ctx_get(struct xh2a_ctl_dev_ctx *ctl_ctx);
void xh2a_ctl_ctx_put(struct xh2a_ctl_dev_ctx *ctl_ctx);

void xh2a_ctl_cleanup_all_locks(struct xh2a_ctl_ctx *ctl);
void xh2a_ctl_release_locks(struct xh2a_ctl_ctx *ctl, pid_t tgid);
#endif /* _XH2A_CTL_CTX_H_ */
