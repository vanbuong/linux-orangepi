// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_memory_transfer_internal.h

Abstract:

	This module contains the common declarations shared by driver
	and HAL for Memory Transfer module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/
#ifndef _XH2A_MEMORY_TRANSFER_INTERNAL_H_
#define _XH2A_MEMORY_TRANSFER_INTERNAL_H_

/*
 * XH2A_MEMORY_TRANSFER_DEVICE_NAME - unique name of xh2a memory transfer
 */
#define XH2A_MEMORY_TRANSFER_DEVICE_NAME       "xh2a_memory_transfer_device"

 /*
  * driver_memory_transfer_type - memory transfer type, public to user
  * DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE - host to device
  * DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST - device to host
  * DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE - inner device
  * DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER - peer to peer, MRd method
  * DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT - peer to peer, MWr method
  */
enum driver_memory_transfer_type {
	DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE = 0,
	DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST = 1,
	DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE = 2,
};

 /*
  * xh2a_memory_transfer_ops - memory transfer operations
  */
enum xh2a_memory_transfer_ops {
	XH2A_MEMORY_TRANSFER_COPY_BUFFER = 0,
	XH2A_MEMORY_TRANSFER_MEMBAR = 1,
};

enum driver_memory_transfer_type_internal {
	DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER = 3,
	DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT = 4,
};

/*
 * struct xh2a_memory_transfer_request_arg - ioctl arg for transfer
 * @src_addr: start address of source
 * @dst_addr: start address of destination
 * @size: size of destination
 * @type: transfer type/transfer type internal, d2h/h2d/d2d
 */
struct xh2a_memory_transfer_request_arg {
	uint64_t src_addr;
	uint64_t dst_addr;
	uint64_t size;
	int32_t type;
};

/*
 * xh2a_memory_transfer_membar_request_type - request for membar access
 * XH2A_MEMORY_TRANSFER_MEMBAR_GETINFO - get bar information
 * XH2A_MEMORY_TRANSFER_MEMBAR_MAP - set membar mapping
 * XH2A_MEMORY_TRANSFER_MEMBAR_UNMAP - unset membar mapping
 * XH2A_MEMORY_TRANSFER_MEMBAR_LOCK_DEV - lock device
 * XH2A_MEMORY_TRANSFER_MEMBAR_UNLOCK_DEV - unlock device
 */
enum xh2a_memory_transfer_membar_request_type {
	XH2A_MEMORY_TRANSFER_MEMBAR_GETINFO,
	XH2A_MEMORY_TRANSFER_MEMBAR_MAP,
	XH2A_MEMORY_TRANSFER_MEMBAR_UNMAP,
	XH2A_MEMORY_TRANSFER_MEMBAR_LOCK_DEV,
	XH2A_MEMORY_TRANSFER_MEMBAR_UNLOCK_DEV,
};

/*
 * struct xh2a_memory_transfer_membar_arg - ioctl arg for membar lock
 * @paddr: start address in xh2a's address space
 * @bar_addr: bar address in host's address space
 * @bar_size: bar size
 * @type: request type, getinfo / lock / unlock
 */
struct xh2a_memory_transfer_membar_arg {
	uint64_t paddr;
	uint64_t bar_addr;
	uint64_t bar_size;
	enum xh2a_memory_transfer_membar_request_type type;
};

#define XH2A_MEMORY_TRANSFER_IOC_MAGIC 'B'

#define IOCTL_XH2A_MEMORY_TRANSFER_COPY_BUFFER    \
	_IOWR(XH2A_MEMORY_TRANSFER_IOC_MAGIC,   \
	      XH2A_MEMORY_TRANSFER_COPY_BUFFER, \
	      struct xh2a_memory_transfer_request_arg)

#define IOCTL_XH2A_MEMORY_TRANSFER_MEMBAR                                    \
	_IOWR(XH2A_MEMORY_TRANSFER_IOC_MAGIC, XH2A_MEMORY_TRANSFER_MEMBAR, \
	      struct xh2a_memory_transfer_membar_arg)

#endif // !_XH2A_MEMORY_TRANSFER_INTERNAL_H_
