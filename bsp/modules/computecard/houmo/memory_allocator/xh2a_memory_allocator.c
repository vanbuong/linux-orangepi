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
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_efuse.h>
#include "xh2a_memory_allocator.h"
#include "allocator_algorithm.h"

struct mempool_parameter {
	char name[XH2A_MEMORY_ALLOCATOR_DEVICE_NAME_LEN];
	uint64_t start_addr;
	uint64_t total_size;
};

static const struct mempool_parameter mempool_tbl[] = {
	{ XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_NAME,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_START_ADDR,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_TOTAL_SIZE },
	{ XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_NAME,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_START_ADDR,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM0_TOTAL_SIZE },
	{ XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_NAME,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_START_ADDR,
	  XH2A_MEMORY_ALLOCATOR_MEMPOOL_SPM1_TOTAL_SIZE },
};
#define MEMPOOL_TABLE_SIZE \
	((sizeof(mempool_tbl) / sizeof(struct mempool_parameter)))

static void xh2a_memory_allocator_device_safe_release(struct kref *kref)
{
	struct xh2a_memory_allocator_dev *allocator_dev = container_of(
		kref, struct xh2a_memory_allocator_dev, dev_refcnt);

	allocator_core_destroy(&allocator_dev->allocator_mempool);
	kfree(allocator_dev);
}

static int xh2a_memory_allocator_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct xh2a_memory_allocator_dev *allocator_dev = container_of(
		miscdev, struct xh2a_memory_allocator_dev, miscdev);
	struct xh2a_memory_allocator_file_handle *fh = NULL;

	if (!allocator_dev) {
		pr_err("%s: memory allocator not initialized\n", __func__);
		return -EINVAL;
	}

	if (atomic_read(&allocator_dev->dev_initialized) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (atomic_read(&allocator_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&allocator_dev->dev_refcnt) == 0) {
		pr_err("%s: device is removing...\n", __func__);
		return -ENODEV;
	}

	fh = kzalloc(sizeof(struct xh2a_memory_allocator_file_handle),
		     GFP_KERNEL);

	if (!fh) {
		dev_err(miscdev->this_device, "%s: kzalloc failed\n", __func__);
		kref_put(&allocator_dev->dev_refcnt,
			 xh2a_memory_allocator_device_safe_release);
		return -ENOMEM;
	}

	fh->allocator = allocator_dev;
	fh->pid = current->pid;
	fh->tgid = current->tgid;
	INIT_LIST_HEAD(&fh->buffer_object_list);
	INIT_LIST_HEAD(&fh->node);

	if (atomic_read(&allocator_dev->dev_removed)) {
		kref_put(&allocator_dev->dev_refcnt,
			 xh2a_memory_allocator_device_safe_release);
		kfree(fh);
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&allocator_dev->allocator_mutex);
	list_add_tail(&fh->node, &allocator_dev->file_handle_list);
	mutex_unlock(&allocator_dev->allocator_mutex);

	filp->private_data = fh;

	return 0;
}

static int xh2a_memory_allocator_release(struct inode *inode, struct file *filp)
{
	struct xh2a_memory_allocator_dev *allocator_dev = NULL;
	struct xh2a_memory_allocator_file_handle *fh = NULL;
	struct xh2a_memory_allocator_buffer_object *pos, *n;

	fh = filp->private_data;
	filp->private_data = NULL;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	allocator_dev = fh->allocator;

	if (!allocator_dev) {
		pr_err("%s: allocator is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&allocator_dev->allocator_mutex);
	list_for_each_entry_safe(pos, n, &fh->buffer_object_list, node) {
		allocator_core_free(&allocator_dev->allocator_mempool,
				    pos->start, pos->size);
		list_del(&pos->node);
		kfree(pos);
	}
	list_del_init(&fh->node);
	mutex_unlock(&allocator_dev->allocator_mutex);
	kfree(fh);
	kref_put(&allocator_dev->dev_refcnt,
		 xh2a_memory_allocator_device_safe_release);
	return 0;
}

static int xh2a_memory_allocator_ioctl_alloc(
	struct xh2a_memory_allocator_dev *allocator_dev,
	struct xh2a_memory_allocator_file_handle *fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_allocator_request_arg ioc_request;
	struct xh2a_memory_allocator_buffer_object *bo;
	uint64_t allocated_paddr = 0;

	if (!allocator_dev->start_addr || !allocator_dev->total_size) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: allocator not initialized\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		goto out;
	}

	if (ioc_request.size == 0 ||
	    (ioc_request.size > allocator_dev->total_size)) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: size invalid\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	allocated_paddr = allocator_core_alloc(
		&allocator_dev->allocator_mempool, ioc_request.size);

	if (!allocated_paddr) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: alloc failed\n", __func__);
		ret = -ENOMEM;
		goto out;
	}

	bo = kzalloc(sizeof(struct xh2a_memory_allocator_buffer_object),
		     GFP_KERNEL);

	if (!bo) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: kzalloc failed\n", __func__);
		ret = -ENOMEM;
		allocator_core_free(&allocator_dev->allocator_mempool,
				    allocated_paddr, ioc_request.size);
		goto out;
	}

	bo->start = allocated_paddr;
	bo->size = ioc_request.size;
	bo->pid = current->pid;
	bo->tgid = current->tgid;

	ioc_request.start = allocated_paddr;
	ret = copy_to_user((void __user *)arg, &ioc_request,
			   sizeof(ioc_request));

	if (ret) {
		ret = -EFAULT;
		dev_err(allocator_dev->miscdev.this_device,
			"%s: copy_to_user failed\n", __func__);
		allocator_core_free(&allocator_dev->allocator_mempool,
				    allocated_paddr, ioc_request.size);
		kfree(bo);
		goto out;
	}

	/*  add to fh list */
	list_add_tail(&bo->node, &fh->buffer_object_list);

