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
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_efuse.h>
#include "xh2a_fast_memory.h"

struct address_section_parameter {
	uint64_t start_addr;
	uint64_t total_size;
};

static const struct address_section_parameter valid_address_tbl[] = {
	{ XH2A_FAST_MEMORY_DDR_START_ADDR, XH2A_FAST_MEMORY_DDR_TOTAL_SIZE },
	{ XH2A_FAST_MEMORY_SPM0_START_ADDR, XH2A_FAST_MEMORY_SPM0_TOTAL_SIZE },
	{ XH2A_FAST_MEMORY_SPM1_START_ADDR, XH2A_FAST_MEMORY_SPM1_TOTAL_SIZE },
	{ XH2A_FAST_MEMORY_TIM1_START_ADDR, XH2A_FAST_MEMORY_TIM1_TOTAL_SIZE },
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

static void xh2a_fast_memory_device_safe_release(struct kref *kref)
{
	struct xh2a_fast_memory_dev *fast_memory_dev =
		container_of(kref, struct xh2a_fast_memory_dev, dev_refcnt);

	allocator_core_destroy(&fast_memory_dev->mempool);
	kfree(fast_memory_dev);
}

static int xh2a_fast_memory_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct xh2a_fast_memory_dev *fast_memory_dev =
		container_of(miscdev, struct xh2a_fast_memory_dev, miscdev);
	struct xh2a_fast_memory_file_handle *fh = NULL;

	if (!fast_memory_dev) {
		dev_err(miscdev->this_device,
			"%s: fast memory not initialized\n", __func__);
		return -EINVAL;
	}

	if (atomic_read(&fast_memory_dev->dev_initialized) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (atomic_read(&fast_memory_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&fast_memory_dev->dev_refcnt) == 0) {
		pr_err("%s: device is removing...\n", __func__);
		return -ENODEV;
	}

	fh = kzalloc(sizeof(struct xh2a_fast_memory_file_handle), GFP_KERNEL);

	if (!fh) {
		dev_err(miscdev->this_device, "%s: kzalloc failed\n", __func__);
		kref_put(&fast_memory_dev->dev_refcnt,
			 xh2a_fast_memory_device_safe_release);
		return -ENOMEM;
	}

	fh->fast_memory = fast_memory_dev;
	fh->pid = current->pid;
	INIT_LIST_HEAD(&fh->node);
	INIT_LIST_HEAD(&fh->buffer_object_list);

	if (atomic_read(&fast_memory_dev->dev_removed)) {
		kref_put(&fast_memory_dev->dev_refcnt,
			 xh2a_fast_memory_device_safe_release);
		kfree(fh);
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&fast_memory_dev->fast_memory_mutex);
	list_add_tail(&fh->node, &fast_memory_dev->file_handle_list);
	mutex_unlock(&fast_memory_dev->fast_memory_mutex);

	filp->private_data = fh;

	return 0;
}

static int xh2a_fast_memory_release(struct inode *inode, struct file *filp)
{
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;
	struct xh2a_fast_memory_file_handle *fh = NULL;
	struct xh2a_fast_memory_buffer_object *pos, *n;

	fh = filp->private_data;
	filp->private_data = NULL;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	fast_memory_dev = fh->fast_memory;

	if (!fast_memory_dev) {
		pr_err("%s: fast memory is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&fast_memory_dev->fast_memory_mutex);
	list_for_each_entry_safe(pos, n, &fh->buffer_object_list, node) {
		allocator_core_free(&fast_memory_dev->mempool, pos->start,
				    pos->size);
		list_del(&pos->node);
		kfree(pos);
	}
	list_del_init(&fh->node);
	mutex_unlock(&fast_memory_dev->fast_memory_mutex);

	kfree(fh);

	kref_put(&fast_memory_dev->dev_refcnt,
		 xh2a_fast_memory_device_safe_release);

	return 0;
}

static int
xh2a_fast_memory_ioctl_get_vaddr(struct xh2a_fast_memory_dev *fast_memory_dev,
				 struct xh2a_fast_memory_file_handle *fh,
				 unsigned long arg)
{
	int ret = -EINVAL;
	struct xh2a_fast_memory_request_arg ioc_request;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	ioc_request.host_vaddr = fh->user_vaddr;
	ioc_request.size = fast_memory_dev->dma_size;

	ret = copy_to_user((void __user *)arg, &ioc_request,
			   sizeof(ioc_request));

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: copy_to_user failed\n", __func__);

		return -EFAULT;
	}

	return ret;
}

static int xh2a_fast_memory_ioctl_alloc_buffer(
	struct xh2a_fast_memory_dev *fast_memory_dev,
	struct xh2a_fast_memory_file_handle *fh, unsigned long arg)
{
	int ret = -EINVAL;
	struct xh2a_fast_memory_request_arg ioc_request;
	struct xh2a_fast_memory_buffer_object *buffer_object = NULL;
	uint64_t allocated_paddr;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	if (ioc_request.size > fast_memory_dev->dma_size) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: size %llu is too large\n", __func__,
			ioc_request.size);
		return -EINVAL;
	}

	buffer_object = kzalloc(sizeof(struct xh2a_fast_memory_buffer_object),
				GFP_KERNEL);

	if (!buffer_object) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}

	ret = mutex_lock_interruptible(&fast_memory_dev->fast_memory_mutex);

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		kfree(buffer_object);
		return -ERESTARTSYS;
	}

	allocated_paddr = allocator_core_alloc(&fast_memory_dev->mempool,
					       ioc_request.size);

	if (allocated_paddr == 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: allocator_core_alloc failed\n", __func__);
		kfree(buffer_object);
		mutex_unlock(&fast_memory_dev->fast_memory_mutex);
		return -ENOMEM;
	}

	buffer_object->start = allocated_paddr;
	buffer_object->size = ioc_request.size;
	buffer_object->pid = current->pid;

	ioc_request.host_vaddr = buffer_object->start -
				 XH2A_FAST_MEMORY_POOL_START + fh->user_vaddr;
	ret = copy_to_user((void __user *)arg, &ioc_request,
			   sizeof(ioc_request));

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: copy_to_user failed\n", __func__);
		allocator_core_free(&fast_memory_dev->mempool, allocated_paddr,
				    ioc_request.size);
		kfree(buffer_object);

		mutex_unlock(&fast_memory_dev->fast_memory_mutex);
		return -EFAULT;
	}

	list_add_tail(&buffer_object->node, &fh->buffer_object_list);

	mutex_unlock(&fast_memory_dev->fast_memory_mutex);

	return ret;
}

