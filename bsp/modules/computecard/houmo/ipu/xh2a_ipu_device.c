// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/suspend.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_spm_api.h>
#include <xh2a_address.h>

#include "xh2a_ipu_device.h"
#include "xh2a_ipu_file_handle.h"
#include "xh2a_ipu_policy.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_ioctl.h"
#include "xh2a_ipu_interrupt.h"
#include "xh2a_ipu_mempool.h"

static void xh2a_device_schedule_work(struct work_struct *work)
{
	struct xh2a_ipu_group *group = NULL;
	struct xh2a_ipu_device *ipu_dev =
		container_of(work, struct xh2a_ipu_device, group_sche_work);

	dev_dbg(ipu_dev->miscdev.this_device, "ipu schedule group work\n");

	mutex_lock(&ipu_dev->dev_mutex);

	if (!ipu_dev->policy) {
		mutex_unlock(&ipu_dev->dev_mutex);
		return;
	}

	group = ipu_dev->policy->ops->pick_runnable_group(ipu_dev->policy);

	while (group) {
		xh2a_ipu_group_kds_load(group);
		mutex_unlock(&ipu_dev->dev_mutex);
		mutex_lock(&ipu_dev->dev_mutex);

		if (!ipu_dev->policy)
			break;

		group = ipu_dev->policy->ops->pick_runnable_group(
			ipu_dev->policy);
	}
	mutex_unlock(&ipu_dev->dev_mutex);
}

static void xh2a_ipu_device_safe_release(struct kref *kref)
{
	struct xh2a_ipu_device *ipu_dev =
		container_of(kref, struct xh2a_ipu_device, dev_refcnt);

	kfree(ipu_dev);
}

static int xh2a_ipu_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct xh2a_ipu_device *ipu_dev =
		container_of(miscdev, struct xh2a_ipu_device, miscdev);
	struct xh2a_ipu_file_handle *file_handle = NULL;

	if (!ipu_dev) {
		dev_err(miscdev->this_device, "invalid device\n");
		return -EINVAL;
	}

	if (atomic_read(&ipu_dev->dev_initialized) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (atomic_read(&ipu_dev->dev_removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&ipu_dev->dev_refcnt) == 0) {
		pr_err("%s: device is removing\n", __func__);
		return -ENODEV;
	}

	file_handle = xh2a_ipu_file_handle_create(ipu_dev);
	if (IS_ERR_OR_NULL(file_handle)) {
		dev_err(miscdev->this_device,
			"%s: xh2a_ipu_file_handle_create failed\n", __func__);
		kref_put(&ipu_dev->dev_refcnt, xh2a_ipu_device_safe_release);
		return -ENOMEM;
	}

	filp->private_data = file_handle;

	return 0;
}

static int xh2a_ipu_release(struct inode *inode, struct file *filp)
{
	struct xh2a_ipu_file_handle *file_handle = filp->private_data;
	struct xh2a_ipu_device *ipu_dev = NULL;

	filp->private_data = NULL;

	if (!file_handle) {
		pr_err("%s: file_handle is NULL\n", __func__);
		return -EINVAL;
	}

	ipu_dev = file_handle->ipu_dev;

	if (!ipu_dev) {
		pr_err("%s: ipu_dev is NULL\n", __func__);
		return -EINVAL;
	}

	xh2a_ipu_file_handle_destroy(file_handle);

	kref_put(&ipu_dev->dev_refcnt, xh2a_ipu_device_safe_release);

	return 0;
}

static long xh2a_ipu_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	return xh2a_ipu_ioctl_dispatch(filp, cmd, arg);
}

static const struct file_operations xh2a_ipu_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_ipu_open,
	.release = xh2a_ipu_release,
	.unlocked_ioctl = xh2a_ipu_ioctl,
};

static ssize_t device_id_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);

	struct xh2a_ipu_device *pipu =
		container_of(miscdev, struct xh2a_ipu_device, miscdev);
	return sprintf(buf, "%d\n", xh2a_pcie_device_index(pipu->private_data));
}
static DEVICE_ATTR_RO(device_id);

