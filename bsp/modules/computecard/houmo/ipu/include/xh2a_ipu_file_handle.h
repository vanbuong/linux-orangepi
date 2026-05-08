// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_FILE_HANDLE_H_
#define _XH2A_IPU_FILE_HANDLE_H_

#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/types.h>
#include <linux/kref.h>

/**
 * struct xh2a_ipu_file_handle - Represents a file handle
 *     associated with an IPU device.
 * @ipu_dev: Pointer to the IPU device associated with this file handle.
 * @node: List node for linking this file handle in a list.
 * @group_idr: IDR for managing and allocating unique group IDs.
 * @file_mutex: Flie mutex on this file handle.
 * @min_id: Minimum ID for the group IDR.
 */
struct xh2a_ipu_device;
struct xh2a_ipu_file_handle {
	struct xh2a_ipu_device *ipu_dev;
	struct list_head node;
	struct idr group_idr;
	struct mutex file_mutex;
	int min_id;
	atomic_t drop_flag;
};

/* file handle api */
struct xh2a_ipu_file_handle *
xh2a_ipu_file_handle_create(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_file_handle_destroy(struct xh2a_ipu_file_handle *handle);

struct xh2a_group_result *
xh2a_ipu_file_handle_get_group_result(struct xh2a_ipu_file_handle *handle);

void xh2a_ipu_file_handle_stop_group(struct xh2a_ipu_file_handle *file_handle);

#endif /* _XH2A_IPU_FILE_HANDLE_H_ */