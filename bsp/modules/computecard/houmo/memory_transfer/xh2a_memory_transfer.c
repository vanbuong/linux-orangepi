// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_efuse.h>
#include "xh2a_memory_transfer.h"

#define ADDRESS_2GB 0x80000000ULL

struct address_section_parameter {
	uint64_t start_addr;
	uint64_t total_size;
};

static struct address_section_parameter valid_address_tbl[] = {
	{ XH2A_MEMORY_TRANSFER_DDR_START_ADDR,
	  XH2A_MEMORY_TRANSFER_DDR_TOTAL_SIZE },
	{ XH2A_MEMORY_TRANSFER_SPM0_START_ADDR,
	  XH2A_MEMORY_TRANSFER_SPM0_TOTAL_SIZE },
	{ XH2A_MEMORY_TRANSFER_SPM1_START_ADDR,
	  XH2A_MEMORY_TRANSFER_SPM1_TOTAL_SIZE },
	{ XH2A_MEMORY_TRANSFER_TIM1_START_ADDR,
	  XH2A_MEMORY_TRANSFER_TIM1_TOTAL_SIZE },
};

#define VALID_ADDRESS_TABLE_SIZE \
	((sizeof(valid_address_tbl) / sizeof(struct address_section_parameter)))

static inline bool is_address_valid(uint64_t addr, uint64_t size)
{
	int i;

	for (i = 0; i < VALID_ADDRESS_TABLE_SIZE; i++) {
		if (addr >= valid_address_tbl[i].start_addr &&
		    addr + size <= valid_address_tbl[i].start_addr +
					   valid_address_tbl[i].total_size)
			return true;
	}

	pr_err("address 0x%llx size 0x%llx is not valid device address.\n",
	       addr, size);

	return false;
}

static void xh2a_memory_transfer_device_safe_release(struct kref *kref)
{
	struct xh2a_memory_transfer_dev *transfer_dev =
		container_of(kref, struct xh2a_memory_transfer_dev, dev_refcnt);
	kfree(transfer_dev);
}

static int xh2a_memory_transfer_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct xh2a_memory_transfer_dev *transfer_dev =
		container_of(miscdev, struct xh2a_memory_transfer_dev, miscdev);
	struct xh2a_memory_transfer_file_handle *fh = NULL;

	if (!transfer_dev) {
		pr_err("%s: memory transfer not initialized\n", __func__);
		return -EINVAL;
	}

	if (atomic_read(&transfer_dev->dev_initialized) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (atomic_read(&transfer_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&transfer_dev->dev_refcnt) == 0) {
		pr_err("%s: device is removing\n", __func__);
		return -ENODEV;
	}

	fh = kzalloc(sizeof(struct xh2a_memory_transfer_file_handle),
		     GFP_KERNEL);

	if (!fh) {
		dev_err(miscdev->this_device, "%s: kzalloc failed\n", __func__);
		kref_put(&transfer_dev->dev_refcnt,
			 xh2a_memory_transfer_device_safe_release);
		return -ENOMEM;
	}

	fh->transfer = transfer_dev;
	INIT_LIST_HEAD(&fh->node);
	mutex_init(&fh->file_handle_mutex);

	if (atomic_read(&transfer_dev->dev_removed)) {
		kref_put(&transfer_dev->dev_refcnt,
			 xh2a_memory_transfer_device_safe_release);
		kfree(fh);
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&transfer_dev->transfer_mutex);
	list_add_tail(&fh->node, &transfer_dev->file_handle_list);
	mutex_unlock(&transfer_dev->transfer_mutex);

	filp->private_data = fh;

	return 0;
}