static ssize_t device_dbdf_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	uint32_t dbdf = 0;
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_ipu_device *pipu =
		container_of(miscdev, struct xh2a_ipu_device, miscdev);

	xh2a_pcie_device_dbdf(pipu->private_data, &dbdf);
	return sprintf(buf, "%04x:%02x:%02x.%02x\n", (dbdf >> 16) & 0xffff,
		       (dbdf >> 8) & 0xff, (dbdf >> 3) & 0x1f, dbdf & 0x7);
}
static DEVICE_ATTR_RO(device_dbdf);

static struct attribute *xh2a_ipu_info_node_attrs[] = {
	&dev_attr_device_id.attr,
	&dev_attr_device_dbdf.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xh2a_ipu_info_node);

static void xh2a_ipu_spm_save_snapshot(struct xh2a_ipu_device *ipu_dev)
{
	int ret;

	/*save spm. TODO. add efuse detect which core is good*/
	if (ipu_dev->available_tiles_mask & 0xf) {
		ret = xh2a_pcie_dma_read_mem(ipu_dev->private_data,
					     XH2A_DEVICE_SPM0_START,
					     ipu_dev->spm_snapshot_buf[0],
					     XH2A_DEVICE_SPM0_SIZE);

		if (ret != 0)
			dev_err(ipu_dev->miscdev.this_device,
				"%s: dma read fail\n", __func__);
	}

	if (ipu_dev->available_tiles_mask & 0xf0) {
		ret = xh2a_pcie_dma_read_mem(ipu_dev->private_data,
					     XH2A_DEVICE_SPM1_START,
					     ipu_dev->spm_snapshot_buf[1],
					     XH2A_DEVICE_SPM1_SIZE);

		if (ret != 0)
			dev_err(ipu_dev->miscdev.this_device,
				"%s: dma read fail\n", __func__);
	}
}

static void xh2a_ipu_spm_restore_snapshot(struct xh2a_ipu_device *ipu_dev)
{
	int ret;

	/*save spm. TODO. add efuse detect which core is good*/
	if (ipu_dev->available_tiles_mask & 0xf) {
		ret = xh2a_pcie_dma_write_mem(ipu_dev->private_data,
					      XH2A_DEVICE_SPM0_START,
					      ipu_dev->spm_snapshot_buf[0],
					      XH2A_DEVICE_SPM0_SIZE);

		if (ret != 0)
			dev_err(ipu_dev->miscdev.this_device,
				"%s: dma write fail\n", __func__);
	}

	if (ipu_dev->available_tiles_mask & 0xf0) {
		ret = xh2a_pcie_dma_write_mem(ipu_dev->private_data,
					      XH2A_DEVICE_SPM1_START,
					      ipu_dev->spm_snapshot_buf[1],
					      XH2A_DEVICE_SPM1_SIZE);

		if (ret != 0)
			dev_err(ipu_dev->miscdev.this_device,
				"%s: dma write fail\n", __func__);
	}
}

/*
 * xh2a_ipu_pm_notifier_prepare()
 * 	- before pm prepare the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	ipu_dev = client->client_data;

	xh2a_ipu_load_stop(ipu_dev);

	if (rollback) {
		xh2a_ipu_load_start(ipu_dev);

		if (atomic_dec_and_test(&ipu_dev->block_ioctl_flag))
			wake_up_interruptible(&ipu_dev->block_ioctl_wq);
	} else {
		atomic_inc(&ipu_dev->block_ioctl_flag);
	}

	return 0;
}

/*
 * xh2a_ipu_pm_notifier_complete()
 * 	- after pm complete the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	ipu_dev = client->client_data;

	atomic_set(&ipu_dev->is_reboot, 0);
	xh2a_ipu_load_start(ipu_dev);

	return 0;
}

/*
 * xh2a_ipu_pm_prepare()
 * 	- pm prepare the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;
	struct xh2a_ipu_policy *old = NULL;
	struct xh2a_ipu_file_handle *fh_pos, *fh_n;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	if (ipu_dev->group_work_inited)
		cancel_work_sync(&ipu_dev->group_sche_work);

	xh2a_ipu_spm_save_snapshot(ipu_dev);
	xh2a_ipu_hw_shutdown(ipu_dev);

	mutex_lock(&ipu_dev->dev_mutex);
	old = ipu_dev->policy;
	ipu_dev->policy = NULL;
	mutex_unlock(&ipu_dev->dev_mutex);

	if (old)
		xh2a_ipu_policy_destroy(old);

	/* if not s2idle, mark all fd to invald */
	mutex_lock(&ipu_dev->fh_mutex);

	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
		list_for_each_entry_safe(fh_pos, fh_n,
					 &ipu_dev->file_handle_list, node) {
			atomic_set(&fh_pos->drop_flag, 1);
		}

	mutex_unlock(&ipu_dev->fh_mutex);
	return 0;
}

