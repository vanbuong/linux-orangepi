// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_system_internal.h

Abstract:

	This module contains the common declarations shared by driver
	and HAL for System module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/
#ifndef _XH2A_SYSTEM_INTERNAL_H_
#define _XH2A_SYSTEM_INTERNAL_H_

/*
 * XH2A_SYSTEM_DEVICE_NAME - unique name of xh2a sys device
 */
#define XH2A_SYSTEM_DEVICE_NAME     "xh2a_system_device"

#define XH2A_SYSTEM_DEVICE_NAME_LEN 64

 /*
  * xh2a_system_rw_type - sys type, read or write
  * readb|w|l|q
  * writeb|w|l|q
  */
enum xh2a_system_rw_type {
	XH2A_SYSTEM_RW_TYPE_READB,
	XH2A_SYSTEM_RW_TYPE_READW,
	XH2A_SYSTEM_RW_TYPE_READL,
	XH2A_SYSTEM_RW_TYPE_READQ,
	XH2A_SYSTEM_RW_TYPE_WRITEB,
	XH2A_SYSTEM_RW_TYPE_WRITEW,
	XH2A_SYSTEM_RW_TYPE_WRITEL,
	XH2A_SYSTEM_RW_TYPE_WRITEQ,
	XH2A_SYSTEM_RW_TYPE_MAX,
};

/*
 * xh2a_system_dumpload_type - sys type, dump or load
 * dump | load
 */
enum xh2a_system_dumpload_type {
	XH2A_SYSTEM_DUMPLOAD_TYPE_DUMP,
	XH2A_SYSTEM_DUMPLOAD_TYPE_LOAD,
	XH2A_SYSTEM_DUMPLOAD_TYPE_MAX,
};

 /*
  * xh2a_sys_ops - sys operations
  */
enum xh2a_sys_ops {
	XH2A_SYS_RW_REQUEST,
	XH2A_SYS_DUMPLOAD_REQUEST,
	XH2A_SYS_FULLCHIP_RESET_REQUEST,
	XH2A_SYS_CTC_GET_REQUEST,
	XH2A_SYS_CTC_SET_REQUEST,
};

/*
 * struct xh2a_sys_rw_request_arg - ioctl arg for rw request
 * @addr: addr to read or write
 * @data: data read out or write in
 * @type: rw type, readb|w|l|q or writeb|w|l|q
 */
struct xh2a_sys_rw_request_arg {
	uint64_t addr;
	uint64_t data;
	enum xh2a_system_rw_type type;
};

/*
 * struct xh2a_sys_dumpload_request_arg - ioctl arg for dump/load request
 * @device_addr: device addr to dump or load
 * @host_addr: host addr to dump or load
 * @size: size to dump or load
 * @type: dump or load
 */
struct xh2a_sys_dumpload_request_arg {
	uint64_t device_addr;
	uint64_t host_addr;
	uint32_t size;
	enum xh2a_system_dumpload_type type;
};

/*
 * struct xh2a_sys_fullchip_reset_request_arg - ioctl arg for fullchip reset
 * @reserved: reserved data
 */
struct xh2a_sys_fullchip_reset_request_arg {
	uint64_t reserved;
};


/*
 * struct xh2a_sys_ctc_info - xh2a sys ctc info
 * @chip_id: ctc chip id
 * @group_id: ctc group id
 * @group_size: size of ctc group
 * @group_num: total number of ctc groups in the system
 * @is_single: 1: single chip, 0: multi-chip
 */
struct xh2a_sys_ctc_info {
	int chip_id;
	int group_id;
	int group_size;
	int group_num;
	int is_single;
};


#define XH2A_SYS_IOC_MAGIC 'G'

#define IOCTL_XH2A_SYS_RW                              \
	_IOWR(XH2A_SYS_IOC_MAGIC, XH2A_SYS_RW_REQUEST, \
	      struct xh2a_sys_rw_request_arg)

#define IOCTL_XH2A_SYS_DUMPLOAD                              \
	_IOWR(XH2A_SYS_IOC_MAGIC, XH2A_SYS_DUMPLOAD_REQUEST, \
	      struct xh2a_sys_dumpload_request_arg)

#define IOCTL_XH2A_SYS_FULLCHIP_RESET                              \
	_IOWR(XH2A_SYS_IOC_MAGIC, XH2A_SYS_FULLCHIP_RESET_REQUEST, \
	      struct xh2a_sys_fullchip_reset_request_arg)

#define IOCTL_XH2A_SYS_CTC_GET                              \
	_IOWR(XH2A_SYS_IOC_MAGIC, XH2A_SYS_CTC_GET_REQUEST, \
	      struct xh2a_sys_ctc_info)

#define IOCTL_XH2A_SYS_CTC_SET                              \
	_IOWR(XH2A_SYS_IOC_MAGIC, XH2A_SYS_CTC_SET_REQUEST, \
	      struct xh2a_sys_ctc_info)


#endif // !_XH2A_SYSTEM_INTERNAL_H_
