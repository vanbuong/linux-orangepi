// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_DEVICE_H_
#define _XH2A_IPU_DEVICE_H_

#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <xh2a_pcie_api.h>

#include "xh2a_ipu_hw.h"
#include "xh2a_ipu_config.h"
#include "xh2a_ipu_policy.h"
#include "xh2a_ipu_mempool.h"
#include "xh2a_ipu_load.h"

/**
 * struct xh2a_ipu_msi_work - Structure for representing an IPU MSI work item.
 * @work: Work structure of pcie.
 * @msi_id: Identifier for the specific MSI associated with the work item.
 */
struct xh2a_ipu_msi_work {
	struct work_struct work;
	int msi_id;
};

/**
 * struct xh2a_ipu_crg_regs - Structure for representing ipu crg regs.
 * @pll_int: PLL integer reg, offset 0x8.
 * @pll_frac: PLL frac reg, offset 0xC.
 * @clk_select: Clock select reg, offset 0x20.
 * @clk_div: Clocl divider reg, offset 0x90.
 */
struct xh2a_ipu_crg_regs {
	uint32_t pll_int;
	uint32_t pll_frac;
	uint32_t clk_select;
	uint32_t clk_div;
};

/**
 * struct xh2a_ipu_device - Represents an IPU device.
 * @name: Name of the IPU device.
 * @miscdev: Misc device for registering the IPU as a misc device.
 * @dev_mutex: Mutex for synchronizing access to the device.
 * @fh_mutex: Mutex for synchronizing access to the file_handle.
 * @load_mutex: Mutex for synchronizing access to the ipu load.
 * @event_lock: Spinlock for ipu event list.
 * @ops: booter queue operations for ipu device.
 * @file_handle_list: List of file handles associated with the device.
 * @isr_event_list: List of isr events associated with the device.
 * @client: client for pcie handle.
 *     usage and preventing premature release.
 * @msi_works: The work struct array of msi.
 * @core_works: The work struct array of msi, for post irq work.
 * @private_data: Handle for pcie.
 * @event_pool: Memory pool for events.
 * @tile_queues: Tile queues for group execution across cores and tiles.
 * Organized as a 2D array where:
 *  - First dimension corresponds to cores (XH2A_IPU_CORE_NUM).
 *  - Second dimension corresponds to tiles per core (XH2A_TILE_NUM_PER_CORE).
 * @policy: The policy for tile queue allocation.
 * @group_sche_work: The work struct for group scheduling.
 * @ipu_event_work: The work struct for ipu event.
 * @ipu_load_work: The work struct for ipu load.
 * @group_wq: The workqueue for group scheduling and test.
 * @load_dev: IPU device load statistics info.
 * @load_timer: Kernel timer used to trigger periodic load computation.
 * @lock: Spinlock for synchronizing access to shared fields
 *        across timer and ISR contexts.
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @dev_initialized: flag for device initialized
 * @dev_removed: flag for device removed
 * @dev_refcnt: reference count for device
 */
struct xh2a_ipu_device {
	char name[XH2A_IPU_NAME_LEN];
	struct miscdevice miscdev;
	struct mutex dev_mutex;
	struct mutex fh_mutex;
	struct mutex dump_mutex;
	spinlock_t event_lock;

	const struct xh2a_booter_ops *ops;

	struct list_head file_handle_list;
	struct list_head isr_event_list;

	struct xh2a_pcie_client client;
	bool msi_work_inited;
	struct xh2a_ipu_msi_work msi_works[XH2A_IPU_MSI_WORK_NUM];
	struct xh2a_ipu_msi_work core_works[XH2A_IPU_CORE_NUM];
	void *private_data;

	struct xh2a_event_pool event_pool;
	struct xh2a_ipu_booter_queue tile_queues[XH2A_IPU_CORE_NUM]
						[XH2A_TILE_NUM_PER_CORE];

	struct xh2a_ipu_policy *policy;
	bool group_work_inited;
	int last_core;
	struct work_struct group_sche_work;
	struct work_struct ipu_event_work;
	struct work_struct ipu_load_work;
	struct workqueue_struct *group_wq;

	struct xh2a_ipu_load_core load_core[XH2A_IPU_CORE_NUM];
	struct timer_list load_timer;
	spinlock_t load_lock;
	atomic_t load_stop;
	atomic_t is_reboot; /* reboot or shutdown */
	struct notifier_block load_shutdown_nb;

	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;

	uint32_t available_cores_num;
	uint32_t available_tiles_mask;
	void *spm_snapshot_buf[XH2A_IPU_CORE_NUM];

	atomic_t dev_initialized;
	atomic_t dev_removed;
	struct kref dev_refcnt;
};

struct xh2a_ipu_device *xh2a_ipu_device_create(void *handle);
void xh2a_ipu_device_destroy(struct xh2a_ipu_device *ipu_dev);

#endif /* _XH2A_IPU_DEVICE_H_ */
