// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_fast_memory_internal.h

Abstract:

	This module contains the common declarations shared by driver
	and HAL for fast memory copy module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/
#ifndef _XH2A_FAST_MEMORY_INTERNAL_H_
#define _XH2A_FAST_MEMORY_INTERNAL_H_

/*
 * XH2A_FAST_MEMORY_DEVICE_NAME - unique name of xh2a fast memory
 */
#define XH2A_FAST_MEMORY_DEVICE_NAME	  "xh2a_fast_memory_device"

enum driver_fast_memory_copy_buffer_type {
	DRIVER_FAST_MEMORY_COPY_BUFFER_HOST_TO_DEVICE,
	DRIVER_FAST_MEMORY_COPY_BUFFER_DEVICE_TO_HOST,
};

enum xh2a_fast_memory_ops {
	XH2A_FAST_MEMORY_GET_VADDR,
	XH2A_FAST_MEMORY_ALLOC_BUFFER,
	XH2A_FAST_MEMORY_FREE_BUFFER,
	XH2A_FAST_MEMORY_COPY_BUFFER,
};

struct xh2a_fast_memory_request_arg {
	uint64_t device_paddr;
	uint64_t host_vaddr;
	uint64_t size;
	enum driver_fast_memory_copy_buffer_type type;
};

#define XH2A_FAST_MEMORY_IOC_MAGIC 'Z'
#define IOCTL_XH2A_FAST_MEMORY_GET_VADDR                              \
	_IOWR(XH2A_FAST_MEMORY_IOC_MAGIC, XH2A_FAST_MEMORY_GET_VADDR, \
	      struct xh2a_fast_memory_request_arg)

#define IOCTL_XH2A_FAST_MEMORY_ALLOC_BUFFER                              \
	_IOWR(XH2A_FAST_MEMORY_IOC_MAGIC, XH2A_FAST_MEMORY_ALLOC_BUFFER, \
	      struct xh2a_fast_memory_request_arg)

#define IOCTL_XH2A_FAST_MEMORY_FREE_BUFFER                              \
	_IOWR(XH2A_FAST_MEMORY_IOC_MAGIC, XH2A_FAST_MEMORY_FREE_BUFFER, \
	      struct xh2a_fast_memory_request_arg)

#define IOCTL_XH2A_FAST_MEMORY_COPY_BUFFER                              \
	_IOWR(XH2A_FAST_MEMORY_IOC_MAGIC, XH2A_FAST_MEMORY_COPY_BUFFER, \
	      struct xh2a_fast_memory_request_arg)

#endif // !_XH2A_FAST_MEMORY_INTERNAL_H_