static int xh2a_memory_transfer_release(struct inode *inode, struct file *filp)
{
	struct xh2a_memory_transfer_dev *transfer_dev = NULL;
	struct xh2a_memory_transfer_file_handle *fh = NULL;

	fh = filp->private_data;
	filp->private_data = NULL;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	transfer_dev = fh->transfer;

	if (!transfer_dev) {
		pr_err("%s: transfer is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&transfer_dev->transfer_mutex);
	if (fh->file_handle_lockmap) {
		xh2a_pcie_membar_unmap(transfer_dev->private_data);
	}

	if (transfer_dev->lock_dev && (transfer_dev->lock_dev_owner == fh)) {
		transfer_dev->lock_dev = 0;
		transfer_dev->lock_dev_owner = NULL;
		wake_up_interruptible(&transfer_dev->lock_dev_wq);
	}

	list_del_init(&fh->node);
	mutex_unlock(&transfer_dev->transfer_mutex);
	kfree(fh);
	kref_put(&transfer_dev->dev_refcnt,
		 xh2a_memory_transfer_device_safe_release);
	return 0;
}

static int xh2a_memory_transfer_ioctl_core_transfer(
	struct xh2a_memory_transfer_dev *transfer_dev, unsigned long arg)
{
	int ret;
	struct xh2a_memory_transfer_request_arg ioc_request;
	uint64_t done_size = 0;
	uint64_t xfer_size = 0;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(transfer_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	if (ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER) {
		if (!is_address_valid(ioc_request.dst_addr, ioc_request.size)) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: device dst address/size invalid\n",
				__func__);
			return -EINVAL;
		}
	}

	if (ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE ||
	    ioc_request.type == DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT) {
		if (!is_address_valid(ioc_request.src_addr, ioc_request.size)) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: device src address/size invalid\n",
				__func__);
			return -EINVAL;
		}
	}

	while (done_size != ioc_request.size) {
		if (ioc_request.size - done_size < ADDRESS_2GB)
			xfer_size = ioc_request.size - done_size;
		else
			xfer_size = ADDRESS_2GB;

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_HOST_TO_DEVICE)
			ret = xh2a_pcie_dma_write_mem_userspace(
				transfer_dev->private_data,
				ioc_request.dst_addr + done_size,
				(void __user *)(ioc_request.src_addr +
						done_size),
				xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_DEVICE_TO_HOST)
			ret = xh2a_pcie_dma_read_mem_userspace(
				transfer_dev->private_data,
				ioc_request.src_addr + done_size,
				(void __user *)(ioc_request.dst_addr +
						done_size),
				xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_INNER_DEVICE)
			ret = xh2a_memory_transfer_sysdma_memcpy(
				&transfer_dev->sysdma,
				ioc_request.src_addr + done_size,
				ioc_request.dst_addr + done_size, xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER)
			ret = xh2a_pcie_dma_mrd_direct(
				transfer_dev->private_data,
				ioc_request.src_addr + done_size,
				ioc_request.dst_addr + done_size, xfer_size);

		if (ioc_request.type ==
		    DRIVER_MEMORY_TRANSFER_TYPE_PEER_TO_PEER_ALT)
			ret = xh2a_pcie_dma_mwr_direct(
				transfer_dev->private_data,
				ioc_request.src_addr + done_size,
				ioc_request.dst_addr + done_size, xfer_size);

		if (ret == -ENODEV) {
			pr_err("%s: device is removed\n", __func__);
			return ret;
		}
		if (ret != 0) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: xfer[0x%llx->0x%llx][0x%llx] failed %d\n",
				__func__, ioc_request.src_addr,
				ioc_request.dst_addr, ioc_request.size, ret);
			return ret;
		}

		done_size += xfer_size;
	};

	return 0;
}

