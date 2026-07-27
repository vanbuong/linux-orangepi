// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#include <linux/list.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include "xh2a_rpmsg_core.h"
#include "xh2a_rpmsg_lsm.h"

#define XH2A_RPMSG_LINK_NAME "xh2a_rpmsg_device"

#define XH2A_RPMSG_ADDR_SPACE_RESERVED (1024)

/**
 * struct xh2a_rpmsg_link_idr_ctx
 * @ept: pointer to ept
 */
struct xh2a_rpmsg_link_idr_ctx {
	struct xh2a_rpmsg_ept *ept;
};

/**
 * struct xh2a_rpmsg_msg - context of the received message
 * @node: link to msg queue
 * @addr: source address of the message
 * @len: message length
 * @data: message data
 */
struct xh2a_rpmsg_msg {
	struct list_head node;
	uint32_t addr;
	size_t len;
	char data[XH2A_RPMSG_BUFFER_MAX_SIZE];
};

/**
 * struct xh2a_rpmsg_ept_ctx - context for each open file
 * @link: link associated with the open file
 * @ept_lock: synchronization of @ept modifications
 * @ept: ept pointer
 * @ept_mtu: maximum transmission buffer size for the ept
 * @queue_lock: synchronization of @queue modifications
 * @queue: incoming message queue
 * @waitq: wait object for incoming queue
 */
struct xh2a_rpmsg_ept_ctx {
	struct list_head node;
	struct xh2a_rpmsg_link *link;

	struct mutex ept_lock;
	struct xh2a_rpmsg_ept *ept;

	size_t ept_mtu;

	struct mutex queue_lock;
	struct list_head queue;

	wait_queue_head_t waitq;
};

/* TODO: limits queue length */
static int xh2a_rpmsg_ept_default_cb(struct xh2a_rpmsg_ept *ept, void *buf,
				     int len, void *priv, uint32_t addr)
{
	struct xh2a_rpmsg_msg *msg;
	struct xh2a_rpmsg_ept_ctx *ept_ctx = priv;
	struct device *dev = ept_ctx->link->miscdev.this_device;

	msg = kzalloc(sizeof(struct xh2a_rpmsg_msg), GFP_KERNEL);
	if (!msg) {
		dev_err(dev, "Failed to allocate memory for msg\n");
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&msg->node);

	msg->addr = addr;
	msg->len = len;
	memcpy(msg->data, buf, len);

	mutex_lock(&ept_ctx->queue_lock);
	list_add_tail(&msg->node, &ept_ctx->queue);
	mutex_unlock(&ept_ctx->queue_lock);

	wake_up_interruptible(&ept_ctx->waitq);

	return 0;
}

static int xh2a_rpmsg_open(struct inode *inode, struct file *filp)
{
	struct xh2a_rpmsg_link *link;
	struct xh2a_rpmsg_ept_ctx *ept_ctx;
	struct miscdevice *miscdev = filp->private_data;

	link = container_of(miscdev, struct xh2a_rpmsg_link, miscdev);

	if (link->ops->device_initialized(link) == 0) {
		pr_err("%s: device not initialized\n", __func__);
		return -EAGAIN;
	}

	if (link->ops->device_removed(link)) {
		pr_err("%s: device removed\n", __func__);
		return -ENODEV;
	}

	ept_ctx = kzalloc(sizeof(struct xh2a_rpmsg_ept_ctx), GFP_KERNEL);
	if (!ept_ctx) {
		dev_err(miscdev->this_device, "Failed to allocate memory for "
					      "ept_ctx\n");
		return -ENOMEM;
	}

	ept_ctx->link = link;

	mutex_init(&ept_ctx->ept_lock);
	mutex_init(&ept_ctx->queue_lock);
	INIT_LIST_HEAD(&ept_ctx->queue);
	init_waitqueue_head(&ept_ctx->waitq);
	INIT_LIST_HEAD(&ept_ctx->node);

	if (link->ops->increase_refcount(link) != 0) {
		kfree(ept_ctx);
		return -EBUSY;
	}

	mutex_lock(&link->ept_ctx_lock);
	list_add_tail(&ept_ctx->node, &link->ept_ctx_list);
	mutex_unlock(&link->ept_ctx_lock);

	filp->private_data = ept_ctx;

	return 0;
}

