// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/ioctl.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include "xh2a_ipu_ioctl.h"
#include "xh2a_ipu_device.h"
#include "xh2a_ipu_file_handle.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_kernel.h"

static int xh2a_destroy_group(struct file *filp, int __user *arg)
{
	int ret, group_id;
	struct xh2a_ipu_group *group = NULL;
	struct xh2a_ipu_file_handle *file_handle = filp->private_data;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;

	if (!arg) {
		dev_err(ipu_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);

		return -EINVAL;
	}

	dev_dbg(ipu_dev->miscdev.this_device,
		"%s: IOCTL_XH2A_IPU_DESTROY_GROUP\n", __func__);

	if (copy_from_user(&group_id, (int __user *)arg, sizeof(int))) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);

		return -EFAULT;
	}

	ret = mutex_lock_interruptible(&file_handle->file_mutex);
	if (ret != 0) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -EIO;
	}

	group = idr_find(&file_handle->group_idr, group_id);
	idr_remove(&file_handle->group_idr, group_id);
	mutex_unlock(&file_handle->file_mutex);

	if (!group) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: no match for group_id %d\n", __func__, group_id);

		return -EFAULT;
	}

	ret = xh2a_ipu_group_destroy(group);
	if (ret) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: xh2a_ipu_group_destroy failed, ret = %d\n",
			__func__, ret);
		return ret;
	}

	return ret;
}

static int xh2a_create_group(struct file *filp, int __user *arg)
{
	int group_id;
	struct xh2a_ipu_group *group = NULL;
	struct xh2a_ipu_file_handle *file_handle = filp->private_data;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;

	if (!arg) {
		dev_err(ipu_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);

		return -EINVAL;
	}

	group = xh2a_ipu_group_create(file_handle);
	if (!group) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: xh2a_ipu_group_create failed\n", __func__);
		return -ENOMEM;
	}

	group_id = group->id;

	if (copy_to_user((int __user *)arg, &group_id, sizeof(group_id))) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: copy_to_user failed\n", __func__);

		return -EFAULT;
	}

	return 0;
}

static int
xh2a_launch_kernel_to_group(struct file *filp,
			    const struct xh2a_group_ioctl_cmd __user *arg)
{
	int ret;
	group_id_t group_id;
	struct xh2a_ipu_group *group = NULL;
	struct xh2a_ipu_file_handle *file_handle = filp->private_data;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;
	struct xh2a_group_ioctl_cmd ipu_cmd = { 0 };

	if (!arg) {
		dev_err(ipu_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);

		return -EINVAL;
	}

	if (copy_from_user(&ipu_cmd, arg, sizeof(ipu_cmd))) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);

		return -EFAULT;
	}

	group_id = ipu_cmd.group_id;

	ret = mutex_lock_interruptible(&file_handle->file_mutex);
	if (ret != 0) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -EIO;
	}

	group = idr_find(&file_handle->group_idr, group_id);
	mutex_unlock(&file_handle->file_mutex);

	if (!group) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: no match for group_id %d\n", __func__, group_id);

		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&group->mutex);
	if (ret != 0) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return ret;
	}

	if (ipu_dev->available_cores_num < ipu_cmd.cmd.kld.core_num) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: not enough available cores for group_id %d\n",
			__func__, group_id);
		mutex_unlock(&group->mutex);
		return -EINVAL;
	}

	if (group->core_num == 0 && group->tile_num == 0 &&
	    group->param_type == 0) {
		group->core_num = ipu_cmd.cmd.kld.core_num;
		group->tile_num = ipu_cmd.cmd.kld.tile_num;
		group->param_type = ipu_cmd.cmd.kld.param_type;
	}

	if ((ipu_cmd.cmd.kld.core_num != group->core_num) ||
	    (ipu_cmd.cmd.kld.tile_num != group->tile_num) ||
	    (ipu_cmd.cmd.kld.param_type != group->param_type)) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: err KLD for group_id %d\n", __func__, group_id);
		mutex_unlock(&group->mutex);

		return -EINVAL;
	}

	dev_dbg(ipu_dev->miscdev.this_device,
		"%s: group_id %d, core_num %d, tile_num %d\n", __func__,
		group_id, group->core_num, group->tile_num);

	dev_dbg(ipu_dev->miscdev.this_device,
		"%s: group_id %d, param_type %d, kernel timeout = %d\n",
		__func__, group_id, group->param_type,
		ipu_cmd.cmd.kld.timeout_us);

	ret = xh2a_ipu_group_add_kernel(group, &ipu_cmd.cmd.kld);
	if (ret) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: xh2a_ipu_group_add_kernel failed, ret = %d\n",
			__func__, ret);
		mutex_unlock(&group->mutex);

		return ret;
	}

	mutex_unlock(&group->mutex);

	dev_dbg(ipu_dev->miscdev.this_device, "group %u %p launch %u kernels\n",
		group->id, group, group->kernel_num);

	return 0;
}

