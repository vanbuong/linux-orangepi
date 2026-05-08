// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_SYS_H_
#define _XH2A_SYS_H_

#include <xh2a_pcie_api.h>
#include <xh2a_system_internal.h>

#define CTC_UNINIT_VAL 0xAAAA

/*
 * struct xh2a_sys_dev - xh2a sys device
 * @miscdev: misc device
 * @name_buf: name buffer
 * @sys_mutex: mutex for read/write/ioctl
 * @device_addr: device addr to read or write
 * @private_data: handle for pcie
 * @ctc_info: ctc link info
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @removed: flag for device removed
 * @refcount: reference count
 */
struct xh2a_sys_dev {
	struct miscdevice miscdev;
	char name_buf[XH2A_SYSTEM_DEVICE_NAME_LEN];
	struct mutex sys_mutex;
	uint64_t device_addr;
	void *private_data;
	struct xh2a_pcie_client client;
	struct xh2a_sys_ctc_info ctc_info;
	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;
	atomic_t removed;
	struct kref refcount;
};

void xh2a_ctc_shutdown(struct xh2a_sys_dev *sys_dev);
int xh2a_sys_ctc_reinit(struct xh2a_sys_dev *sys_dev);

#endif /*_XH2A_SYS_H_*/