static int xh2a_rpmsg_clean_ept_ctx(struct xh2a_rpmsg_ept_ctx *ept_ctx)
{
	struct xh2a_rpmsg_msg *msg, *n;

	mutex_lock(&ept_ctx->ept_lock);

	if (!ept_ctx->ept) {
		mutex_unlock(&ept_ctx->ept_lock);
		return -EPIPE;
	}

	xh2a_rpmsg_destroy_ept(ept_ctx->ept);

	ept_ctx->ept = NULL;

	mutex_unlock(&ept_ctx->ept_lock);

	mutex_lock(&ept_ctx->queue_lock);

	list_for_each_entry_safe(msg, n, &ept_ctx->queue, node) {
		list_del(&msg->node);
		kfree(msg);
	}

	mutex_unlock(&ept_ctx->queue_lock);

	return 0;
}

static int xh2a_rpmsg_release(struct inode *inode, struct file *filp)
{
	int ret;
	struct xh2a_rpmsg_ept_ctx *ept_ctx = filp->private_data;
	struct device *dev = ept_ctx->link->miscdev.this_device;

	filp->private_data = NULL;
	ept_ctx->link->ops->decrease_refcount(ept_ctx->link);

	ret = xh2a_rpmsg_clean_ept_ctx(ept_ctx);
	if (ret && ret != -EPIPE) {
		dev_err(dev, "Clean ept ctx failed\n");
		return ret;
	}

	mutex_lock(&ept_ctx->link->ept_ctx_lock);
	list_del(&ept_ctx->node);
	mutex_unlock(&ept_ctx->link->ept_ctx_lock);

	kfree(ept_ctx);
	return 0;
}

struct xh2a_rpmsg_ept *
xh2a_rpmsg_create_ept(struct xh2a_rpmsg_link *link, xh2a_rpmsg_rx_cb_t cb,
		      void *priv, struct xh2a_rpmsg_ept_info *ept_info)
{
	int id_min, id_max, id;
	struct xh2a_rpmsg_ept *ept;
	struct xh2a_rpmsg_ept_info new_info;
	struct xh2a_rpmsg_link_idr_ctx *idr_ctx;
	struct device *dev = link->miscdev.this_device;

	idr_ctx = kzalloc(sizeof(struct xh2a_rpmsg_link_idr_ctx), GFP_KERNEL);
	if (!idr_ctx) {
		dev_err(dev, "Failed to allocate memory for idr ctx");
		return NULL;
	}

	if (ept_info->addr == XH2A_RPMSG_ADDRESS_ANY) {
		id_min = XH2A_RPMSG_ADDR_SPACE_RESERVED;
		id_max = 0;
	} else {
		id_min = ept_info->addr;
		id_max = ept_info->addr + 1;
	}

	mutex_lock(&link->endpoints_lock);
	id = idr_alloc(&link->endpoints, idr_ctx, id_min, id_max, GFP_KERNEL);
	mutex_unlock(&link->endpoints_lock);

	if (id < 0) {
		dev_err(dev, "Failed to allocate idr: %d\n", id);
		goto free_id;
	}

	new_info.addr = id;
	memcpy(new_info.name, ept_info->name, XH2A_RPMSG_EPT_NAME_LENGTH);

	ept = link->ops->create_ept(link, cb, priv, &new_info);
	if (!ept) {
		dev_err(dev, "Create ept failed\n");
		goto free_id;
	}

	return ept;

free_id:
	mutex_lock(&link->endpoints_lock);
	idr_ctx = idr_remove(&link->endpoints, id);
	mutex_unlock(&link->endpoints_lock);

	kfree(idr_ctx);

	return NULL;
}

void xh2a_rpmsg_destroy_ept(struct xh2a_rpmsg_ept *ept)
{
	uint32_t addr;
	struct xh2a_rpmsg_link_idr_ctx *idr_ctx;
	struct xh2a_rpmsg_link *link = ept->link;

	addr = ept->addr;

	ept->ops->destroy_ept(ept);

	mutex_lock(&link->endpoints_lock);
	idr_ctx = idr_remove(&link->endpoints, addr);
	mutex_unlock(&link->endpoints_lock);

	kfree(idr_ctx);
}