out:
	return ret;
}

static int xh2a_memory_allocator_ioctl_free(
	struct xh2a_memory_allocator_dev *allocator_dev,
	struct xh2a_memory_allocator_file_handle *fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_allocator_request_arg ioc_request;
	struct xh2a_memory_allocator_buffer_object *pos, *n;
	int free_cnt = 0;

	ret = copy_from_user(&ioc_request, (void __user *)arg,
			     sizeof(ioc_request));

	if (ret != 0) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		goto out;
	}

	list_for_each_entry_safe(pos, n, &fh->buffer_object_list, node) {
		if (pos->start == ioc_request.start) {
			list_del(&pos->node);
			allocator_core_free(&allocator_dev->allocator_mempool,
					    pos->start, pos->size);
			kfree(pos);
			free_cnt++;
			goto out;
		}
	}

	if (free_cnt != 1) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: free 0x%llx %d times\n", __func__,
			ioc_request.start, free_cnt);
		ret = -EINVAL;
		goto out;
	}

out:
	return ret;
}

static int xh2a_memory_allocator_ioctl_infomem(
	struct xh2a_memory_allocator_dev *allocator_dev,
	struct xh2a_memory_allocator_file_handle *fh, unsigned long arg)
{
	int ret;
	struct xh2a_memory_allocator_info_mem_arg ioc_infomem;

	ret = copy_from_user(&ioc_infomem, (void __user *)arg,
			     sizeof(ioc_infomem));

	if (ret != 0) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		goto out;
	}
	ioc_infomem.start = allocator_dev->start_addr;
	ioc_infomem.total_size = allocator_dev->total_size;
	ioc_infomem.free_size = allocator_dev->allocator_mempool.free_size;
	ioc_infomem.max_free_buffer_size =
		allocator_core_get_max_free_buffer_size(
			&allocator_dev->allocator_mempool);

	ret = copy_to_user((void __user *)arg, &ioc_infomem,
			   sizeof(ioc_infomem));

	if (ret) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		goto out;
	}

out:
	return ret;
}

