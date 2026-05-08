// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_spm_api.h>
#include "xh2a_memory_allocator.h"
#include "allocator_algorithm.h"

#ifndef XH2A_NAME_CONNECTOR
#define XH2A_NAME_CONNECTOR "_"
#endif

int xh2a_memory_allocator_kernel_alloc_spm(void *handle, int spmid, int size,
					   uint64_t *start)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_allocator_dev *allocator_dev;

	if (spmid == 0)
		xh2a_pcie_get_client(
			handle, &client,
			XH2A_MEMORY_ALLOCATOR_DEVICE_NAME XH2A_NAME_CONNECTOR
				XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_NAME);
	else if (spmid == 1)
		xh2a_pcie_get_client(
			handle, &client,
			XH2A_MEMORY_ALLOCATOR_DEVICE_NAME XH2A_NAME_CONNECTOR
				XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_NAME);
	else {
		pr_err("%s: Invalid spm id %d\n", __func__, spmid);
		return -EINVAL;
	}

	if (client == NULL) {
		pr_err("%s: client not found\n", __func__);
		return -EINVAL;
	}

	allocator_dev = client->client_data;
	ret = mutex_lock_interruptible(&allocator_dev->allocator_mutex);

	if (ret != 0) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -ERESTARTSYS;
	}

	*start = allocator_core_alloc(&allocator_dev->allocator_mempool, size);

	if (!(*start)) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: allocator_core_alloc failed\n", __func__);
		ret = -ENOMEM;
	}

	mutex_unlock(&allocator_dev->allocator_mutex);

	return ret;
}
EXPORT_SYMBOL(xh2a_memory_allocator_kernel_alloc_spm);

int xh2a_memory_allocator_kernel_free_spm(void *handle, int spmid, int size,
					  uint64_t start)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_allocator_dev *allocator_dev;

	if (spmid == 0)
		xh2a_pcie_get_client(
			handle, &client,
			XH2A_MEMORY_ALLOCATOR_DEVICE_NAME XH2A_NAME_CONNECTOR
				XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_NAME);
	else if (spmid == 1)
		xh2a_pcie_get_client(
			handle, &client,
			XH2A_MEMORY_ALLOCATOR_DEVICE_NAME XH2A_NAME_CONNECTOR
				XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_NAME);
	else {
		pr_err("%s: Invalid spm id %d\n", __func__, spmid);
		return -EINVAL;
	}

	if (client == NULL) {
		pr_err("%s: client not found\n", __func__);
		return -EINVAL;
	}

	allocator_dev = client->client_data;
	ret = mutex_lock_interruptible(&allocator_dev->allocator_mutex);

	if (ret != 0) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -ERESTARTSYS;
	}

	allocator_core_free(&allocator_dev->allocator_mempool, start, size);

	mutex_unlock(&allocator_dev->allocator_mutex);

	return 0;
}
EXPORT_SYMBOL(xh2a_memory_allocator_kernel_free_spm);