int xh2a_rpmsg_send_kern(struct xh2a_rpmsg_ept *ept, uint32_t dst, void *data,
			 int len)
{
	struct device *dev = ept->link->miscdev.this_device;

	if (!xh2a_rpmsg_can_send(ept->link)) {
		dev_err(dev, "Link is not ready for sending\n");
		return -EPIPE;
	}

	return ept->ops->send_kern(ept, dst, data, len);
}

int xh2a_rpmsg_send_user(struct xh2a_rpmsg_ept *ept, uint32_t dst,
			 void __user *data, int len)
{
	struct device *dev = ept->link->miscdev.this_device;

	if (!xh2a_rpmsg_can_send(ept->link)) {
		dev_err(dev, "Link is not ready for sending\n");
		return -EPIPE;
	}

	return ept->ops->send_user(ept, dst, data, len);
}

static int xh2a_rpmsg_ioctl_create_ept(struct xh2a_rpmsg_ept_ctx *ept_ctx,
				       unsigned long arg)
{
	int ret;
	struct xh2a_rpmsg_ept *ept;
	struct xh2a_rpmsg_ept_info ept_info;
	struct device *dev = ept_ctx->link->miscdev.this_device;

	if (ept_ctx->ept) {
		dev_err(dev, "Ept has been created\n");
		return -EBUSY;
	}

	ret = copy_from_user(&ept_info, (void __user *)arg, sizeof(ept_info));
	if (ret) {
		dev_err(dev, "Copy ept_info from user failed\n");
		return -EFAULT;
	}

	ept = xh2a_rpmsg_create_ept(ept_ctx->link, xh2a_rpmsg_ept_default_cb,
				    ept_ctx, &ept_info);
	if (!ept) {
		dev_err(dev, "Create ept failed\n");
		return -ENOMEM;
	}

	mutex_lock(&ept_ctx->ept_lock);

	if (ept_ctx->ept) {
		dev_err(dev, "Ept has been created\n");
		mutex_unlock(&ept_ctx->ept_lock);
		return -EBUSY;
	}

	ept_ctx->ept_mtu = xh2a_rpmsg_get_mtu(ept);

	ept_ctx->ept = ept;

	mutex_unlock(&ept_ctx->ept_lock);

	ept_info.addr = ept->addr;

	ret = copy_to_user((void __user *)arg, &ept_info, sizeof(ept_info));
	if (ret) {
		dev_err(dev, "Copy ept_info to user failed\n");
		ret = -EFAULT;
		goto destroy_ept;
	}

	return 0;

destroy_ept:
	xh2a_rpmsg_destroy_ept(ept);

	return ret;
}

static int xh2a_rpmsg_ioctl_destroy_ept(struct xh2a_rpmsg_ept_ctx *ept_ctx)
{
	int ret;

	ret = xh2a_rpmsg_clean_ept_ctx(ept_ctx);

	wake_up_interruptible(&ept_ctx->waitq);

	return ret;
}

static int xh2a_rpmsg_ioctl_send(struct xh2a_rpmsg_ept_ctx *ept_ctx,
				 unsigned long arg)
{
	int ret;
	struct xh2a_rpmsg_xmit_info xmit_info;
	struct xh2a_rpmsg_link *link = ept_ctx->link;
	struct device *dev = link->miscdev.this_device;

	if (!ept_ctx->ept) {
		dev_err(dev, "Ept has not been created\n");
		return -EPIPE;
	}

	ret = copy_from_user(&xmit_info, (void __user *)arg, sizeof(xmit_info));
	if (ret) {
		dev_err(dev, "Copy xmit_info from user failed\n");
		return -EFAULT;
	}

	if (xmit_info.buf == NULL) {
		dev_err(dev, "Invalid xmit_info: NULL buf\n");
		return -EINVAL;
	}

	if (xmit_info.size > ept_ctx->ept_mtu) {
		dev_err(dev, "Invalid xmit_info: size too large\n");
		return -EINVAL;
	}

	mutex_lock(&ept_ctx->ept_lock);

	if (!ept_ctx->ept) {
		dev_err(dev, "Ept has not been created\n");
		mutex_unlock(&ept_ctx->ept_lock);
		return -EPIPE;
	}

	/* TODO: check @dst exists */

	ret = xh2a_rpmsg_send_user(ept_ctx->ept, xmit_info.addr, xmit_info.buf,
				   xmit_info.size);

	mutex_unlock(&ept_ctx->ept_lock);

	return ret;
}