/*
 * xh2a_ipu_pm_complete()
 * 	- pm complete the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev->policy = xh2a_ipu_policy_create(ipu_dev);

	xh2a_ipu_hw_startup(ipu_dev);
	xh2a_ipu_spm_restore_snapshot(ipu_dev);

	if (atomic_dec_and_test(&ipu_dev->block_ioctl_flag))
		wake_up_interruptible(&ipu_dev->block_ioctl_wq);

	return 0;
}

/*
 * xh2a_ipu_pm_runtime_suspend()
 * 	- pm runtime suspend the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_runtime_suspend(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	xh2a_ipu_load_stop(ipu_dev);

	xh2a_ipu_spm_save_snapshot(ipu_dev);
	xh2a_ipu_hw_shutdown(ipu_dev);

	return 0;
}

/*
 * xh2a_ipu_pm_runtime_resume()
 * 	- pm runtime resume the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_pm_runtime_resume(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	ipu_dev = client->client_data;

	if (ipu_dev == NULL) {
		pr_err("%s: Client data is NULL for '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return -EINVAL;
	}

	xh2a_ipu_hw_startup(ipu_dev);
	xh2a_ipu_spm_restore_snapshot(ipu_dev);

	xh2a_ipu_load_start(ipu_dev);

	return 0;
}

struct xh2a_ipu_device *xh2a_ipu_device_create(void *handle)
{
	int ret, i;
	uint32_t mask;
	char wq_name[XH2A_IPU_WQ_NAME_LEN];
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev = NULL;

	ipu_dev = kzalloc(sizeof(struct xh2a_ipu_device), GFP_KERNEL);

	if (!ipu_dev) {
		pr_err("%s: alloc ipu_dev failed\n", __func__);
		return NULL;
	}

	ipu_dev->private_data = handle;

	atomic_set(&ipu_dev->dev_initialized, 0);
	atomic_set(&ipu_dev->dev_removed, 0);
	kref_init(&ipu_dev->dev_refcnt);

	memset(ipu_dev->name, 0, XH2A_IPU_NAME_LEN);
	memset(wq_name, 0, XH2A_IPU_WQ_NAME_LEN);
	snprintf(ipu_dev->name, XH2A_IPU_NAME_LEN, XH2A_IPU_DEVICE_NAME "%d",
		 xh2a_pcie_device_index(handle));
	snprintf(wq_name, XH2A_IPU_WQ_NAME_LEN, XH2A_IPU_DEVICE_NAME "%d_wq",
		 xh2a_pcie_device_index(handle));

	ipu_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	ipu_dev->miscdev.name = ipu_dev->name;
	ipu_dev->miscdev.fops = &xh2a_ipu_fops;
	ipu_dev->miscdev.mode = 0666;
	ipu_dev->miscdev.parent = xh2a_pcie_device_ptr(handle);
	ipu_dev->miscdev.groups = xh2a_ipu_info_node_groups;

	ret = misc_register(&ipu_dev->miscdev);

	if (ret) {
		pr_err("%s: misc_register failed\n", __func__);
		goto err_malloc;
	}

	client = &ipu_dev->client;
	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN, XH2A_IPU_DEVICE_NAME);

	xh2a_ipu_msi_work_register(ipu_dev);

	client->client_data = ipu_dev;
	client->private_data = handle;

	client->prepare_cb = xh2a_ipu_pm_prepare;
	client->complete_cb = xh2a_ipu_pm_complete;
	client->notifier_prepare_cb = xh2a_ipu_pm_notifier_prepare;
	client->notifier_complete_cb = xh2a_ipu_pm_notifier_complete;
	atomic_set(&ipu_dev->block_ioctl_flag, 0);
	init_waitqueue_head(&ipu_dev->block_ioctl_wq);
	client->runtime_suspend_cb = xh2a_ipu_pm_runtime_suspend;
	client->runtime_resume_cb = xh2a_ipu_pm_runtime_resume;

	ipu_dev->spm_snapshot_buf[0] = vmalloc(XH2A_DEVICE_SPM0_SIZE);
	ipu_dev->spm_snapshot_buf[1] = vmalloc(XH2A_DEVICE_SPM1_SIZE);
	if (ipu_dev->spm_snapshot_buf[0] == NULL ||
	    ipu_dev->spm_snapshot_buf[1] == NULL) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: Failed to allocate memory\n", __func__);
		goto err_mem;
	}

	ipu_dev->group_wq = create_singlethread_workqueue(wq_name);
	if (!ipu_dev->group_wq) {
		dev_err(ipu_dev->miscdev.this_device, "create workqueue "
						      "failed\n");
		goto err_misc_dev;
	}

	ipu_dev->last_core = -1;
	ipu_dev->policy = xh2a_ipu_policy_create(ipu_dev);
	if (!ipu_dev->policy)
		goto err_workqueue;

	ret = xh2a_ipu_get_efuse_info(ipu_dev);
	if (ret != 0 || ipu_dev->available_tiles_mask == 0) {
		/* To do : Adapt chip without burned efuse */
		ipu_dev->available_tiles_mask = 0xFF;
		ipu_dev->available_cores_num = 0x2;
	}

	INIT_WORK(&ipu_dev->group_sche_work, xh2a_device_schedule_work);
	INIT_WORK(&ipu_dev->ipu_event_work, xh2a_ipu_event_work);
	INIT_WORK(&ipu_dev->ipu_load_work, xh2a_ipu_load_work);
	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		mask = (0xF << (i * 4));
		if ((ipu_dev->available_tiles_mask & mask) != mask)
			continue;

		ipu_dev->core_works[i].msi_id = i;
		INIT_WORK(&ipu_dev->core_works[i].work,
			  xh2a_ipuss_core_bh_work);
	}

	ipu_dev->group_work_inited = true;

	mutex_init(&ipu_dev->dev_mutex);
	mutex_init(&ipu_dev->fh_mutex);
	mutex_init(&ipu_dev->dump_mutex);

	INIT_LIST_HEAD(&ipu_dev->file_handle_list);
	INIT_LIST_HEAD(&ipu_dev->isr_event_list);
	spin_lock_init(&ipu_dev->event_lock);

	ret = xh2a_pcie_register_client(handle, client);
	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_policy;
	}

	xh2a_ipu_booter_pre_startup(ipu_dev);

	xh2a_ipu_hw_startup(ipu_dev);

	xh2a_ipu_event_pool_create(&ipu_dev->event_pool);

	xh2a_ipu_load_init(ipu_dev);

	atomic_set(&ipu_dev->dev_initialized, 1);

	return ipu_dev;

