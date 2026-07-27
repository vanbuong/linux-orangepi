// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/fs.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/types.h>
#include <linux/sched.h>

#include <xh2a_ctl_internal.h>
#include "xh2a_ctl.h"
#include "xh2a_ctl_dev.h"

static void xh2a_ctl_free_dev_ctx(struct kref *kref)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx =
		container_of(kref, struct xh2a_ctl_dev_ctx, kref);

	kfree(ctl_ctx);
}

void xh2a_ctl_ctx_get(struct xh2a_ctl_dev_ctx *ctl_ctx)
{
	kref_get(&ctl_ctx->kref);
}

void xh2a_ctl_ctx_put(struct xh2a_ctl_dev_ctx *ctl_ctx)
{
	kref_put(&ctl_ctx->kref, xh2a_ctl_free_dev_ctx);
}

static int xh2a_ctl_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *mdev = filp->private_data;
	struct xh2a_ctl_ctx *ctl =
		container_of(mdev, struct xh2a_ctl_ctx, miscdev);

	dev_dbg(ctl->miscdev.this_device, "%s: open\n", __func__);

	filp->private_data = ctl;

	return 0;
}

static int xh2a_ctl_release(struct inode *inode, struct file *filp)
{
	struct xh2a_ctl_ctx *ctl = filp->private_data;
	pid_t tgid;
	struct device *ctldev;

	if (!ctl) {
		filp->private_data = NULL;
		return 0;
	}

	tgid = task_tgid_vnr(current);
	ctldev = ctl->miscdev.this_device;

	dev_dbg(ctldev, "%s: release tgid=%d\n", __func__, tgid);

	/* Unlock all device locks owned by current TGID */
	xh2a_ctl_release_locks(ctl, tgid);

	filp->private_data = NULL;

	return 0;
}

int xh2a_ctl_register_dev(struct xh2a_ctl_ctx *ctl, uint32_t dev_id,
			  void *handle)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx, *tmp;
	struct xh2a_pcie_dev *pcie_dev = (struct xh2a_pcie_dev *)handle;
	struct device *ctldev;

	if (!ctl || dev_id >= XH2A_MAX_PCIE_CARDS || !handle)
		return -EINVAL;

	ctldev = ctl->miscdev.this_device;

	dev_dbg(ctldev, "%s: register dev_id=%u pcie_dev=%p\n", __func__,
		dev_id, pcie_dev);

	/* prevent duplicate device registration - keep mutex locked during
	 * entire operation */
	mutex_lock(&ctl->lock);
	list_for_each_entry(tmp, &ctl->ctl_list, node) {
		if (tmp->dev_id == dev_id) {
			mutex_unlock(&ctl->lock);
			return -EEXIST;
		}
	}

	ctl_ctx = xh2a_ctl_create_lock(ctl, dev_id, handle);
	if (!ctl_ctx) {
		mutex_unlock(&ctl->lock);
		return -ENOMEM;
	}

	list_add_tail(&ctl_ctx->node, &ctl->ctl_list);
	mutex_unlock(&ctl->lock);

	dev_dbg(ctldev, "%s: dev_id=%u registered, ctx=%p\n", __func__, dev_id,
		ctl_ctx);

	wake_up_all(&ctl_ctx->wq);

	return 0;
}

void xh2a_ctl_unregister_dev(struct xh2a_ctl_ctx *ctl, uint32_t dev_id)
{
	if (!ctl || dev_id >= XH2A_MAX_PCIE_CARDS)
		return;

	xh2a_ctl_destroy_lock(ctl, dev_id);
}

static long xh2a_ctl_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct xh2a_ctl_lock_req req;
	struct xh2a_ctl_ctx *ctl = filp->private_data;
	struct device *ctldev;

	if (!ctl)
		return -EINVAL;

	ctldev = ctl->miscdev.this_device;

	switch (cmd) {
	case IOCTL_XH2A_CTL_LOCK_DEV:
	case IOCTL_XH2A_CTL_UNLOCK_DEV:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;

		if (ctldev)
			dev_dbg(ctldev,
				"%s: ioctl cmd=0x%x devmask=0x%llx flags=0x%x "
				"timeout_ms=%u\n",
				__func__, cmd, (unsigned long long)req.devmask,
				req.flags, req.timeout_ms);

		if (cmd == IOCTL_XH2A_CTL_LOCK_DEV)
			return xh2a_ctl_lock_dev(filp, &req);
		else
			return xh2a_ctl_unlock_dev(filp, &req);
	default:
		if (ctldev)
			dev_dbg(ctldev, "%s: unsupported ioctl cmd=0x%x\n",
				__func__, cmd);
		return -ENOTTY;
	}
}

static const struct file_operations xh2a_ctl_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_ctl_open,
	.release = xh2a_ctl_release,
	.unlocked_ioctl = xh2a_ctl_ioctl,
};

int xh2a_ctl_init(struct xh2a_ctl_ctx **ctl_out)
{
	int ret;
	struct xh2a_ctl_ctx *ctl;

	if (!ctl_out)
		return -EINVAL;

	ctl = kzalloc(sizeof(*ctl), GFP_KERNEL);
	if (!ctl)
		return -ENOMEM;

	mutex_init(&ctl->lock);
	INIT_LIST_HEAD(&ctl->ctl_list);

	ctl->miscdev.minor = MISC_DYNAMIC_MINOR;
	ctl->miscdev.name = "xh2a_ctl0";
	ctl->miscdev.fops = &xh2a_ctl_fops;
	ctl->miscdev.mode = 0666;

	ret = misc_register(&ctl->miscdev);
	if (ret) {
		kfree(ctl);
		return ret;
	}

	dev_dbg(ctl->miscdev.this_device, "%s: ctl created dev=%s\n", __func__,
		dev_name(ctl->miscdev.this_device));

	*ctl_out = ctl;

	return 0;
}

void xh2a_ctl_exit(struct xh2a_ctl_ctx *ctl)
{
	if (!ctl)
		return;

	dev_dbg(ctl->miscdev.this_device, "%s: ctl exit\n", __func__);

	misc_deregister(&ctl->miscdev);

	xh2a_ctl_cleanup_all_locks(ctl);

	kfree(ctl);
}

MODULE_LICENSE("GPL");