static int xh2a_memory_transfer_ioctl_core_membar(
	struct xh2a_memory_transfer_dev *transfer_dev,
	struct xh2a_memory_transfer_file_handle *transfer_fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_transfer_membar_arg ioc_request;

	ret = mutex_lock_interruptible(&transfer_fh->file_handle_mutex);

	if (ret != 0) {
		dev_err(transfer_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -ERESTARTSYS;
	}

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(transfer_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		goto out;
	}

	if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_GETINFO) {
		ret = xh2a_pcie_membar_getinfo(transfer_dev->private_data,
					       &ioc_request.bar_addr,
					       &ioc_request.bar_size);

		if (ret != 0) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: xh2a_pcie_get_membar_info failed\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		ret = copy_to_user((void __user *)arg, &ioc_request,
				   sizeof(ioc_request));

		if (ret) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: copy_to_user failed\n", __func__);
			ret = -EBUSY;
			goto out;
		}
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_MAP) {
		if (!transfer_dev->lock_dev ||
		    (transfer_dev->lock_dev_owner != transfer_fh)) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: device not locked with this file\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		if (transfer_fh->file_handle_lockmap) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: file handle already locked membar\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		if (!is_address_valid(ioc_request.paddr, 4)) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: map address invalid\n", __func__);
			ret = -EINVAL;
			goto out;
		}

		/* ioc.type MEMBAR_MAP is required between MEMBAR_LOCK_DEV and
		 * MEMBAR_UNLOCK_DEV. It is safe to map membar without lock */
		ret = xh2a_pcie_membar_map_no_lock(transfer_dev->private_data,
						   ioc_request.paddr);

		if (ret != 0) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: xh2a_pcie_membar_map_no_lock failed\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		transfer_fh->file_handle_lockmap = 1;
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_UNMAP) {
		if (!transfer_dev->lock_dev ||
		    (transfer_dev->lock_dev_owner != transfer_fh)) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: device not locked with this file\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		if (!transfer_fh->file_handle_lockmap) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: file handle already unlocked membar\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		/* ioc.type MEMBAR_UNMAP is required between MEMBAR_LOCK_DEV and
		 * MEMBAR_UNLOCK_DEV. It is safe to map membar without lock */
		ret = xh2a_pcie_membar_unmap_no_lock(
			transfer_dev->private_data);

		if (ret != 0) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: xh2a_pcie_membar_unmap_no_lock failed\n",
				__func__);
			ret = -EBUSY;
			goto out;
		}

		transfer_fh->file_handle_lockmap = 0;
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_LOCK_DEV) {
		ret = mutex_lock_interruptible(&transfer_dev->transfer_mutex);

		if (ret != 0) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			ret = -ERESTARTSYS;
			goto out;
		}

		while (transfer_dev->lock_dev != 0) {
			mutex_unlock(&transfer_dev->transfer_mutex);

			ret = wait_event_interruptible(
				transfer_dev->lock_dev_wq,
				transfer_dev->lock_dev == 0);

			if (ret != 0) {
				dev_err(transfer_dev->miscdev.this_device,
					"%s: wait_event_interruptible failed\n",
					__func__);
				ret = -ERESTARTSYS;
				goto out;
			}

			ret = mutex_lock_interruptible(
				&transfer_dev->transfer_mutex);

			if (ret != 0) {
				dev_err(transfer_dev->miscdev.this_device,
					"%s: mutex_lock_interruptible failed\n",
					__func__);
				ret = -ERESTARTSYS;
				goto out;
			}
		}

		transfer_dev->lock_dev = 1;
		transfer_dev->lock_dev_owner = transfer_fh;
		mutex_unlock(&transfer_dev->transfer_mutex);
	} else if (ioc_request.type == XH2A_MEMORY_TRANSFER_MEMBAR_UNLOCK_DEV) {
		ret = mutex_lock_interruptible(&transfer_dev->transfer_mutex);

		if (ret != 0) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			ret = -ERESTARTSYS;
			goto out;
		}

		if (!transfer_dev->lock_dev ||
		    (transfer_dev->lock_dev_owner != transfer_fh)) {
			dev_err(transfer_dev->miscdev.this_device,
				"%s: device already unlocked or not owner\n",
				__func__);
			mutex_unlock(&transfer_dev->transfer_mutex);
			ret = -EBUSY;
			goto out;
		}
		transfer_dev->lock_dev = 0;
		transfer_dev->lock_dev_owner = NULL;
		wake_up_interruptible(&transfer_dev->lock_dev_wq);
		mutex_unlock(&transfer_dev->transfer_mutex);
	} else {
		dev_err(transfer_dev->miscdev.this_device,
			"%s: invalid request type\n", __func__);
		ret = -EINVAL;
		goto out;
	}

out:
	mutex_unlock(&transfer_fh->file_handle_mutex);
	return ret;
}