static long xh2a_memory_allocator_ioctl(struct file *filp, unsigned int cmd,
					unsigned long arg)
{
	int ret;
	struct miscdevice *miscdev = NULL;
	struct xh2a_memory_allocator_dev *allocator_dev = NULL;
	struct xh2a_memory_allocator_file_handle *fh = NULL;

	fh = filp->private_data;

	if (!fh) {
		pr_err("%s: fh is NULL\n", __func__);
		return -EINVAL;
	}

	allocator_dev = fh->allocator;

	if (!allocator_dev) {
		pr_err("%s: allocator is NULL\n", __func__);
		return -EINVAL;
	}

	miscdev = &allocator_dev->miscdev;

	if (atomic_read(&allocator_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	ret = mutex_lock_interruptible(&allocator_dev->allocator_mutex);

	if (ret != 0) {
		dev_err(miscdev->this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -ERESTARTSYS;
	}

	switch (cmd) {
	case IOCTL_XH2A_MEMORY_ALLOCATOR_ALLOC:
		ret = xh2a_memory_allocator_ioctl_alloc(allocator_dev, fh, arg);
		break;

	case IOCTL_XH2A_MEMORY_ALLOCATOR_FREE:
		ret = xh2a_memory_allocator_ioctl_free(allocator_dev, fh, arg);
		break;

	case IOCTL_XH2A_MEMORY_ALLOCATOR_GET_MEM_INFO:
		ret = xh2a_memory_allocator_ioctl_infomem(allocator_dev, fh,
							  arg);
		break;

	default:
		dev_err(miscdev->this_device, "%s: unknown cmd 0x%x\n",
			__func__, cmd);
		break;
	}

	mutex_unlock(&allocator_dev->allocator_mutex);

	return ret;
}

static const struct file_operations xh2a_memory_allocator_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_memory_allocator_open,
	.release = xh2a_memory_allocator_release,
	.unlocked_ioctl = xh2a_memory_allocator_ioctl,
};

static ssize_t xh2a_memory_allocator_info_show(struct device *dev,
					       struct device_attribute *attr,
					       char *buf)
{
	int ret, len;
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_memory_allocator_dev *allocator_dev = container_of(
		miscdev, struct xh2a_memory_allocator_dev, miscdev);
	struct xh2a_memory_allocator_file_handle *fh_pos, *fh_n;
	struct xh2a_memory_allocator_buffer_object *bo_pos, *bo_n;

	ret = mutex_lock_interruptible(&allocator_dev->allocator_mutex);

	if (ret) {
		dev_err(miscdev->this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return 0;
	}

	len = sprintf(buf, "%s memory pool infomation:\n", __func__);
	len += sprintf(buf + len, "start_addr = 0x%llx\n",
		       allocator_dev->start_addr);
	len += sprintf(buf + len, "total_size = 0x%llx\n",
		       allocator_dev->total_size);
	len += sprintf(buf + len, "free_size = 0x%llx\n",
		       allocator_dev->allocator_mempool.free_size);

	list_for_each_entry_safe(fh_pos, fh_n, &allocator_dev->file_handle_list,
				 node) {
		len += sprintf(buf + len,
			       "\tfile handler: tgid = %d, pid = %d\n",
			       fh_pos->tgid, fh_pos->pid);
		list_for_each_entry_safe(bo_pos, bo_n,
					 &fh_pos->buffer_object_list, node) {
			len += sprintf(buf + len,
				       "\t\tbo: addr = 0x%llx, size = 0x%llx\n",
				       bo_pos->start, bo_pos->size);
		}
		len += sprintf(buf + len, "\n");
	}

	mutex_unlock(&allocator_dev->allocator_mutex);
	return len;
}
static DEVICE_ATTR_RO(xh2a_memory_allocator_info);

static struct attribute *xh2a_memory_allocator_attrs[] = {
	&dev_attr_xh2a_memory_allocator_info.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xh2a_memory_allocator);

static int xh2a_memory_allocator_update_ddr_size(void *handle, uint64_t *size)
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
 * xh2a_memory_allocator_probe_one() - probe for one memory pool
 * @handle: handle for pcie device
 * @mempool_name: the name of the memory pool
 * @mempool_start: start address of the memory pool
 * @mempool_size: size of the memory pool
 */
static int xh2a_memory_allocator_probe_one(void *handle,
					   const char *mempool_name,
					   uint64_t mempool_start,
					   uint64_t mempool_size)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_allocator_dev *allocator_dev = NULL;
	uint64_t size;

	pr_debug("%s: %s\n", __func__, mempool_name);

	allocator_dev =
		kzalloc(sizeof(struct xh2a_memory_allocator_dev), GFP_KERNEL);

	if (allocator_dev == NULL) {
		pr_err("%s: alloc allocator_dev failed\n", __func__);
		return -ENOMEM;
	}

	client = &allocator_dev->client;

	allocator_dev->private_data = handle;

	mutex_init(&allocator_dev->allocator_mutex);

	INIT_LIST_HEAD(&allocator_dev->file_handle_list);

	atomic_set(&allocator_dev->dev_initialized, 0);
	atomic_set(&allocator_dev->dev_removed, 0);
	kref_init(&allocator_dev->dev_refcnt);

	memset(allocator_dev->name_buf, 0,
	       XH2A_MEMORY_ALLOCATOR_DEVICE_NAME_LEN);
	snprintf(allocator_dev->name_buf, XH2A_MEMORY_ALLOCATOR_DEVICE_NAME_LEN,
		 XH2A_MEMORY_ALLOCATOR_DEVICE_NAME "%d_%s",
		 xh2a_pcie_device_index(handle), mempool_name);

	allocator_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	allocator_dev->miscdev.name = allocator_dev->name_buf;
	allocator_dev->miscdev.fops = &xh2a_memory_allocator_fops;
	allocator_dev->miscdev.mode = 0666;
	allocator_dev->miscdev.parent = xh2a_pcie_device_ptr(handle);
	allocator_dev->miscdev.groups = xh2a_memory_allocator_groups;

	ret = misc_register(&allocator_dev->miscdev);

	if (ret) {
		pr_err("%s: misc_register failed\n", __func__);
		goto err_misc_register;
	}

	allocator_dev->allocator_mempool.dev =
		allocator_dev->miscdev.this_device;

	size = mempool_size;
	if (strcmp(mempool_name, XH2A_MEMORY_ALLOCATOR_MEMPOOL_DDR_NAME) == 0) {
		ret = xh2a_memory_allocator_update_ddr_size(handle, &size);
		if ((ret == 0) && (size != 0)) {
			size -= XH2A_DEVICE_SYSTEM_SIZE;
		} else {
			size = mempool_size;
		}
	}

	allocator_dev->start_addr = mempool_start;
	allocator_dev->total_size = size;
	ret = allocator_core_init(&allocator_dev->allocator_mempool,
				  allocator_dev->miscdev.this_device,
				  allocator_dev->start_addr,
				  allocator_dev->total_size);
	if (ret) {
		dev_err(allocator_dev->miscdev.this_device,
			"%s: allocator_core_init failed\n", __func__);
		goto err_core_init;
	}
	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_MEMORY_ALLOCATOR_DEVICE_NAME "_%s", mempool_name);

	client->work_num = 0;

	client->client_data = allocator_dev;
	client->private_data = handle;

	ret = xh2a_pcie_register_client(handle, client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		misc_deregister(&allocator_dev->miscdev);
		goto err_misc_register;
	}

	atomic_set(&allocator_dev->dev_initialized, 1);

	return 0;

err_core_init:
	misc_deregister(&allocator_dev->miscdev);
err_misc_register:
	kfree(allocator_dev);
	return ret;
}

/*
 * xh2a_memory_allocator_remove_one() - remove one memory pool
 * @handle: handle for pcie device
 * @mempool_name: the name of the memory pool
 */
static int xh2a_memory_allocator_remove_one(void *handle,
					    const char *mempool_name)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_memory_allocator_dev *allocator_dev;
	struct xh2a_memory_allocator_file_handle *fh_pos, *fh_n;
	struct xh2a_memory_allocator_buffer_object *bo_pos, *bo_n;
	char client_name[XH2A_MEMORY_ALLOCATOR_DEVICE_NAME_LEN];

	pr_debug("%s: %s\n", __func__, mempool_name);
	snprintf(client_name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_MEMORY_ALLOCATOR_DEVICE_NAME "_%s", mempool_name);

	xh2a_pcie_get_client(handle, &client, client_name);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	allocator_dev = client->client_data;

	atomic_set(&allocator_dev->dev_removed, 1);

	mutex_lock(&allocator_dev->allocator_mutex);
	list_for_each_entry_safe(fh_pos, fh_n, &allocator_dev->file_handle_list,
				 node) {
		list_for_each_entry_safe(bo_pos, bo_n,
					 &fh_pos->buffer_object_list, node) {
			allocator_core_free(&allocator_dev->allocator_mempool,
					    bo_pos->start, bo_pos->size);
			list_del(&bo_pos->node);
			kfree(bo_pos);
		}
		list_del_init(&fh_pos->node);
	}
	mutex_unlock(&allocator_dev->allocator_mutex);

	misc_deregister(&allocator_dev->miscdev);

	xh2a_pcie_unregister_client(handle, client);

	allocator_dev->private_data = NULL;
	kref_put(&allocator_dev->dev_refcnt,
		 xh2a_memory_allocator_device_safe_release);
	return 0;
}

