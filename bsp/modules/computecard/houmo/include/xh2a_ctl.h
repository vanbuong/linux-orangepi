// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_CTL_H_
#define _XH2A_CTL_H_

#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/list.h>

/**
 * struct xh2a_ctl_ctx - Global ctl context used by the ctl misc device.
 * @miscdev:   Misc device backing /dev/xh2a_ctl.
 * @lock:      Protects ctl_list and other ctl-wide shared state.
 * @ctl_list: List head of all per-device lock contexts
 *             (struct xh2a_ctl_dev_ctx).
 */
struct xh2a_ctl_ctx {
	struct miscdevice miscdev;

	struct mutex lock;

	struct list_head ctl_list;
};

#endif /*_XH2A_CTL_H_*/