static long xh2a_memory_transfer_ioctl(struct file *filp, unsigned int cmd,
				       unsigned long arg)
{
	int ret = -EINVAL;
	struct xh2a_memory_transfer_dev *transfer_dev = NULL;
	struct xh2a_memory_transfer_file_handle *fh = NULL;
	struct miscdevice *miscdev = NULL;

	fh = filp->private_data;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	transfer_dev = fh->transfer;

	if (!transfer_dev) {
		pr_err("%s: transfer is NULL\n", __func__);
		return -EINVAL;
	}

	miscdev = &transfer_dev->miscdev;

	if (atomic_read(&transfer_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (atomic_read(&transfer_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: device try enter sleep, wait...\n", __func__);

		ret = wait_event_interruptible(
			transfer_dev->block_ioctl_wq,
			atomic_read(&transfer_dev->block_ioctl_flag) == 0);

		if (ret < 0) {
			dev_err(miscdev->this_device,
				"%s: wait_event_interruptible failed %d\n",
				__func__, ret);
			/* s2idle keeps devices' DDR, retry blocked ioctl;
			 * otherwise drop it. */
			if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
				return -EAGAIN;
			else
				return -EINTR;
		}
	}

	pm_runtime_get_sync(miscdev->parent);

	switch (cmd) {
	case IOCTL_XH2A_MEMORY_TRANSFER_COPY_BUFFER:
		ret = xh2a_memory_transfer_ioctl_core_transfer(transfer_dev,
							       arg);
		break;

	case IOCTL_XH2A_MEMORY_TRANSFER_MEMBAR:
		ret = xh2a_memory_transfer_ioctl_core_membar(transfer_dev, fh,
							     arg);
		break;

	default:
		dev_err(miscdev->this_device, "%s: unknown cmd 0x%x\n",
			__func__, cmd);
		break;
	}

	if ((ret != 0) && atomic_read(&transfer_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: ioctl failed in sleep process. need retry\n",
			__func__);
		/* s2idle keeps devices' DDR, retry blocked ioctl; otherwise
		 * drop it. */
		if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
			ret = -EAGAIN;
		else
			ret = -EINTR;
	}
	pm_runtime_mark_last_busy(miscdev->parent);
	pm_runtime_put_sync(miscdev->parent);
	return ret;
}

static const struct file_operations xh2a_memory_transfer_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_memory_transfer_open,
	.release = xh2a_memory_transfer_release,
	.unlocked_ioctl = xh2a_memory_transfer_ioctl,
};

/*
 * xh2a_memory_transfer_pm_notifier_prepare()
 *     - before pm prepare the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	if (rollback) {
		if (atomic_dec_and_test(&transfer_dev->block_ioctl_flag))
			wake_up_interruptible(&transfer_dev->block_ioctl_wq);
	} else {
		atomic_inc(&transfer_dev->block_ioctl_flag);
	}

	return 0;
}

/*
 * xh2a_memory_transfer_pm_notifier_complete()
 *     - after pm complete the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	(void)transfer_dev;

	return 0;
}

/*
 * xh2a_memory_transfer_pm_prepare()
 *     - pm prepare the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	xh2a_memory_transfer_sysdma_pm_prepare(&transfer_dev->sysdma);

	return 0;
}

/*
 * xh2a_memory_transfer_pm_complete()
 *     - pm complete the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	xh2a_memory_transfer_sysdma_pm_complete(&transfer_dev->sysdma);

	if (atomic_dec_and_test(&transfer_dev->block_ioctl_flag))
		wake_up_interruptible(&transfer_dev->block_ioctl_wq);

	return 0;
}

static int xh2a_memory_transfer_update_ddr_size(void *handle, uint64_t *size)
{
	int ret;
	uint32_t ddr_chip_quantity, ddr_chip_capacity;

	ret = xh2a_pcie_get_efuse_data(handle, XH2A_EFUSE_SUBTYPE_DDR_0_ROW,
				       XH2A_EFUSE_SUBTYPE_DDR_0_BIT,
				       XH2A_EFUSE_SUBTYPE_DDR_0_LENGTH,
				       &ddr_chip_quantity);

	if (ret != 0) {
		pr_err("%s: get efuse data failed\n", __func__);
		*size = 0;
		return ret;
	}

	if (ddr_chip_quantity > 6) {
		pr_err("%s: get invalid ddr chip quantity %d\n", __func__,
		       ddr_chip_quantity);
		*size = 0;
		return -1;
	}

	ret = xh2a_pcie_get_efuse_data(handle, XH2A_EFUSE_SUBTYPE_DDR_1_ROW,
				       XH2A_EFUSE_SUBTYPE_DDR_1_BIT,
				       XH2A_EFUSE_SUBTYPE_DDR_1_LENGTH,
				       &ddr_chip_capacity);

	if (ret != 0) {
		pr_err("%s: get efuse data failed\n", __func__);
		*size = 0;
		return ret;
	}

	if (ddr_chip_capacity > 16) {
		pr_err("%s: get invalid ddr chip capacity %d\n", __func__,
		       ddr_chip_capacity);
		*size = 0;
		return -1;
	}

	*size = ddr_chip_quantity * ddr_chip_capacity * 0x40000000ULL;
	return 0;
}

/*
 * xh2a_memory_transfer_probe()
 *     - probe function for memory_transfer device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_probe(void *handle)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev = NULL;
	uint64_t size;

	pr_debug("%s\n", __func__);

	transfer_dev =
		kzalloc(sizeof(struct xh2a_memory_transfer_dev), GFP_KERNEL);

	if (transfer_dev == NULL) {
		pr_err("%s: alloc transfer_dev failed\n", __func__);
		return -ENOMEM;
	}

	client = &transfer_dev->client;

	transfer_dev->private_data = handle;

	mutex_init(&transfer_dev->transfer_mutex);

	transfer_dev->lock_dev = 0;
	transfer_dev->lock_dev_owner = NULL;
	init_waitqueue_head(&transfer_dev->lock_dev_wq);

	INIT_LIST_HEAD(&transfer_dev->file_handle_list);

	atomic_set(&transfer_dev->dev_initialized, 0);
	atomic_set(&transfer_dev->dev_removed, 0);
	kref_init(&transfer_dev->dev_refcnt);

	atomic_set(&transfer_dev->block_ioctl_flag, 0);
	init_waitqueue_head(&transfer_dev->block_ioctl_wq);

	memset(transfer_dev->name_buf, 0, XH2A_MEMORY_TRANSFER_DEVICE_NAME_LEN);
	snprintf(transfer_dev->name_buf, XH2A_MEMORY_TRANSFER_DEVICE_NAME_LEN,
		 XH2A_MEMORY_TRANSFER_DEVICE_NAME "%d",
		 xh2a_pcie_device_index(handle));

	transfer_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	transfer_dev->miscdev.name = transfer_dev->name_buf;
	transfer_dev->miscdev.fops = &xh2a_memory_transfer_fops;
	transfer_dev->miscdev.mode = 0666;
	transfer_dev->miscdev.parent = xh2a_pcie_device_ptr(handle);

	ret = misc_register(&transfer_dev->miscdev);

	if (ret) {
		pr_err("%s: misc_register failed\n", __func__);
		goto err_misc_register;
	}

	ret = xh2a_memory_transfer_sysdma_init(
		&transfer_dev->sysdma, transfer_dev->miscdev.this_device,
		transfer_dev->private_data);

	if (ret != 0) {
		dev_err(transfer_dev->miscdev.this_device,
			"%s: sysdma init failed\n", __func__);
		goto err_sysdma_init;
	}

	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	client->work[0].msi_id = XH2A_PCIE_MSI_ID_CPUSS;
	client->work[0].msi_work = &transfer_dev->sysdma.sysdma_work;
	client->work_num = 1;

	client->client_data = transfer_dev;
	client->private_data = handle;

	client->prepare_cb = xh2a_memory_transfer_pm_prepare;
	client->complete_cb = xh2a_memory_transfer_pm_complete;
	client->notifier_prepare_cb = xh2a_memory_transfer_pm_notifier_prepare;
	client->notifier_complete_cb =
		xh2a_memory_transfer_pm_notifier_complete;

	ret = xh2a_pcie_register_client(handle, client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_misc_register;
	}

	ret = xh2a_memory_transfer_update_ddr_size(handle, &size);
	if ((ret == 0) && (size != 0)) {
		size -= XH2A_DEVICE_SYSTEM_SIZE;
		valid_address_tbl[0].total_size = size;
	}

	atomic_set(&transfer_dev->dev_initialized, 1);

	return 0;

err_sysdma_init:
	misc_deregister(&transfer_dev->miscdev);

err_misc_register:
	kfree(transfer_dev);
	return ret;
}

/*
 * xh2a_memory_transfer_remove()
 *     - remove the memory_transfer device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_transfer_remove(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_transfer_dev *transfer_dev;
	struct xh2a_memory_transfer_file_handle *fh_pos, *fh_n;

	pr_debug("%s\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_MEMORY_TRANSFER_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transfer_dev = client->client_data;

	atomic_set(&transfer_dev->dev_removed, 1);

	xh2a_memory_transfer_sysdma_deinit(&transfer_dev->sysdma);

	mutex_lock(&transfer_dev->transfer_mutex);
	list_for_each_entry_safe(fh_pos, fh_n, &transfer_dev->file_handle_list,
				 node) {
		list_del_init(&fh_pos->node);
	}
	mutex_unlock(&transfer_dev->transfer_mutex);

	misc_deregister(&transfer_dev->miscdev);

	xh2a_pcie_unregister_client(handle, client);

	transfer_dev->private_data = NULL;
	kref_put(&transfer_dev->dev_refcnt,
		 xh2a_memory_transfer_device_safe_release);
	return 0;
}

/*
 * xh2a_memory_transfer_notifier_call()
 *     - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_memory_transfer_notifier_call(struct notifier_block *nb,
					      unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_memory_transfer_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_memory_transfer_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_memory_transfer_notifier_block = {
	.notifier_call = xh2a_memory_transfer_notifier_call,
};

/*
 * xh2a_memory_transfer_register_driver()
 *     - register memory_transfer driver
 */
int __init xh2a_memory_transfer_register_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_memory_transfer_notifier_block);
	return 0;
}

/*
 * xh2a_memory_transfer_unregister_driver()
 *     - unregister memory_transfer driver
 */
void __exit xh2a_memory_transfer_unregister_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_unregister_notifier_chain(
		&xh2a_memory_transfer_notifier_block);
}

MODULE_LICENSE("GPL");
