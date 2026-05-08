// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_ipu_internal.h

Abstract:

	This module contains the declarations shared by driver
	and HAL for IPU module for internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/

#ifndef XH2A_IPU_INTERNAL_H_
#define XH2A_IPU_INTERNAL_H_

#define XH2A_IPU_DEVICE_NAME       "xh2a_ipu"

#define XH2A_GROUP_RESULT_BUF_SZ 1
#define INVALID_GROUP_ID -1
typedef int32_t group_id_t;

/** struct ipu_kernel_launch_data - Kernel launch info£º
 * @kernel_addr: kernel code physical addr
 * @kernel_size: kernel code size
 * @param_phy_addr: param physical addr for core0
 * @param_size: param size
 * @param_type: bit0: DDR, bit1: SPM
 * @core_num: the total number of cores need to be used in the group
 * @tile_num: the number of tiles need to be used each core
 * @ilm_mode: Indicates instruction mode: 0:ilm 1:cache
 * @timeout_us: timeout in us of each kernel execution
 */
struct ipu_kernel_launch_data {
	uint64_t kernel_addr;
	uint32_t kernel_size;
	uint64_t param_phy_addr;
	uint32_t param_size;
	uint32_t param_type;
	uint32_t core_num;
	uint32_t tile_num;
	uint32_t ilm_mode;
	uint32_t timeout_us;
};

/**
 * struct xh2a_group_ioctl_cmd - Structure for IOCTL commands for groups.
 * @group_id: Identifier for the specific group being operated on.
 * @cmd: Union containing command-specific data.
 *       - kld: Kernel launch data used for initiating kernel operations.
 *       - result: Buffer to store the results of the operation,
 *       - `timeout_us`: timeout in us from execute api to trigger booter.
 *                 with a fixed size defined by XH2A_GROUP_RESULT_BUF_SZ.
 */
struct xh2a_group_ioctl_cmd {
	int group_id;
	uint32_t timeout_us;
	struct {
		struct ipu_kernel_launch_data kld;
		uint32_t result[XH2A_GROUP_RESULT_BUF_SZ];
	} cmd;
};

#define XH2A_IPU_IOCTL_MAGIC 'C'

#define IOCTL_XH2A_IPU_CREATE_GROUP \
	_IOR(XH2A_IPU_IOCTL_MAGIC, 0, group_id_t)
#define IOCTL_XH2A_IPU_DESTROY_GROUP \
	_IOW(XH2A_IPU_IOCTL_MAGIC, 1, group_id_t)
#define IOCTL_XH2A_IPU_LAUNCH_KERNEL \
	_IOW(XH2A_IPU_IOCTL_MAGIC, 2, struct xh2a_group_ioctl_cmd)
#define IOCTL_XH2A_IPU_EXECUTE_GROUP \
	_IOWR(XH2A_IPU_IOCTL_MAGIC, 3, struct xh2a_group_ioctl_cmd)
#define IOCTL_XH2A_IPU_DEVICE_RESET \
    _IOW(XH2A_IPU_IOCTL_MAGIC, 4, int32_t)
#define IOCTL_XH2A_IPU_GET_LOADAVG	\
    _IOWR(XH2A_IPU_IOCTL_MAGIC, 5, uint32_t)

#endif // !XH2A_IPU_INTERNAL_H_