static int xh2a_execute_group(struct file *filp,
			      struct xh2a_group_ioctl_cmd __user *arg)
{
	int ret;
	struct xh2a_ipu_group *group = NULL;
	struct xh2a_group_ioctl_cmd ipu_cmd = { 0 };
	struct xh2a_ipu_file_handle *file_handle = filp->private_data;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;
	int oldstate;

	if (!arg) {
		dev_err(ipu_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);

		return -EINVAL;
	}

	dev_dbg(ipu_dev->miscdev.this_device,
		"%s: IOCTL_XH2A_IPU_EXECUTE_GROUP\n", __func__);

	if (copy_from_user(&ipu_cmd, (int __user *)arg, sizeof(ipu_cmd))) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);

		return -EFAULT;
	}

	ret = mutex_lock_interruptible(&file_handle->file_mutex);
	if (ret != 0) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: mutex_lock_interruptible failed\n", __func__);
		return -EIO;
	}

	group = idr_find(&file_handle->group_idr, ipu_cmd.group_id);
	mutex_unlock(&file_handle->file_mutex);

	if (!group) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: no match for group_id %d\n", __func__,
			ipu_cmd.group_id);

		return -EFAULT;
	}

	oldstate = atomic_cmpxchg(&group->status, GROUP_PACKING, GROUP_PENDING);
	if (oldstate == GROUP_CREATED) {
		oldstate = atomic_xchg(&group->status, GROUP_DONE);
		dev_dbg(ipu_dev->miscdev.this_device,
			"group %u %p sync done without kernels, state: %d\n",
			group->id, group, oldstate);
		return 0;
	}

	if (oldstate != GROUP_PACKING) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group status %d error %d\n", __func__, oldstate,
			group->id);

		return -EACCES;
	}

	if (group->kernel_num > XH2A_GROUP_MAX_KERNEL_NUM) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: group %u %p unsupport kernel num: %u\n", __func__,
			group->id, group, group->kernel_num);

		return -ENOMEM;
	}

	if ((group->core_num > XH2A_IPU_CORE_NUM) ||
	    (group->tile_num > XH2A_TILE_NUM_PER_CORE)) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: err core/tile num, group_id %d\n", __func__,
			group->id);
		return -EINVAL;
	}

	if ((group->param_type != XH2A_GROUP_PARAM_DDR) &&
	    (group->param_type != XH2A_GROUP_PARAM_SPM)) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: err param type, group_id %d\n", __func__,
			group->id);
		return -EINVAL;
	}

	ret = xh2a_ipu_group_execute(group, ipu_cmd.timeout_us);
	if (ret) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: xh2a_ipu_group_execute failed, ret = %d\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static int xh2a_ipu_get_load(struct file *filp, int __user *arg)
{
	uint32_t core_id, ipu_load;
	unsigned long flags;
	struct xh2a_ipu_file_handle *file_handle = filp->private_data;
	struct xh2a_ipu_device *ipu_dev = file_handle->ipu_dev;

	if (!arg) {
		dev_err(ipu_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);

		return -EINVAL;
	}

	dev_dbg(ipu_dev->miscdev.this_device,
		"%s: IOCTL_XH2A_IPU_DESTROY_GROUP\n", __func__);

	if (copy_from_user(&core_id, (uint32_t __user *)arg,
			   sizeof(uint32_t))) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);

		return -EFAULT;
	}

	spin_lock_irqsave(&ipu_dev->load_lock, flags);
	xh2a_ipu_get_core_load(ipu_dev, core_id, &ipu_load);
	spin_unlock_irqrestore(&ipu_dev->load_lock, flags);

	if (copy_to_user((uint32_t __user *)arg, &ipu_load, sizeof(uint32_t))) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: copy_to_user failed\n", __func__);

		return -EFAULT;
	}

	return 0;
}

