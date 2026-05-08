// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_rpmsg_internal.h

Abstract:

	This module contains the common declarations shared by driver
	and HAL for RPMsg module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/
#ifndef XH2A_RPMSG_INTERNAL_H_
#define XH2A_RPMSG_INTERNAL_H_

#define XH2A_RPMSG_DEVICE_NAME "xh2a_rpmsg_device"

#define XH2A_RPMSG_ADDRESS_ANY     0xFFFFFFFF
#define XH2A_RPMSG_EPT_NAME_LENGTH 64
#define XH2A_RPMSG_BUFFER_MAX_SIZE 496

enum xh2a_rpmsg_ioctl_cmd {
	XH2A_RPMSG_CREATE_EPT = 0x0,
	XH2A_RPMSG_DESTROY_EPT,
	XH2A_RPMSG_SEND,
	XH2A_RPMSG_RECV,
};

/**
 * struct xh2a_rpmsg_ept_info - ept information for creating
 * @addr: local address. set to XH2A_RPMSG_ADDRESS_ANY if not care
 * @name: name of service
 */
struct xh2a_rpmsg_ept_info {
	uint32_t addr;
	char name[XH2A_RPMSG_EPT_NAME_LENGTH];
};

/**
 * struct xh2a_rpmsg_xmit_info - transmit infomation for sending of receiving
 * @buf: send or recv buffer
 * @size: buffer size
 * @addr: src address of message for recveiving, dst address for sending
 * @timeout: time to wait when the resource is not available,
 *	     only used for receiving
 */
struct xh2a_rpmsg_xmit_info {
	void* buf;
	size_t size;
	uint32_t addr;
	uint32_t timeout;
};

#define XH2A_RPMSG_IOC_MAGIC 'F'

#define IOCTL_XH2A_RPMSG_CREATE_EPT                          \
	_IOWR(XH2A_RPMSG_IOC_MAGIC, XH2A_RPMSG_CREATE_EPT, \
	      struct xh2a_rpmsg_ept_info)

#define IOCTL_XH2A_RPMSG_DESTROY_EPT \
	_IO(XH2A_RPMSG_IOC_MAGIC, XH2A_RPMSG_DESTROY_EPT)

#define IOCTL_XH2A_RPMSG_SEND \
	_IOW(XH2A_RPMSG_IOC_MAGIC, XH2A_RPMSG_SEND, struct xh2a_rpmsg_xmit_info)

#define IOCTL_XH2A_RPMSG_RECV                          \
	_IOWR(XH2A_RPMSG_IOC_MAGIC, XH2A_RPMSG_RECV, \
	      struct xh2a_rpmsg_xmit_info)

#endif // !XH2A_RPMSG_INTERNAL_H_
