// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */

#ifndef _XH2A_I2C_H_
#define _XH2A_I2C_H_

#define XH2A_I2C_DEVICE_NAME_LEN 64

/*
 * struct xh2a_i2c_dev - xh2a i2c device
 * @miscdev: misc device
 * @name_buf: name buffer
 * @i2c_mutex: mutex for i2c
 * @client: pcie client struct
 * @xh2a_i2c_base: i2c bus base address
 * @private_data: handle for pcie
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @opened: flag for opened
 * @removed: flag for removed
 * @refcount: reference count
 */
struct xh2a_i2c_dev {
	struct miscdevice miscdev;
	char name_buf[XH2A_I2C_DEVICE_NAME_LEN];
	struct mutex i2c_mutex;
	struct xh2a_pcie_client client;
	uint64_t xh2a_i2c_base;
	void *private_data;
	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;
	atomic_t opened;
	atomic_t removed;
	struct kref refcount;
};

#define DW_IC_TAR	     0x4
#define DW_IC_DATA_CMD	     0x10
#define DW_IC_RAW_INTR_STAT  0x34
#define DW_IC_CLR_TX_ABRT    0x54
#define DW_IC_CLR_STOP_DET   0x60
#define DW_IC_ENABLE	     0x6c
#define DW_IC_STATUS	     0x70
#define DW_IC_TXFLR	     0x74
#define DW_IC_RXFLR	     0x78
#define DW_IC_TX_ABRT_SOURCE 0x80

#define IC_CMD	0x0100
#define IC_STOP 0x0200

#define IC_STOP_DET 0x0200

#define IC_STATUS_SA   0x0040
#define IC_STATUS_MA   0x0020
#define IC_STATUS_RFF  0x0010
#define IC_STATUS_RFNE 0x0008
#define IC_STATUS_TFE  0x0004
#define IC_STATUS_TFNF 0x0002
#define IC_STATUS_ACT  0x0001

#define IC_ENABLE  0x0001U
#define IC_DISABLE 0x0000U

#define I2C0_BASE_ADDR 0x70010000U
#define I2C1_BASE_ADDR 0x70011000U
#define I2C2_BASE_ADDR 0x6088c000U

#define I2C_FIFO_DEPTH 16

#define POLL_TIMEOUT_MS 500
#define POLL_DELAY_MIN	100
#define POLL_DELAY_MAX	200

#endif