static int xh2a_rpmsg_ioctl_recv(struct xh2a_rpmsg_ept_ctx *ept_ctx,
				 unsigned long arg)
{
	int ret;
	size_t size;
	long timeout;
	struct xh2a_rpmsg_msg *msg = NULL;
	struct xh2a_rpmsg_xmit_info xmit_info;
	struct xh2a_rpmsg_link *link = ept_ctx->link;
	struct device *dev = link->miscdev.this_device;

	if (!ept_ctx->ept) {
		dev_err(dev, "Ept has not been created\n");
		return -EPIPE;
	}

	ret = copy_from_user(&xmit_info, (void __user *)arg, sizeof(xmit_info));
	if (ret) {
		dev_err(dev, "Copy xmit_info from user failed\n");
		return -EFAULT;
	}

	if (xmit_info.buf == NULL) {
		dev_err(dev, "Invalid xmit_info: NULL buf\n");
		return -EINVAL;
	}

	mutex_lock(&ept_ctx->queue_lock);

	if (list_empty(&ept_ctx->queue)) {
		mutex_unlock(&ept_ctx->queue_lock);

		if (!xmit_info.timeout) {
			dev_err(dev, "No available message\n");
			return -EAGAIN;
		}

		timeout = wait_event_interruptible_timeout(
			ept_ctx->waitq,
			!list_empty(&ept_ctx->queue) || !ept_ctx->ept,
			msecs_to_jiffies(xmit_info.timeout));

		if (ept_ctx->link->ops->device_removed(ept_ctx->link)) {
			pr_err("%s: device removed\n", __func__);
			return -ENODEV;
		}
		if (!timeout) {
			dev_err(dev, "No available message\n");
			return -ETIMEDOUT;
		} else if (timeout == -ERESTARTSYS) {
			dev_err(dev, "Interrupted\n");
			return -EINTR;
		}

		if (!ept_ctx->ept) {
			dev_err(dev, "Ept has not been created\n");
			return -EPIPE;
		}

		mutex_lock(&ept_ctx->queue_lock);
	}

	if (!list_empty(&ept_ctx->queue)) {
		msg = list_first_entry(&ept_ctx->queue, struct xh2a_rpmsg_msg,
				       node);
		list_del(&msg->node);
	}

	mutex_unlock(&ept_ctx->queue_lock);

	if (!msg) {
		dev_err(dev, "No available message\n");
		return -EFAULT;
	}

	size = min_t(size_t, xmit_info.size, msg->len);
	ret = copy_to_user((void __user *)xmit_info.buf, msg->data, size);
	if (ret) {
		dev_err(dev, "Copy data to user failed\n");
		ret = -EFAULT;
		goto exit;
	}

	xmit_info.size = size;
	xmit_info.addr = msg->addr;
	ret = copy_to_user((void __user *)arg, &xmit_info, sizeof(xmit_info));
	if (ret) {
		dev_err(dev, "Copy xmit_info to user failed\n");
		ret = -EFAULT;
	}

exit:
	kfree(msg);

	return ret;
}

