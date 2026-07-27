// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2026 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_ctl_internal.h

Abstract:

	This module contains the declarations shared by driver
	and HAL for IPU module for internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/

#ifndef XH2A_CTL_INTERNAL_H_
#define XH2A_CTL_INTERNAL_H_

#define XH2A_CTL_DEVICE_NAME "xh2a_ctl"

/**
 * struct xh2a_ctl_lock_req - Lock/unlock request for xh2a ctl.
 * @devmask:     Target device bitmask, where each bit represents a PCIE card
 * minor index.
 * @flags:       Flags for lock operation (e.g., non-blocking).
 * @timeout_ms:  Timeout in milliseconds; 0 means infinite wait in
 *               blocking mode.
 * @reserved:    Reserved for future extensions; must be zero.
 */
struct xh2a_ctl_lock_req {
	uint64_t devmask;
	uint32_t flags;
	uint32_t timeout_ms;
	uint32_t reserved;
};

#define XH2A_CTL_IOCTL_MAGIC 'c'

#define IOCTL_XH2A_CTL_LOCK_DEV \
	_IOW(XH2A_CTL_IOCTL_MAGIC, 0xA0, struct xh2a_ctl_lock_req)
#define IOCTL_XH2A_CTL_UNLOCK_DEV \
	_IOW(XH2A_CTL_IOCTL_MAGIC, 0xA1, struct xh2a_ctl_lock_req)

#endif /*  !XH2A_CTL_INTERNAL_H_ */