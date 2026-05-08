// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#ifndef _XH2A_RPMSG_LITE_H
#define _XH2A_RPMSG_LITE_H

#include "xh2a_pcie_api.h"

#include "rpmsg_lite.h"

#include "xh2a_rpmsg_core.h"

#include <xh2a_rpmsg_internal.h>

#define XH2A_RPMSG_LITE_LINK_NUM (RL_PLATFORM_HIGHEST_LINK_ID + 1)

struct xh2a_rpmsg_lite_ext;

/**
 * struct xh2a_rpmsg_lite_link - link instance
 * @link: xh2a-rpmsg link object
 * @rl_inst_lock: sync modifications of rl_inst
 * @rl_inst: pointer to rpmsg-lite instance created by rpmsg-lite lib
 * @rldev: see struct xh2a_rpmsg_lite_dev
 */
struct xh2a_rpmsg_lite_link {
	struct xh2a_rpmsg_link link;

	struct mutex rl_inst_lock;
	struct rpmsg_lite_instance *rl_inst;

	struct xh2a_rpmsg_lite_dev *rldev;
};

/**
 * struct xh2a_rpmsg_lite_ept - ept instance
 * @ept: xh2a-rpmsg ept object
 * @rx_cb_adapter: adapt callback prototype in xh2a rpmsg core to rpmsg-lite
 * @rl_ept: pointer to rpmsg-lite ept created by rpmsg-lite lib
 */
struct xh2a_rpmsg_lite_ept {
	struct xh2a_rpmsg_ept ept;

	rl_ept_rx_cb_t rx_cb_adapter;
	struct rpmsg_lite_endpoint *rl_ept;
};

/**
 * struct xh2a_rpmsg_lite_link_info - link information
 * @shm_start_pa: start physical memory of the link
 * @shm_size: share memory size
 */
struct xh2a_rpmsg_lite_link_info {
	phys_addr_t shm_start_pa;
	size_t shm_size;
};

/**
 * struct xh2a_rpmsg_lite_dev - main structure of xh2a-rpmsg-lite
 * @rpmsg_lite_work: bottom half of interrupt
 * @private_data: pointer to pcie drv
 * @rpmsg_lite_work: bottom half of interrupt
 * @link: see struct xh2a_rpmsg_lite_link
 * @rl_platform_context: pointer to platform context in rpmsg-lite lib
 * @block_ioctl_flag: flag to block ioctl
 * @block_ioctl_wq: waitqueue for block ioctl
 */
struct xh2a_rpmsg_lite_dev {
	struct xh2a_pcie_client client;
	void *private_data;

	struct work_struct rpmsg_lite_work;

	struct xh2a_rpmsg_lite_link links[XH2A_RPMSG_LITE_LINK_NUM];

	struct mutex ext_lock;
	struct xh2a_rpmsg_lite_ext *ext;

	void *rl_platform_context;

	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;
	atomic_t dev_initialized;
	atomic_t dev_removed;
	struct kref dev_refcnt;
};

#endif /* _XH2A_RPMSG_LITE_H_ */