/*
 * xh2a_memory_allocator_probe()
 *     - probe function for memory_allocator device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_allocator_probe(void *handle)
{
	int i, ret;

	for (i = 0; i < MEMPOOL_TABLE_SIZE; i++) {
		ret = xh2a_memory_allocator_probe_one(
			handle, mempool_tbl[i].name, mempool_tbl[i].start_addr,
			mempool_tbl[i].total_size);

		if (ret != 0)
			return ret;
	}

	return 0;
}

/*
 * xh2a_memory_allocator_remove()
 *     - remove the memory_allocator device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_memory_allocator_remove(void *handle)
{
	int i;

	for (i = 0; i < MEMPOOL_TABLE_SIZE; i++)
		xh2a_memory_allocator_remove_one(handle, mempool_tbl[i].name);

	return 0;
}

/*
 * xh2a_memory_allocator_notifier_call()
 *     - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_memory_allocator_notifier_call(struct notifier_block *nb,
					       unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_memory_allocator_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_memory_allocator_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_memory_allocator_notifier_block = {
	.notifier_call = xh2a_memory_allocator_notifier_call,
};

/*
 * xh2a_memory_allocator_register_driver()
 *     - register memory_allocator driver
 */
int __init xh2a_memory_allocator_register_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_register_notifier_chain(
		&xh2a_memory_allocator_notifier_block);
	return 0;
}

/*
 * xh2a_memory_allocator_unregister_driver()
 *     - unregister memory_allocator driver
 */
void __exit xh2a_memory_allocator_unregister_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_unregister_notifier_chain(
		&xh2a_memory_allocator_notifier_block);
}

MODULE_LICENSE("GPL");