static int
xh2a_fast_memory_ioctl_free_buffer(struct xh2a_fast_memory_dev *fast_memory_dev,
				   struct xh2a_fast_memory_file_handle *fh,
				   unsigned long arg)
{
	int ret = -EINVAL;
	struct xh2a_fast_memory_request_arg ioc_request;
	struct xh2a_fast_memory_buffer_object *pos, *n;
	int free_cnt = 0;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	ret = mutex_lock_interruptible(&fast_memory_dev->fast_memory_mutex);

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -ERESTARTSYS;
	}

	list_for_each_entry_safe(pos, n, &fh->buffer_object_list, node) {
		if (pos->start == ioc_request.host_vaddr - fh->user_vaddr +
					  XH2A_FAST_MEMORY_POOL_START) {
			list_del(&pos->node);
			allocator_core_free(&fast_memory_dev->mempool,
					    pos->start, pos->size);
			kfree(pos);
			free_cnt++;
			break;
		}
	}

	if (free_cnt != 1) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: free 0x%llx %d times\n", __func__,
			ioc_request.host_vaddr, free_cnt);
		ret = -EINVAL;
	}

	mutex_unlock(&fast_memory_dev->fast_memory_mutex);

	return ret;
}

static int
xh2a_fast_memory_ioctl_copy_buffer(struct xh2a_fast_memory_dev *fast_memory_dev,
				   struct xh2a_fast_memory_file_handle *fh,
				   unsigned long arg)
{
	int ret = -EINVAL;
	struct xh2a_fast_memory_request_arg ioc_request;
	uint64_t host_offset;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	host_offset = ioc_request.host_vaddr - fh->user_vaddr;
	if (host_offset > fast_memory_dev->dma_size) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: host_vaddr 0x%llx [0x%llx] is out of range\n",
			__func__, ioc_request.host_vaddr, ioc_request.size);
		return -EINVAL;
	}

	if (!is_address_valid(ioc_request.device_paddr, ioc_request.size)) {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: device_paddr 0x%llx [0x%llx] is out of range\n",
			__func__, ioc_request.device_paddr, ioc_request.size);
		return -EINVAL;
	}

	if (ioc_request.type == DRIVER_FAST_MEMORY_COPY_BUFFER_HOST_TO_DEVICE) {
		dma_sync_single_for_device(fast_memory_dev->miscdev.parent,
					   host_offset +
						   fast_memory_dev->dma_paddr,
					   ioc_request.size, DMA_TO_DEVICE);
		ret = xh2a_pcie_dma_mrd_direct(
			fast_memory_dev->private_data,
			host_offset + fast_memory_dev->dma_paddr,
			ioc_request.device_paddr, ioc_request.size);

		if (ret == -ENODEV) {
			pr_err("%s: device is removed\n", __func__);
			return ret;
		}
		if (ret != 0) {
			dev_err(fast_memory_dev->miscdev.this_device,
				"%s: mrd failed\n", __func__);
			return ret;
		}
	} else if (ioc_request.type ==
		   DRIVER_FAST_MEMORY_COPY_BUFFER_DEVICE_TO_HOST) {
		ret = xh2a_pcie_dma_mwr_direct(
			fast_memory_dev->private_data, ioc_request.device_paddr,
			host_offset + fast_memory_dev->dma_paddr,
			ioc_request.size);

		if (ret == -ENODEV) {
			pr_err("%s: device is removed\n", __func__);
			return ret;
		}
		if (ret != 0) {
			dev_err(fast_memory_dev->miscdev.this_device,
				"%s: mwr failed\n", __func__);
			return ret;
		}
		dma_sync_single_for_cpu(fast_memory_dev->miscdev.parent,
					host_offset +
						fast_memory_dev->dma_paddr,
					ioc_request.size, DMA_FROM_DEVICE);
	} else {
		dev_err(fast_memory_dev->miscdev.this_device,
			"%s: unknown type %d\n", __func__, ioc_request.type);
	}

	return ret;
}

