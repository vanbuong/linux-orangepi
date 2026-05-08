// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef __XH2A_TRANSPORT_PROTOCOL_H__
#define __XH2A_TRANSPORT_PROTOCOL_H__

#include <linux/types.h>
#include <linux/device.h>

/*
 * memory id to save device image
 */
enum xh2a_transport_memory_id {
	XH2A_TRANSPORT_MEMORY_ID_E2,
	XH2A_TRANSPORT_MEMORY_ID_E2_BOTTOM,
	XH2A_TRANSPORT_MEMORY_ID_U7,
	XH2A_TRANSPORT_MEMORY_ID_COUNT,
};

/*
 * host memory status, uninitialized / saved / restored
 */
enum xh2a_transport_memory_status {
	XH2A_TRANSPORT_MEMORY_STATUS_UNINITIALIZED,
	XH2A_TRANSPORT_MEMORY_STATUS_SAVED,
	XH2A_TRANSPORT_MEMORY_STATUS_RESTORED,
};

/*
 * struct transport_memory - transport memory structure
 * @hostmem: pointer of host memory to store device image
 * @status: status of image
 * @memid: memory id of device image
 * @paddr: physical address of device image
 * @size: size of device image
 */
struct transport_memory {
	void *hostmem;
	uint32_t status;
	uint32_t memid;
	uint64_t paddr;
	uint64_t size;
};

struct xh2a_transport_protocol_handle {
	struct work_struct transport_work;
	struct transport_memory mem[XH2A_TRANSPORT_MEMORY_ID_COUNT];
	void *private_data;
};

int xh2a_transport_protocol_init(struct xh2a_transport_protocol_handle *handle,
				 void *private_data);

int xh2a_transport_protocol_deinit(
	struct xh2a_transport_protocol_handle *handle);

#endif