static long xh2a_rpmsg_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	int ret;
	struct xh2a_rpmsg_ept_ctx *ept_ctx = filp->private_data;
	struct device *dev = ept_ctx->link->miscdev.this_device;

	if (!ept_ctx) {
		pr_err("%s: invalid ept_ctx\n", __func__);
		return -EINVAL;
	}

	if (ept_ctx->link->ops->device_removed(ept_ctx->link)) {
		pr_err("%s: device removed\n", __func__);
		return -ENODEV;
	}

	pm_runtime_get_sync(ept_ctx->link->miscdev.parent);
	switch (cmd) {
	case IOCTL_XH2A_RPMSG_CREATE_EPT:
		ret = xh2a_rpmsg_ioctl_create_ept(ept_ctx, arg);
		break;

	case IOCTL_XH2A_RPMSG_DESTROY_EPT:
		ret = xh2a_rpmsg_ioctl_destroy_ept(ept_ctx);
		break;

	case IOCTL_XH2A_RPMSG_SEND:
		ret = xh2a_rpmsg_ioctl_send(ept_ctx, arg);
		break;

	case IOCTL_XH2A_RPMSG_RECV:
		ret = xh2a_rpmsg_ioctl_recv(ept_ctx, arg);
		break;

	default:
		dev_err(dev, "unknown cmd 0x%x\n", cmd);
		ret = -EFAULT;
		break;
	}

	pm_runtime_mark_last_busy(ept_ctx->link->miscdev.parent);
	pm_runtime_put_sync(ept_ctx->link->miscdev.parent);

	return ret;
}

static __poll_t xh2a_rpmsg_poll(struct file *filp, poll_table *wait)
{
	__poll_t mask = 0;
	struct xh2a_rpmsg_ept_ctx *ept_ctx = filp->private_data;
	struct device *dev = ept_ctx->link->miscdev.this_device;

	if (!ept_ctx->ept) {
		dev_err(dev, "Ept has not been created\n");
		return EPOLLERR;
	}

	poll_wait(filp, &ept_ctx->waitq, wait);

	if (!list_empty(&ept_ctx->queue))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

static const struct file_operations xh2a_rpmsg_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_rpmsg_open,
	.release = xh2a_rpmsg_release,
	.poll = xh2a_rpmsg_poll,
	.unlocked_ioctl = xh2a_rpmsg_ioctl,
};

int xh2a_rpmsg_link_register(struct xh2a_rpmsg_link *link, int parent_index,
			     struct device *parent_dev,
			     xh2a_rpmsg_link_cb_t link_up_cb,
			     xh2a_rpmsg_link_cb_t link_down_cb)
{
	int ret;
	struct device *dev = link->miscdev.this_device;

	idr_init(&link->endpoints);
	mutex_init(&link->endpoints_lock);
	INIT_LIST_HEAD(&link->ept_ctx_list);
	mutex_init(&link->ept_ctx_lock);

	snprintf(link->name, XH2A_RPMSG_LINK_NAME_LEN,
		 XH2A_RPMSG_LINK_NAME "%d_%u", parent_index, link->link_id);

	link->miscdev.minor = MISC_DYNAMIC_MINOR;
	link->miscdev.name = link->name;
	link->miscdev.fops = &xh2a_rpmsg_fops;
	link->miscdev.mode = 0666;
	link->miscdev.parent = parent_dev;

	ret = misc_register(&link->miscdev);
	if (ret) {
		dev_err(dev, "Failed to register xh2a rpmsg misc device\n");
		return ret;
	}

	ret = xh2a_rpmsg_lsm_init(link, link_up_cb, link_down_cb);
	if (ret) {
		dev_err(dev, "Failed to initialize xh2a rpmsg LSM\n");
		goto unregister;
	}

	return 0;

unregister:
	misc_deregister(&link->miscdev);

	return ret;
}

void xh2a_rpmsg_link_unregister(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_ept_ctx *ept_ctx, *ept_ctx_n;

	mutex_lock(&link->ept_ctx_lock);
	list_for_each_entry_safe(ept_ctx, ept_ctx_n, &link->ept_ctx_list,
				 node) {
		list_del_init(&ept_ctx->node);
		xh2a_rpmsg_clean_ept_ctx(ept_ctx);
		wake_up_interruptible(&ept_ctx->waitq);
	}
	mutex_unlock(&link->ept_ctx_lock);

	xh2a_rpmsg_link_down(link);

	/* At this point, we assume that no further messages will arrive. */

	xh2a_rpmsg_lsm_exit(link);

	misc_deregister(&link->miscdev);

	return;
}