static long xh2a_fast_memory_ioctl(struct file *filp, unsigned int cmd,
				   unsigned long arg)
{
	int ret = -EINVAL;
	struct miscdevice *miscdev = NULL;
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;
	struct xh2a_fast_memory_file_handle *fh = NULL;

	fh = filp->private_data;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	fast_memory_dev = fh->fast_memory;

	if (!fast_memory_dev) {
		pr_err("%s: fast_memory is NULL\n", __func__);
		return -EINVAL;
	}

	miscdev = &fast_memory_dev->miscdev;

	if (atomic_read(&fast_memory_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (atomic_read(&fast_memory_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: device try enter sleep, wait...\n", __func__);
		ret = wait_event_interruptible(
			fast_memory_dev->block_ioctl_wq,
			atomic_read(&fast_memory_dev->block_ioctl_flag) == 0);
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

	switch (cmd) {
	case IOCTL_XH2A_FAST_MEMORY_GET_VADDR:
		ret = xh2a_fast_memory_ioctl_get_vaddr(fast_memory_dev, fh,
						       arg);
		break;
	case IOCTL_XH2A_FAST_MEMORY_ALLOC_BUFFER:
		ret = xh2a_fast_memory_ioctl_alloc_buffer(fast_memory_dev, fh,
							  arg);
		break;
	case IOCTL_XH2A_FAST_MEMORY_FREE_BUFFER:
		ret = xh2a_fast_memory_ioctl_free_buffer(fast_memory_dev, fh,
							 arg);
		break;
	case IOCTL_XH2A_FAST_MEMORY_COPY_BUFFER:
		pm_runtime_get_sync(miscdev->parent);

		ret = xh2a_fast_memory_ioctl_copy_buffer(fast_memory_dev, fh,
							 arg);
		pm_runtime_mark_last_busy(miscdev->parent);
		pm_runtime_put_sync(miscdev->parent);
		break;
	default:
		dev_err(miscdev->this_device, "%s: unknown cmd 0x%x\n",
			__func__, cmd);
		break;
	}

	if ((ret != 0) && atomic_read(&fast_memory_dev->block_ioctl_flag)) {
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

	return ret;
}

static int xh2a_fast_memory_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;
	struct xh2a_fast_memory_file_handle *fh = NULL;

	fh = filp->private_data;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	fast_memory_dev = fh->fast_memory;

	if (!fast_memory_dev) {
		pr_err("%s: fast_memory is NULL\n", __func__);
		return -EINVAL;
	}

	if (vma->vm_end - vma->vm_start > fast_memory_dev->dma_size)
		return -EINVAL;

	if (remap_pfn_range(vma, vma->vm_start,
			    fast_memory_dev->dma_paddr >> PAGE_SHIFT,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		return -EAGAIN;
	}

	fh->user_vaddr = vma->vm_start;

	return 0;
}

static const struct file_operations xh2a_fast_memory_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_fast_memory_open,
	.release = xh2a_fast_memory_release,
	.unlocked_ioctl = xh2a_fast_memory_ioctl,
	.mmap = xh2a_fast_memory_mmap,
};

/*
 * xh2a_fast_memory_pm_notifier_prepare()
 *     - before pm prepare the fast memory device
 * @handle: handle of xh2a_pcie_dev
 * @rollback: true for rollback, false for prepare
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_fast_memory_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;

	xh2a_pcie_get_client(handle, &client, XH2A_FAST_MEMORY_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	fast_memory_dev = client->client_data;

	if (rollback) {
		if (atomic_dec_and_test(&fast_memory_dev->block_ioctl_flag))
			wake_up_interruptible(&fast_memory_dev->block_ioctl_wq);
	} else {
		atomic_inc(&fast_memory_dev->block_ioctl_flag);
	}

	return 0;
}

/*
 * xh2a_fast_memory_pm_notifier_complete()
 *     - after pm complete the fast memory device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_fast_memory_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;

	xh2a_pcie_get_client(handle, &client, XH2A_FAST_MEMORY_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	fast_memory_dev = client->client_data;

	(void)fast_memory_dev;

	return 0;
}

/*
 * xh2a_fast_memory_pm_prepare()
 *     - pm prepare the fast memory device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_fast_memory_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_FAST_MEMORY_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	fast_memory_dev = client->client_data;

	(void)fast_memory_dev;

	return 0;
}

/*
 * xh2a_fast_memory_pm_complete()
 *     - pm complete the fast memory device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_fast_memory_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;

	xh2a_pcie_get_client(handle, &client, XH2A_FAST_MEMORY_DEVICE_NAME);

	(void)is_compatible;
	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	fast_memory_dev = client->client_data;

	if (atomic_dec_and_test(&fast_memory_dev->block_ioctl_flag))
		wake_up_interruptible(&fast_memory_dev->block_ioctl_wq);

	return 0;
}

/*
 * xh2a_fast_memory_probe()
 *     - probe function for fast memory device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_fast_memory_probe(void *handle)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_fast_memory_dev *fast_memory_dev = NULL;

	pr_debug("%s\n", __func__);

	fast_memory_dev =
		kzalloc(sizeof(struct xh2a_fast_memory_dev), GFP_KERNEL);

	if (fast_memory_dev == NULL) {
		pr_err("%s: alloc fast_memory_dev failed\n", __func__);
		return -ENOMEM;
	}

	client = &fast_memory_dev->client;

	fast_memory_dev->private_data = handle;

	mutex_init(&fast_memory_dev->fast_memory_mutex);

	INIT_LIST_HEAD(&fast_memory_dev->file_handle_list);

	atomic_set(&fast_memory_dev->dev_initialized, 0);
	atomic_set(&fast_memory_dev->dev_removed, 0);
	kref_init(&fast_memory_dev->dev_refcnt);

	atomic_set(&fast_memory_dev->block_ioctl_flag, 0);
	init_waitqueue_head(&fast_memory_dev->block_ioctl_wq);

	memset(fast_memory_dev->name_buf, 0, XH2A_FAST_MEMORY_DEVICE_NAME_LEN);
	snprintf(fast_memory_dev->name_buf, XH2A_FAST_MEMORY_DEVICE_NAME_LEN,
		 XH2A_FAST_MEMORY_DEVICE_NAME "%d",
		 xh2a_pcie_device_index(handle));

	fast_memory_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	fast_memory_dev->miscdev.name = fast_memory_dev->name_buf;
	fast_memory_dev->miscdev.fops = &xh2a_fast_memory_fops;
	fast_memory_dev->miscdev.mode = 0666;
	fast_memory_dev->miscdev.parent = xh2a_pcie_device_ptr(handle);
	ret = misc_register(&fast_memory_dev->miscdev);

	if (ret) {
		pr_err("%s: misc_register failed\n", __func__);
		goto err_misc_register;
	}

	/* alloc dma memory */
	fast_memory_dev->dma_size = XH2A_FAST_MEMORY_DMA_SIZE;
	fast_memory_dev->dma_vaddr = dma_alloc_coherent(
		fast_memory_dev->miscdev.parent, fast_memory_dev->dma_size,
		&fast_memory_dev->dma_paddr, GFP_KERNEL);
	if (!fast_memory_dev->dma_vaddr) {
		pr_err("%s: dma_alloc_coherent failed\n", __func__);
		ret = -ENOMEM;
		goto err_dma_alloc;
	}

	allocator_core_init(&fast_memory_dev->mempool,
			    fast_memory_dev->miscdev.this_device,
			    XH2A_FAST_MEMORY_POOL_START,
			    fast_memory_dev->dma_size);

	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_FAST_MEMORY_DEVICE_NAME);

	client->work_num = 0;

	client->client_data = fast_memory_dev;
	client->private_data = handle;

	client->prepare_cb = xh2a_fast_memory_pm_prepare;
	client->complete_cb = xh2a_fast_memory_pm_complete;
	client->notifier_prepare_cb = xh2a_fast_memory_pm_notifier_prepare;
	client->notifier_complete_cb = xh2a_fast_memory_pm_notifier_complete;

	ret = xh2a_pcie_register_client(handle, client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		dma_free_coherent(fast_memory_dev->miscdev.parent,
				  fast_memory_dev->dma_size,
				  fast_memory_dev->dma_vaddr,
				  fast_memory_dev->dma_paddr);
		misc_deregister(&fast_memory_dev->miscdev);
		goto err_misc_register;
	}

	atomic_set(&fast_memory_dev->dev_initialized, 1);

	return 0;

err_dma_alloc:
	misc_deregister(&fast_memory_dev->miscdev);
err_misc_register:
	kfree(fast_memory_dev);
	return ret;
}

/*
 * xh2a_fast_memory_remove()
 *     - remove the fast memory device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_fast_memory_remove(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_fast_memory_dev *fast_memory_dev;
	struct xh2a_fast_memory_file_handle *fh_pos, *fh_n;
	struct xh2a_fast_memory_buffer_object *bo_pos, *bo_n;

	pr_debug("%s\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_FAST_MEMORY_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	fast_memory_dev = client->client_data;

	atomic_set(&fast_memory_dev->dev_removed, 1);

	mutex_lock(&fast_memory_dev->fast_memory_mutex);
	list_for_each_entry_safe(fh_pos, fh_n,
				 &fast_memory_dev->file_handle_list, node) {
		list_for_each_entry_safe(bo_pos, bo_n,
					 &fh_pos->buffer_object_list, node) {
			allocator_core_free(&fast_memory_dev->mempool,
					    bo_pos->start, bo_pos->size);
			list_del(&bo_pos->node);
			kfree(bo_pos);
		}
		list_del_init(&fh_pos->node);
	}
	mutex_unlock(&fast_memory_dev->fast_memory_mutex);

	dma_free_coherent(fast_memory_dev->miscdev.parent,
			  fast_memory_dev->dma_size, fast_memory_dev->dma_vaddr,
			  fast_memory_dev->dma_paddr);

	misc_deregister(&fast_memory_dev->miscdev);

	xh2a_pcie_unregister_client(handle, client);

	fast_memory_dev->private_data = NULL;
	kref_put(&fast_memory_dev->dev_refcnt,
		 xh2a_fast_memory_device_safe_release);

	return 0;
}

/*
 * xh2a_fast_memory_notifier_call()
 *     - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_fast_memory_notifier_call(struct notifier_block *nb,
					  unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_fast_memory_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_fast_memory_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_fast_memory_notifier_block = {
	.notifier_call = xh2a_fast_memory_notifier_call,
};

/*
 * xh2a_fast_memory_register_driver()
 *     - register fast memory driver
 */
int __init xh2a_fast_memory_register_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_fast_memory_notifier_block);
	return 0;
}

/*
 * xh2a_fast_memory_unregister_driver()
 *     - unregister fast memory driver
 */
void __exit xh2a_fast_memory_unregister_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_unregister_notifier_chain(&xh2a_fast_memory_notifier_block);
}

MODULE_LICENSE("GPL");
