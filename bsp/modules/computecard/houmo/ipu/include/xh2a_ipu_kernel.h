// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_KERNEL_H_
#define _XH2A_IPU_KERNEL_H_

#include <linux/list.h>
#include <linux/types.h>
#include "xh2a_ipu_config.h"
#include <xh2a_ipu_internal.h>

/**
 * struct xh2a_ipu_kernel - Represents a kernel within an IPU group.
 * @node: List node for linking this kernel into the group's kernel list.
 * @group: Pointer to the task group to which this kernel belongs.
 * @kld: Data structure containing the launch parameters and
 *    configuration for the kernel execution.
 * @param_spm_addr: Physical address for the kernel's parameter in SPM memory.
 * @param_spm_offset: Offset for the kernel's parameter in SPM memory.
 * @host_param: Pointer to the host memory for the kernel's parameters.
 */
struct xh2a_ipu_group;
struct xh2a_ipu_kernel {
	struct list_head node;
	struct xh2a_ipu_group *group;
	struct ipu_kernel_launch_data kld;
	uint64_t param_spm_addr[XH2A_IPU_CORE_NUM];
	uint64_t param_spm_offset;
	uint32_t *host_param;
};

/* kernel api */
struct xh2a_ipu_kernel *xh2a_ipu_kernel_create(struct xh2a_ipu_group *group,
					       struct ipu_kernel_launch_data *kld);
void xh2a_ipu_kernel_destroy(struct xh2a_ipu_kernel *kernel);
void xh2a_kernel_host_param_free(struct xh2a_ipu_kernel *kernel);
int xh2a_kernel_transfer_param2spm(struct xh2a_ipu_kernel *kernel,
				   uint32_t core_id);

#endif /* _XH2A_IPU_KERNEL_H_ */