err_policy:
	cancel_work_sync(&ipu_dev->group_sche_work);
	cancel_work_sync(&ipu_dev->ipu_event_work);
	cancel_work_sync(&ipu_dev->ipu_load_work);

	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		mask = (0xF << (i * 4));
		if ((ipu_dev->available_tiles_mask & mask) != mask)
			continue;

		cancel_work_sync(&ipu_dev->core_works[i].work);
	}
	ipu_dev->group_work_inited = false;

	xh2a_ipu_policy_destroy(ipu_dev->policy);

err_workqueue:
	destroy_workqueue(ipu_dev->group_wq);
	ipu_dev->group_wq = NULL;

err_mem:
	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		if (ipu_dev->spm_snapshot_buf[i])
			vfree(ipu_dev->spm_snapshot_buf[i]);
	}

err_misc_dev:
	xh2a_ipu_msi_work_unregister(ipu_dev);
	misc_deregister(&ipu_dev->miscdev);

err_malloc:
	kfree(ipu_dev);
	ipu_dev = NULL;

	return NULL;
}

void xh2a_ipu_device_destroy(struct xh2a_ipu_device *ipu_dev)
{
	int i;
	uint32_t mask;
	struct xh2a_pcie_client *client = &ipu_dev->client;
	struct xh2a_ipu_file_handle *fh_pos, *fh_n;

	if (!ipu_dev)
		return;

	atomic_set(&ipu_dev->dev_removed, 1);

	mutex_lock(&ipu_dev->fh_mutex);
	list_for_each_entry_safe(fh_pos, fh_n, &ipu_dev->file_handle_list,
				 node) {
		list_del_init(&fh_pos->node);
		xh2a_ipu_file_handle_stop_group(fh_pos);
	}
	mutex_unlock(&ipu_dev->fh_mutex);

	xh2a_ipu_load_exit(ipu_dev);

	xh2a_ipu_event_pool_destroy(&ipu_dev->event_pool);

	xh2a_ipu_hw_shutdown(ipu_dev);

	if (ipu_dev->group_work_inited) {
		cancel_work_sync(&ipu_dev->group_sche_work);
		cancel_work_sync(&ipu_dev->ipu_event_work);
		cancel_work_sync(&ipu_dev->ipu_load_work);
		for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
			mask = (0xF << (i * 4));
			if ((ipu_dev->available_tiles_mask & mask) != mask)
				continue;

			cancel_work_sync(&ipu_dev->core_works[i].work);
		}

		ipu_dev->group_work_inited = false;
	}

	if (ipu_dev->policy) {
		xh2a_ipu_policy_destroy(ipu_dev->policy);
		ipu_dev->policy = NULL;
	}

	if (ipu_dev->group_wq) {
		destroy_workqueue(ipu_dev->group_wq);
		ipu_dev->group_wq = NULL;
	}

	xh2a_ipu_msi_work_unregister(ipu_dev);

	for (i = 0; i < XH2A_IPU_CORE_NUM; i++) {
		if (ipu_dev->spm_snapshot_buf[i])
			vfree(ipu_dev->spm_snapshot_buf[i]);
	}

	misc_deregister(&ipu_dev->miscdev);

	xh2a_pcie_unregister_client(client->private_data, client);
	ipu_dev->private_data = NULL;
	kref_put(&ipu_dev->dev_refcnt, xh2a_ipu_device_safe_release);
}