long xh2a_ipu_ioctl_dispatch(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	int ret = 0;
	uint32_t cmd_size;
	struct miscdevice *miscdev = NULL;
	struct xh2a_ipu_file_handle *file_handle = NULL;
	struct xh2a_ipu_device *ipu_dev = NULL;
	struct xh2a_ipu_policy *old = NULL;

	file_handle = filp->private_data;

	if (!file_handle) {
		pr_err("%s: file_handle is NULL\n", __func__);
		return -EINVAL;
	}

	ipu_dev = file_handle->ipu_dev;

	if (!ipu_dev) {
		pr_err("%s: ipu_dev is NULL\n", __func__);
		return -EINVAL;
	}

	miscdev = &ipu_dev->miscdev;

	if (atomic_read(&ipu_dev->dev_removed)) {
		pr_err("%s: device is removed\n", __func__);
		return -ENODEV;
	}

	if (_IOC_TYPE(cmd) != XH2A_IPU_IOCTL_MAGIC) {
		dev_err(miscdev->this_device, "%s: ioctl type dismatch\n",
			__func__);
		return -ENOTTY;
	}

	cmd_size = _IOC_SIZE(cmd);

	if (atomic_read(&ipu_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: device try enter sleep, wait...\n", __func__);

		ret = wait_event_interruptible(
			ipu_dev->block_ioctl_wq,
			atomic_read(&ipu_dev->block_ioctl_flag) == 0);

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

	if (atomic_read(&file_handle->drop_flag)) {
		dev_dbg(miscdev->this_device, "%s: this fd is dropped\n",
			__func__);
		return -EBADF;
	}

	pm_runtime_get_sync(miscdev->parent);
	switch (cmd) {
	case IOCTL_XH2A_IPU_DESTROY_GROUP: {
		dev_dbg(miscdev->this_device,
			"%s: IOCTL_XH2A_IPU_DESTROY_GROUP\n", __func__);

		if ((cmd_size != sizeof(group_id_t))) {
			dev_err(miscdev->this_device,
				"%s at line:%d, Invalid IOCTL size\n", __func__,
				__LINE__);

			ret = -EINVAL;
			break;
		}

		ret = xh2a_destroy_group(filp, (group_id_t __user *)arg);

		if (ret)
			dev_err(miscdev->this_device,
				"%s: xh2a_destroy_group ioctl failed\n",
				__func__);

		break;
	}

	case IOCTL_XH2A_IPU_CREATE_GROUP: {
		dev_dbg(miscdev->this_device,
			"%s: IOCTL_XH2A_IPU_CREATE_GROUP\n", __func__);

		if ((cmd_size != sizeof(group_id_t))) {
			dev_err(miscdev->this_device,
				"%s at line:%d, Invalid IOCTL size\n", __func__,
				__LINE__);

			ret = -EINVAL;
			break;
		}

		ret = xh2a_create_group(filp, (group_id_t __user *)arg);

		if (ret)
			dev_err(miscdev->this_device,
				"%s: xh2a_create_group ioctl failed\n",
				__func__);

		break;
	}

	case IOCTL_XH2A_IPU_LAUNCH_KERNEL: {
		dev_dbg(miscdev->this_device,
			"%s: IOCTL_XH2A_IPU_LAUNCH_KERNEL\n", __func__);

		if ((cmd_size != sizeof(struct xh2a_group_ioctl_cmd))) {
			dev_err(miscdev->this_device,
				"%s at line:%d, Invalid IOCTL size\n", __func__,
				__LINE__);

			ret = -EINVAL;
			break;
		}

		ret = xh2a_launch_kernel_to_group(
			filp, (struct xh2a_group_ioctl_cmd __user *)arg);

		if (ret)
			dev_err(miscdev->this_device,
				"%s: xh2a_launch_kernel_to_group ioctl"
				" failed\n",
				__func__);

		break;
	}

	case IOCTL_XH2A_IPU_EXECUTE_GROUP: {
		dev_dbg(miscdev->this_device,
			"%s: IOCTL_XH2A_IPU_EXECUTE_GROUP\n", __func__);

		if (cmd_size != sizeof(struct xh2a_group_ioctl_cmd)) {
			dev_err(miscdev->this_device,
				"%s at line:%d, Invalid IOCTL size\n", __func__,
				__LINE__);

			ret = -EINVAL;
			break;
		}

		ret = xh2a_execute_group(
			filp, (struct xh2a_group_ioctl_cmd __user *)arg);

		if (ret)
			dev_err(miscdev->this_device,
				"%s: xh2a_execute_group ioctl"
				" failed\n",
				__func__);

		break;
	}

	case IOCTL_XH2A_IPU_GET_LOADAVG: {
		dev_dbg(miscdev->this_device,
			"%s: IOCTL_XH2A_IPU_GET_LOADAVG\n", __func__);

		if (cmd_size != sizeof(uint32_t)) {
			dev_err(miscdev->this_device,
				"%s at line:%d, Invalid IOCTL size\n", __func__,
				__LINE__);

			ret = -EINVAL;
			break;
		}

		ret = xh2a_ipu_get_load(filp, (uint32_t __user *)arg);
		if (ret)
			dev_err(miscdev->this_device,
				"%s: xh2a_ipu_get_load ioctl"
				" failed\n",
				__func__);

		break;
	}

	case IOCTL_XH2A_IPU_DEVICE_RESET: {
		dev_dbg(miscdev->this_device,
			"%s: IOCTL_XH2A_IPU_DEVICE_RESET\n", __func__);

		ret = mutex_lock_interruptible(&ipu_dev->dev_mutex);

		if (ret != 0) {
			dev_err(miscdev->this_device,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			break;
		}

		if (kref_read(&ipu_dev->dev_refcnt) > 2) {
			/* do not reset IPU while other fd in use. */
			ret = -EBUSY;
			dev_err(miscdev->this_device,
				"%s: ipu device is busy\n", __func__);
			mutex_unlock(&ipu_dev->dev_mutex);
			break;
		}

		mutex_unlock(&ipu_dev->dev_mutex);

		if (ipu_dev->group_work_inited)
			cancel_work_sync(&ipu_dev->group_sche_work);

		xh2a_ipu_load_stop(ipu_dev);
		xh2a_ipu_hw_shutdown(ipu_dev);

		mutex_lock(&ipu_dev->dev_mutex);
		old = ipu_dev->policy;
		ipu_dev->policy = NULL;
		mutex_unlock(&ipu_dev->dev_mutex);

		if (old)
			xh2a_ipu_policy_destroy(old);

		ipu_dev->policy = xh2a_ipu_policy_create(ipu_dev);

		xh2a_ipu_hw_startup(ipu_dev);
		xh2a_ipu_load_start(ipu_dev);

		ret = mutex_lock_interruptible(&ipu_dev->dev_mutex);

		if (ret != 0) {
			dev_err(miscdev->this_device,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			break;
		}

		mutex_unlock(&ipu_dev->dev_mutex);

		break;
	}

	default:
		ret = -ENOTTY;
		break;
	}

	pm_runtime_mark_last_busy(miscdev->parent);
	pm_runtime_put_sync(miscdev->parent);

	if ((ret != 0) && atomic_read(&ipu_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: ioctl called in sleep process.\n", __func__);
		/* s2idle keeps devices' DDR, retry blocked ioctl; otherwise
		 * drop it. */
		if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
			ret = -EAGAIN;
		else
			ret = -EINTR;
	}

	return ret;
}