/*
 * xh2a_ipu_probe() - init function for ipu device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_probe(void *handle)
{
	struct xh2a_ipu_device *ipu_dev = NULL;

	pr_debug("%s:\n", __func__);

	ipu_dev = xh2a_ipu_device_create(handle);

	if (!ipu_dev) {
		pr_err("%s: xh2a_ipu device init failed\n", __func__);
		return -ENOMEM;
	}

	return 0;
}

/*
 * xh2a_ipu_remove() - remove the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_remove(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	pr_debug("%s:\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return 0;
	}

	ipu_dev = client->client_data;

	xh2a_ipu_device_destroy(ipu_dev);

	return 0;
}

/*
 * xh2a_ipu_shutdown() - shutdown the ipu device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_ipu_shutdown(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_ipu_device *ipu_dev;

	pr_debug("%s:\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_IPU_DEVICE_NAME);

	if (client == NULL) {
		pr_err("%s: Failed to get client for name '%s'\n", __func__,
		       XH2A_IPU_DEVICE_NAME);
		return 0;
	}

	ipu_dev = client->client_data;

	xh2a_ipu_load_exit(ipu_dev);

	return 0;
}

/*
 * xh2a_ipu_notifier_call() - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_ipu_notifier_call(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_ipu_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_ipu_remove(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_SHUTDOWN)
		ret = xh2a_ipu_shutdown(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_ipu_notifier_block = {
	.notifier_call = xh2a_ipu_notifier_call,
};

/*
 * xh2a_ipu_register_driver() - register ipu driver
 */
int __init xh2a_ipu_register_driver(void)
{
	pr_debug("%s:\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_ipu_notifier_block);
	return 0;
}

/*
 * xh2a_ipu_unregister_driver() - unregister ipu driver
 */
void __exit xh2a_ipu_unregister_driver(void)
{
	pr_debug("%s:\n", __func__);
	xh2a_host_unregister_notifier_chain(&xh2a_ipu_notifier_block);
}

MODULE_LICENSE("GPL");
