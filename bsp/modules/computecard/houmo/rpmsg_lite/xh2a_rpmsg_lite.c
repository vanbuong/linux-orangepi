// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <xh2a_address.h>

#include "xh2a_rpmsg_lite.h"
#include "xh2a_rpmsg_lite_ext.h"

/*
 * Host notify Device(U7) by ELBI, use Xh2aPcieWriteBarMsgBit() provided by
 * PCIe-driver instead of touching the ELBI register directly
 */
#define PCIE_ELBI_MSG_SET_OFS	  (0x60280000 + 0x0FD0)
#define PCIE_ELBI_MSG_CLR_OFS	  (0x60280000 + 0x0FD4)
#define PCIE_ELBI_MSG_MASK_U7_OFS (0x60280000 + 0x0FD8)
#define PCIE_ELBI_MSG_MASK_E2_OFS (0x60280000 + 0x0FDC)

/* E2 uses low 16 bits and U7 uses high 16 bits */
#define PCIE_ELBI_MSG_U7_SHIFT	 (16)
#define PCIE_ELBI_MSG_U7_BITMASK (0xFFFF0000)

#define HOST_RVQ_MSG_BIT	  (0)
#define HOST_TVQ_MSG_BIT	  (1)
#define HOST_RVQ_MSG		  BIT(HOST_RVQ_MSG_BIT)
#define HOST_TVQ_MSG		  BIT(HOST_TVQ_MSG_BIT)
#define HOST_LINK_SHIFT(_link_id) ((_link_id) << 1)
#define HOST_LINK_MSG_MASK(_link_id) \
	((HOST_RVQ_MSG | HOST_TVQ_MSG) << HOST_LINK_SHIFT(_link_id))

/*  Device(U7) notify Host by MSI-TRIGGER */
#define MSI_MASKN_OFS	     (0x48000000 + 0x80)
#define MSI_VLD_OFS	     (0x48000000 + 0x84)
#define MSI_TRIGGER_SET_OFS  (0x48000000 + 0x90)
#define MSI_TRIGGER_CLR_OFS  (0x48000000 + 0x94)
#define MSI_TRIGGER_MASK_OFS (0x48000000 + 0x98)
#define MSI_TRIGGER_VLD_OFS  (0x48000000 + 0x9C)

#define MSI_MASKN_U7_SHIFT (8)

#define DEVICE_RVQ_MSG_BIT	    (1)
#define DEVICE_TVQ_MSG_BIT	    (0)
#define DEVICE_RVQ_MSG		    BIT(DEVICE_RVQ_MSG_BIT)
#define DEVICE_TVQ_MSG		    BIT(DEVICE_TVQ_MSG_BIT)
#define DEVICE_LINK_SHIFT(_link_id) ((_link_id) << 1)
#define DEVICE_LINK_MSG_MASK(_link_id) \
	((DEVICE_RVQ_MSG | DEVICE_TVQ_MSG) << DEVICE_LINK_SHIFT(_link_id))

static
struct xh2a_rpmsg_lite_link_info link_infos[XH2A_RPMSG_LITE_LINK_NUM] = {
	[0] = {
		.shm_start_pa	= XH2A_DEVICE_DDR_RPMSG_START,
		.shm_size	= XH2A_DEVICE_DDR_RPMSG_SIZE,
	},
};

static inline bool
xh2a_rpmsg_lite_verify_addr_range(struct xh2a_rpmsg_lite_dev *rldev,
				  uint64_t addr, uint32_t len)
{
	int link;

	for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++) {
		if (addr < link_infos[link].shm_start_pa ||
		    addr >= link_infos[link].shm_start_pa +
				    link_infos[link].shm_size)
			return false;

		if (len > link_infos[link].shm_size)
			return false;
	}

	return true;
}

static void xh2a_rpmsg_lite_remote_writew(void *handle, uint64_t addr,
					  uint16_t data)
{
	struct xh2a_rpmsg_lite_dev *rldev = handle;

	xh2a_pcie_pio_writew(rldev->private_data, addr, data);
}

static uint16_t xh2a_rpmsg_lite_remote_readw(void *handle, uint64_t addr)
{
	uint16_t data;
	struct xh2a_rpmsg_lite_dev *rldev = handle;

	xh2a_pcie_pio_readw(rldev->private_data, addr, &data);

	return data;
}

static uint64_t xh2a_rpmsg_lite_remote_readq(void *handle, uint64_t addr)
{
	uint64_t data;
	struct xh2a_rpmsg_lite_dev *rldev = handle;

	xh2a_pcie_pio_readq(rldev->private_data, addr, &data);

	return data;
}

#define XH2A_RPMSG_DMA_LEN_THRESHOLD 0xFFFFFFFF

static void __xh2a_rpmsg_lite_memcpy_to_remote(void *handle, uint64_t dst,
					       void *src, uint32_t len)
{
	if (len <= XH2A_RPMSG_DMA_LEN_THRESHOLD)
		xh2a_pcie_pio_write_mem(handle, dst, src, len);
	else
		xh2a_pcie_dma_write_mem(handle, dst, src, len);
}

static void xh2a_rpmsg_lite_memcpy_to_remote(void *handle, uint64_t dst,
					     void *src, uint32_t len)
{
	struct xh2a_rpmsg_lite_dev *rldev = handle;

	if (!xh2a_rpmsg_lite_verify_addr_range(rldev, dst, len)) {
		pr_err("%s: Invalid shmem physical address 0x%llx, len 0x%x\n",
		       __func__, dst, len);
		return;
	}

	__xh2a_rpmsg_lite_memcpy_to_remote(rldev->private_data, dst, src, len);
}

static void xh2a_rpmsg_lite_memcpy_from_remote(void *handle, void *dst,
					       uint64_t src, uint32_t len)
{
	struct xh2a_rpmsg_lite_dev *rldev = handle;

	if (!xh2a_rpmsg_lite_verify_addr_range(rldev, src, len)) {
		pr_err("%s: Invalid shmem physical address 0x%llx, len 0x%x\n",
		       __func__, src, len);
		return;
	}

	if (len <= XH2A_RPMSG_DMA_LEN_THRESHOLD)
		xh2a_pcie_pio_read_mem(rldev->private_data, src, dst, len);
	else
		xh2a_pcie_dma_read_mem(rldev->private_data, src, dst, len);
}

static void xh2a_rpmsg_lite_bind_platform_context(void *handle,
						  void *platform_context)
{
	struct xh2a_rpmsg_lite_dev *rldev = handle;

	rldev->rl_platform_context = platform_context;
}

static void xh2a_rpmsg_lite_notify_device(void *handle, uint32_t link_id,
					  uint32_t queue_id)
{
	uint32_t msgbit;
	struct xh2a_rpmsg_lite_dev *rldev = handle;

	if (link_id > RL_PLATFORM_HIGHEST_LINK_ID || queue_id > 1) {
		pr_err("%s: Invalid link_id or queue_id\n", __func__);
		return;
	}

	msgbit = (link_id << 1) + queue_id;

	xh2a_pcie_write_bar_msgbit(rldev->private_data, msgbit,
				   XH2A_PCIE_MSG_TO_U7);
}

static void xh2a_rpmsg_lite_link_up(void *handle, uint32_t link_id)
{
	int ret = 0;
	struct xh2a_rpmsg_lite_link *rllink;
	struct xh2a_rpmsg_lite_dev *rldev = handle;
	int device_id = xh2a_pcie_device_index(rldev->private_data);

	if (link_id >= XH2A_RPMSG_LITE_LINK_NUM) {
		pr_err("Unexpected link id %d\n", link_id);
		return;
	}

	rllink = &rldev->links[link_id];

	if (rpmsg_lite_is_link_up(rllink->rl_inst)) {
		pr_debug("rpmsg device %d link %d up already\n", device_id,
			 link_id);
		return;
	}

	ret = rpmsg_lite_link_post_init(rllink->rl_inst);
	if (ret) {
		pr_err("RPMsg link post init failed\n");
		return;
	}

	rpmsg_lite_link_up(rllink->rl_inst);
	rpmsg_lite_link_trigger(rllink->rl_inst);

	ret = xh2a_rpmsg_link_up(&rllink->link);
	if (!ret)
		pr_info("rpmsg device %d link %d up\n", device_id, link_id);
}

static struct rpmsg_platform_ops xh2a_rpmsg_lite_platform_ops = {
	.writew_remote = xh2a_rpmsg_lite_remote_writew,
	.readw_remote = xh2a_rpmsg_lite_remote_readw,
	.readq_remote = xh2a_rpmsg_lite_remote_readq,
	.memcpy_to_remote = xh2a_rpmsg_lite_memcpy_to_remote,
	.memcpy_from_remote = xh2a_rpmsg_lite_memcpy_from_remote,
	.bind_platform_context = xh2a_rpmsg_lite_bind_platform_context,
	.notify_device = xh2a_rpmsg_lite_notify_device,
	.link_up = xh2a_rpmsg_lite_link_up,
};

static inline void xh2a_rpmsg_lite_enable_msi(struct xh2a_rpmsg_lite_dev *rldev)
{
	uint32_t val;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_MASKN_OFS, &val);
	val |= XH2A_PCIE_MSI_ID_CPUSS_MASK;
	xh2a_pcie_pio_writel(rldev->private_data, MSI_MASKN_OFS, val);
}

static inline void
xh2a_rpmsg_lite_disable_msi(struct xh2a_rpmsg_lite_dev *rldev)
{
	uint32_t val;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_MASKN_OFS, &val);
	val &= ~XH2A_PCIE_MSI_ID_CPUSS_MASK;
	xh2a_pcie_pio_writel(rldev->private_data, MSI_MASKN_OFS, val);
}

static inline void
xh2a_rpmsg_lite_mask_rx_irq(struct xh2a_rpmsg_lite_dev *rldev, int link_id)
{
	uint32_t val;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);
	val &= ~(BIT(DEVICE_TVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link_id));
	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static inline void
xh2a_rpmsg_lite_unmask_rx_irq(struct xh2a_rpmsg_lite_dev *rldev, int link_id)
{
	uint32_t val;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);
	val |= (BIT(DEVICE_TVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link_id));
	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static inline void
xh2a_rpmsg_lite_mask_tx_irq(struct xh2a_rpmsg_lite_dev *rldev, int link_id)
{
	uint32_t val;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);
	val &= ~(BIT(DEVICE_RVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link_id));
	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static inline void
xh2a_rpmsg_lite_unmask_tx_irq(struct xh2a_rpmsg_lite_dev *rldev, int link_id)
{
	uint32_t val;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);
	val |= (BIT(DEVICE_RVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link_id));
	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static inline void
xh2a_rpmsg_lite_mask_rx_irq_all(struct xh2a_rpmsg_lite_dev *rldev)
{
	int link;
	uint32_t val = 0;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);

	for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++)
		val &= ~(BIT(DEVICE_TVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link));

	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static inline void
xh2a_rpmsg_lite_unmask_rx_irq_all(struct xh2a_rpmsg_lite_dev *rldev)
{
	int link;
	uint32_t val = 0;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);

	for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++)
		val |= (BIT(DEVICE_TVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link));

	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static inline void
xh2a_rpmsg_lite_mask_tx_irq_all(struct xh2a_rpmsg_lite_dev *rldev)
{
	int link;
	uint32_t val = 0;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);

	for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++)
		val &= ~(BIT(DEVICE_RVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link));

	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static inline void
xh2a_rpmsg_lite_unmask_tx_irq_all(struct xh2a_rpmsg_lite_dev *rldev)
{
	int link;
	uint32_t val = 0;

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_MASK_OFS, &val);

	for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++)
		val |= (BIT(DEVICE_RVQ_MSG_BIT) << DEVICE_LINK_SHIFT(link));

	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_MASK_OFS, val);
}

static void xh2a_rpmsg_lite_link_handle(struct xh2a_rpmsg_lite_dev *rldev,
					int link_id, uint32_t msgbits)
{
	struct xh2a_rpmsg_lite_link *rllink;

	if (!msgbits)
		return;

	rllink = &rldev->links[link_id];

	mutex_lock(&rllink->rl_inst_lock);

	if (rllink->rl_inst) {
		/* prioritize link up event handling */
		if (msgbits & BIT(DEVICE_RVQ_MSG_BIT))
			platform_isr(rldev->rl_platform_context, link_id,
				     DEVICE_RVQ_MSG_BIT);

		if (msgbits & BIT(DEVICE_TVQ_MSG_BIT))
			platform_isr(rldev->rl_platform_context, link_id,
				     DEVICE_TVQ_MSG_BIT);
	}

	mutex_unlock(&rllink->rl_inst_lock);
}

static void xh2a_rpmsg_lite_work_handler(struct work_struct *work)
{
	uint32_t link_id;
	uint32_t val;
	uint32_t msgbits, link_msgbits;
	struct xh2a_rpmsg_lite_dev *rldev;

	rldev = container_of(work, struct xh2a_rpmsg_lite_dev, rpmsg_lite_work);

	xh2a_pcie_pio_readl(rldev->private_data, MSI_VLD_OFS, &val);
	if (!(val & BIT(MSI_MASKN_U7_SHIFT)))
		return;

	xh2a_rpmsg_lite_disable_msi(rldev);

	xh2a_pcie_pio_readl(rldev->private_data, MSI_TRIGGER_VLD_OFS, &msgbits);

	/* clear before processing */
	xh2a_pcie_pio_writel(rldev->private_data, MSI_TRIGGER_CLR_OFS, msgbits);

	for (link_id = 0; link_id < XH2A_RPMSG_LITE_LINK_NUM; link_id++) {
		if (!(DEVICE_LINK_MSG_MASK(link_id) & msgbits))
			continue;

		link_msgbits = msgbits >> DEVICE_LINK_SHIFT(link_id);

		xh2a_rpmsg_lite_link_handle(rldev, link_id, link_msgbits);
	}

	xh2a_rpmsg_lite_enable_msi(rldev);
}

static void xh2a_rpmsg_lite_destroy_ept(struct xh2a_rpmsg_ept *ept)
{
	int ret;
	struct xh2a_rpmsg_lite_ept *rlept;
	struct xh2a_rpmsg_lite_link *rllink;

	rlept = container_of(ept, struct xh2a_rpmsg_lite_ept, ept);
	rllink = container_of(ept->link, struct xh2a_rpmsg_lite_link, link);

	/* the destruction operation should never fail, and if it does, simply
	 * show the error
	 */
	ret = rpmsg_lite_destroy_ept(rllink->rl_inst, rlept->rl_ept);
	if (ret)
		pr_err("%s: Destroy rpmsg-lite ept failed\n", __func__);

	/* anyway, free the rlept, caller should never touch anymore */
	kfree(rlept);
}

static int xh2a_rpmsg_lite_send_kern(struct xh2a_rpmsg_ept *ept, uint32_t dst,
				     void *data, int len)
{
	int ret;
	void *txbuf;
	uint32_t txsize;
	struct xh2a_rpmsg_lite_ept *rlept;
	struct xh2a_rpmsg_lite_link *rllink;
	struct xh2a_rpmsg_lite_dev *rldev;

	rllink = container_of(ept->link, struct xh2a_rpmsg_lite_link, link);

	txbuf = rpmsg_lite_alloc_tx_buffer(rllink->rl_inst, &txsize,
					   RL_DONT_BLOCK);
	if (!txbuf) {
		pr_err("%s: Alloc rpmsg-lite tx buffer failed\n", __func__);
		return -ENOMEM;
	}

	if (txsize > len)
		txsize = len;

	rldev = rllink->rldev;
	__xh2a_rpmsg_lite_memcpy_to_remote(rldev->private_data, (uint64_t)txbuf,
					   data, txsize);

	rlept = container_of(ept, struct xh2a_rpmsg_lite_ept, ept);

	ret = rpmsg_lite_send_nocopy(rllink->rl_inst, rlept->rl_ept, dst, txbuf,
				     txsize);
	if (ret) {
		pr_err("%s: Send rpmsg-lite tx buffer failed\n", __func__);
		return -EFAULT;
	}

	return 0;
}

static int xh2a_rpmsg_lite_send_user(struct xh2a_rpmsg_ept *ept, uint32_t dst,
				     void __user *data, int len)
{
	int ret;
	void *txbuf;
	uint32_t txsize;
	char buffer[XH2A_RPMSG_BUFFER_MAX_SIZE] = { 0 };
	struct xh2a_rpmsg_lite_ept *rlept;
	struct xh2a_rpmsg_lite_link *rllink;
	struct xh2a_rpmsg_lite_dev *rldev;

	if (len > XH2A_RPMSG_BUFFER_MAX_SIZE) {
		pr_err("%s: invalid length\n", __func__);
		return -EINVAL;
	}

	ret = copy_from_user(buffer, (void __user *)data, len);
	if (ret) {
		pr_err("%s: Copy send buf from user failed!\n", __func__);
		/* FIXME: to release tx buf?? */
		return -EINVAL;
	}

	rllink = container_of(ept->link, struct xh2a_rpmsg_lite_link, link);
	txbuf = rpmsg_lite_alloc_tx_buffer(rllink->rl_inst, &txsize,
					   RL_DONT_BLOCK);
	if (!txbuf) {
		pr_err("%s: Alloc rpmsg-lite tx buffer failed\n", __func__);
		return -ENOMEM;
	}

	if (txsize > len)
		txsize = len;

	rldev = rllink->rldev;

	if (atomic_read(&rldev->block_ioctl_flag)) {
		dev_err(rllink->link.miscdev.this_device,
			"%s: device try enter sleep, wait...\n", __func__);

		ret = wait_event_interruptible(
			rldev->block_ioctl_wq,
			atomic_read(&rldev->block_ioctl_flag) == 0);

		if (ret < 0) {
			dev_err(rllink->link.miscdev.this_device,
				"%s: wait_event_interruptible interrupted %d\n",
				__func__, ret);
			return -EINTR;
		}
	}

	xh2a_pcie_pio_write_mem(rldev->private_data, (uint64_t)txbuf, buffer,
				txsize);

	rlept = container_of(ept, struct xh2a_rpmsg_lite_ept, ept);

	ret = rpmsg_lite_send_nocopy(rllink->rl_inst, rlept->rl_ept, dst, txbuf,
				     txsize);

	if (ret) {
		pr_err("%s: Send rpmsg-lite tx buffer failed\n", __func__);
		ret = -EFAULT;
	}

	if ((ret != 0) && atomic_read(&rldev->block_ioctl_flag)) {
		dev_err(rllink->link.miscdev.this_device,
			"%s: ioctl in sleep process is interrupted\n",
			__func__);
		ret = -EINTR;
	}

	return ret;
}

static size_t xh2a_rpmsg_lite_get_mtu(struct xh2a_rpmsg_ept *ept)
{
	return RL_BUFFER_PAYLOAD_SIZE;
}

static struct xh2a_rpmsg_ept_ops xh2a_rpmsg_lite_ept_ops = {
	.destroy_ept = xh2a_rpmsg_lite_destroy_ept,
	.send_kern = xh2a_rpmsg_lite_send_kern,
	.send_user = xh2a_rpmsg_lite_send_user,
	.get_mtu = xh2a_rpmsg_lite_get_mtu,
};

/* return RL_RELEASE in any case */
static int xh2a_rpmsg_lite_ept_cb(void *payload, uint32_t payload_len,
				  uint32_t src, void *priv)
{
	int ret = 0;
	struct xh2a_rpmsg_lite_ept *rlept = priv;

	if (rlept->ept.cb)
		ret = rlept->ept.cb(&rlept->ept, payload, payload_len,
				    rlept->ept.priv, src);

	if (ret) {
		pr_err("%s: Invoke ept callback failed\n", __func__);
		return RL_RELEASE;
	}

	return RL_RELEASE;
}

static struct xh2a_rpmsg_ept *
xh2a_rpmsg_lite_create_ept(struct xh2a_rpmsg_link *link, xh2a_rpmsg_rx_cb_t cb,
			   void *priv, struct xh2a_rpmsg_ept_info *ept_info)
{
	struct xh2a_rpmsg_lite_ept *rlept;
	struct xh2a_rpmsg_lite_link *rllink;

	rllink = container_of(link, struct xh2a_rpmsg_lite_link, link);

	rlept = kzalloc(sizeof(struct xh2a_rpmsg_lite_ept), GFP_KERNEL);
	if (!rlept) {
		pr_err("%s: Failed to allocate memory for rlept\n", __func__);
		return NULL;
	}

	rlept->ept.link = link;
	memcpy(rlept->ept.name, ept_info->name, XH2A_RPMSG_EPT_NAME_LENGTH);
	rlept->ept.addr = ept_info->addr;
	rlept->ept.cb = cb;
	rlept->ept.priv = priv;
	rlept->ept.ops = &xh2a_rpmsg_lite_ept_ops;

	rlept->rl_ept = rpmsg_lite_create_ept(rllink->rl_inst, ept_info->addr,
					      xh2a_rpmsg_lite_ept_cb, rlept);
	if (!rlept->rl_ept) {
		pr_err("%s: Create rpmsg-lite ept failed\n", __func__);
		goto free;
	}

	return &rlept->ept;

free:
	kfree(rlept);

	return NULL;
}

static void xh2a_rpmsg_lite_device_safe_release(struct kref *kref)
{
	struct xh2a_rpmsg_lite_dev *rldev =
		container_of(kref, struct xh2a_rpmsg_lite_dev, dev_refcnt);

	kfree(rldev);
}

static int xh2a_rpmsg_lite_increase_refcount(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lite_link *rllink;

	rllink = container_of(link, struct xh2a_rpmsg_lite_link, link);
	if (kref_get_unless_zero(&rllink->rldev->dev_refcnt) == 0) {
		pr_err("%s: device is removing...\n", __func__);
		return -ENODEV;
	}

	return 0;
}

static int xh2a_rpmsg_lite_decrease_refcount(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lite_link *rllink;

	rllink = container_of(link, struct xh2a_rpmsg_lite_link, link);
	kref_put(&rllink->rldev->dev_refcnt,
		 xh2a_rpmsg_lite_device_safe_release);
	return 0;
}

static int xh2a_rpmsg_lite_device_removed(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lite_link *rllink;

	rllink = container_of(link, struct xh2a_rpmsg_lite_link, link);

	return atomic_read(&rllink->rldev->dev_removed);
}

static int xh2a_rpmsg_lite_device_initialized(struct xh2a_rpmsg_link *link)
{
	struct xh2a_rpmsg_lite_link *rllink;

	rllink = container_of(link, struct xh2a_rpmsg_lite_link, link);

	return atomic_read(&rllink->rldev->dev_initialized);
}

static struct xh2a_rpmsg_link_ops xh2a_rpmsg_lite_link_ops = {
	.create_ept = xh2a_rpmsg_lite_create_ept,
	.increase_refcount = xh2a_rpmsg_lite_increase_refcount,
	.decrease_refcount = xh2a_rpmsg_lite_decrease_refcount,
	.device_removed = xh2a_rpmsg_lite_device_removed,
	.device_initialized = xh2a_rpmsg_lite_device_initialized,
};

static int xh2a_rpmsg_lite_link_up_cb(struct xh2a_rpmsg_link *link)
{
	(void)link;

	/* Nothing to do now. */

	return 0;
}

static int xh2a_rpmsg_lite_link_down_cb(struct xh2a_rpmsg_link *link)
{
	int ret;
	struct xh2a_rpmsg_lite_link *rllink;
	int device_id;

	rllink = container_of(link, struct xh2a_rpmsg_lite_link, link);

	mutex_lock(&rllink->rl_inst_lock);

	if (!rllink->rl_inst)
		goto unlock;

	ret = rpmsg_lite_link_reset(rllink->rl_inst);
	if (ret != RL_SUCCESS) {
		pr_err("%s: Failed to reset xh2a rpmsg link\n", __func__);
		return -1;
	}

	device_id = xh2a_pcie_device_index(rllink->rldev->private_data);
	pr_info("rpmsg device %d link %d down\n", device_id,
		rllink->link.link_id);

unlock:
	mutex_unlock(&rllink->rl_inst_lock);

	return 0;
}

static int
xh2a_rpmsg_lite_create_link(struct xh2a_rpmsg_lite_dev *rldev, int link_id,
			    struct xh2a_rpmsg_lite_link_info *link_info)
{
	int ret = 0;
	int parent_index;
	struct device *parent_dev;
	struct xh2a_rpmsg_lite_link *rllink;
	struct rpmsg_lite_instance *rl_inst;

	rpmsg_env_init_t rl_env_cfg = {
		.handle = rldev,
		.platform_cfg =
			&(rpmsg_platform_init_data_t){
				.handle = rldev,
				.ops = &xh2a_rpmsg_lite_platform_ops,
			},
	};

	if (link_id >= XH2A_RPMSG_LITE_LINK_NUM) {
		pr_err("Invalid link id for rpmsg\n");
		return -EINVAL;
	}

	rllink = &rldev->links[link_id];
	rl_inst = rpmsg_lite_master_init((void *)link_info->shm_start_pa,
					 link_info->shm_size,
					 rllink->link.link_id, RL_NO_FLAGS,
					 &rl_env_cfg);

	if (rl_inst == NULL) {
		pr_err("%s: rpmsg_lite master init failed\n", __func__);
		goto free;
	}

	rllink->rldev = rldev;
	rllink->rl_inst = rl_inst;
	mutex_init(&rllink->rl_inst_lock);

	rllink->link.link_id = link_id;
	rllink->link.ops = &xh2a_rpmsg_lite_link_ops;

	parent_index = xh2a_pcie_device_index(rldev->private_data);
	parent_dev = xh2a_pcie_device_ptr(rldev->private_data);
	ret = xh2a_rpmsg_link_register(&rllink->link, parent_index, parent_dev,
				       xh2a_rpmsg_lite_link_up_cb,
				       xh2a_rpmsg_lite_link_down_cb);

	if (ret) {
		pr_err("%s: Failed to register xh2a rpmsg link\n", __func__);
		goto deinit;
	}

	rpmsg_lite_link_trigger(rllink->rl_inst);

	return 0;

deinit:
	rpmsg_lite_deinit(rllink->rl_inst);
	rllink->rl_inst = NULL;
free:

	return ret;
}

static void xh2a_rpmsg_lite_destroy_link(struct xh2a_rpmsg_lite_link *rllink)
{
	xh2a_rpmsg_link_unregister(&rllink->link);

	mutex_lock(&rllink->rl_inst_lock);
	rpmsg_lite_deinit(rllink->rl_inst);
	rllink->rl_inst = NULL;
	mutex_unlock(&rllink->rl_inst_lock);
}

/*
 * xh2a_rpmsg_lite_pm_notifier_prepare()
 *     - before pm prepare the rpmsg_lite device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_rpmsg_lite_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_rpmsg_lite_dev *rldev;

	xh2a_pcie_get_client(handle, &client, XH2A_RPMSG_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	rldev = client->client_data;

	if (rollback) {
		if (atomic_dec_and_test(&rldev->block_ioctl_flag))
			wake_up_interruptible(&rldev->block_ioctl_wq);
	} else {
		atomic_inc(&rldev->block_ioctl_flag);
	}

	return 0;
}

/*
 * xh2a_rpmsg_lite_pm_notifier_complete()
 *     - after pm complete the rpmsg_lite device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_rpmsg_lite_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_rpmsg_lite_dev *rldev;

	xh2a_pcie_get_client(handle, &client, XH2A_RPMSG_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	rldev = client->client_data;

	(void)rldev;

	return 0;
}

/*
 * xh2a_rpmsg_lite_pm_prepare()
 *     - pm prepare the rpmsg_lite device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_rpmsg_lite_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_rpmsg_lite_dev *rldev;
	int link;

	xh2a_pcie_get_client(handle, &client, XH2A_RPMSG_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	rldev = client->client_data;

	/* no longer accept tx irq (link up irq) */
	xh2a_rpmsg_lite_disable_msi(rldev);
	xh2a_rpmsg_lite_mask_tx_irq_all(rldev);
	xh2a_rpmsg_lite_mask_rx_irq_all(rldev);

	if (!is_compatible) {
		for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++) {
			if (rldev->links[link].rl_inst != NULL)
				xh2a_rpmsg_lite_destroy_link(
					&rldev->links[link]);
		}
	}

	cancel_work_sync(&rldev->rpmsg_lite_work);

	return 0;
}

/*
 * xh2a_rpmsg_lite_pm_complete()
 *     - pm complete the rpmsg_lite device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_rpmsg_lite_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_rpmsg_lite_dev *rldev;
	int link;

	xh2a_pcie_get_client(handle, &client, XH2A_RPMSG_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	rldev = client->client_data;

	if (atomic_dec_and_test(&rldev->block_ioctl_flag))
		wake_up_interruptible(&rldev->block_ioctl_wq);

	xh2a_rpmsg_lite_disable_msi(rldev);
	xh2a_rpmsg_lite_mask_tx_irq_all(rldev);
	xh2a_rpmsg_lite_mask_rx_irq_all(rldev);

	xh2a_pcie_pio_writel(handle, MSI_TRIGGER_CLR_OFS, 0x3);

	if (!is_compatible) {
		for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++) {
			if (rldev->links[link].rl_inst != NULL) {
				WARN_ONCE(1,
					  "%s: link %d is not destroyed. CHECK "
					  "device!\n",
					  __func__, link);
				break;
			}
			xh2a_rpmsg_lite_create_link(rldev, link,
						    &link_infos[link]);
		}
	}

	xh2a_rpmsg_lite_enable_msi(rldev);
	xh2a_rpmsg_lite_unmask_tx_irq_all(rldev);
	xh2a_rpmsg_lite_unmask_rx_irq_all(rldev);

	return 0;
}

static int xh2a_rpmsg_lite_probe(void *handle)
{
	int ret, link;
	struct xh2a_pcie_client *client;
	struct xh2a_rpmsg_lite_dev *rldev;

	pr_debug("xh2a rpmsg driver init\n");

	rldev = kzalloc(sizeof(struct xh2a_rpmsg_lite_dev), GFP_KERNEL);
	if (!rldev) {
		pr_err("%s: Failed to allocate memory for rldev\n", __func__);
		return -ENOMEM;
	}

	atomic_set(&rldev->dev_initialized, 0);
	atomic_set(&rldev->dev_removed, 0);
	kref_init(&rldev->dev_refcnt);

	rldev->private_data = handle;

	xh2a_rpmsg_lite_disable_msi(rldev);
	xh2a_rpmsg_lite_mask_tx_irq_all(rldev);
	xh2a_rpmsg_lite_mask_rx_irq_all(rldev);

	xh2a_pcie_pio_writel(handle, MSI_TRIGGER_CLR_OFS, 0x3);

	for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++) {
		ret = xh2a_rpmsg_lite_create_link(rldev, link,
						  &link_infos[link]);
		if (ret) {
			pr_err("%s: Create link %d failed\n", __func__, link);
			goto destroy;
		}
	}

	client = &rldev->client;

	INIT_WORK(&rldev->rpmsg_lite_work, xh2a_rpmsg_lite_work_handler);

	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_RPMSG_DEVICE_NAME);

	client->work[0].msi_id = XH2A_PCIE_MSI_ID_CPUSS;
	client->work[0].msi_work = &rldev->rpmsg_lite_work;
	client->work_num = 1;

	ret = xh2a_rpmsg_lite_ext_init(rldev);
	if (ret) {
		pr_err("%s: rpmsg_lite ext init failed\n", __func__);
		goto destroy;
	}

	client->client_data = rldev;
	client->private_data = handle;
	client->prepare_cb = xh2a_rpmsg_lite_pm_prepare;
	client->complete_cb = xh2a_rpmsg_lite_pm_complete;
	client->notifier_prepare_cb = xh2a_rpmsg_lite_pm_notifier_prepare;
	client->notifier_complete_cb = xh2a_rpmsg_lite_pm_notifier_complete;
	atomic_set(&rldev->block_ioctl_flag, 0);
	init_waitqueue_head(&rldev->block_ioctl_wq);

	ret = xh2a_pcie_register_client(handle, client);
	if (ret) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto destroy;
	}

	xh2a_rpmsg_lite_enable_msi(rldev);
	xh2a_rpmsg_lite_unmask_tx_irq_all(rldev);
	xh2a_rpmsg_lite_unmask_rx_irq_all(rldev);

	atomic_set(&rldev->dev_initialized, 1);

	return 0;

destroy:
	for (; link > XH2A_RPMSG_LITE_LINK_NUM; link--)
		xh2a_rpmsg_lite_destroy_link(&rldev->links[link - 1]);

	kfree(rldev);

	return ret;
}

static int xh2a_rpmsg_lite_remove(void *handle)
{
	int link;
	struct xh2a_pcie_client *client;
	struct xh2a_rpmsg_lite_dev *rldev;

	pr_debug("xh2a rpmsg driver deinit\n");

	xh2a_pcie_get_client(handle, &client, XH2A_RPMSG_DEVICE_NAME);
	if (!client)
		return 0;

	rldev = client->client_data;

	atomic_set(&rldev->dev_removed, 1);

	xh2a_rpmsg_lite_ext_deinit(rldev);

	/* no longer accept tx irq (link up irq) */
	xh2a_rpmsg_lite_disable_msi(rldev);
	xh2a_rpmsg_lite_mask_tx_irq_all(rldev);
	xh2a_rpmsg_lite_mask_rx_irq_all(rldev);

	for (link = 0; link < XH2A_RPMSG_LITE_LINK_NUM; link++)
		xh2a_rpmsg_lite_destroy_link(&rldev->links[link]);

	cancel_work_sync(&rldev->rpmsg_lite_work);

	xh2a_pcie_unregister_client(handle, client);

	rldev->private_data = NULL;
	kref_put(&rldev->dev_refcnt, xh2a_rpmsg_lite_device_safe_release);

	return 0;
}

static int xh2a_rpmsg_notifier_call(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	int ret = 0;

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_rpmsg_lite_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_rpmsg_lite_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

static struct notifier_block xh2a_rpmsg_notifier_block = {
	.notifier_call = xh2a_rpmsg_notifier_call,
};

int __init xh2a_rpmsg_register_driver(void)
{
	xh2a_host_register_notifier_chain(&xh2a_rpmsg_notifier_block);

	return 0;
}

void __exit xh2a_rpmsg_unregister_driver(void)
{
	xh2a_host_unregister_notifier_chain(&xh2a_rpmsg_notifier_block);
}

MODULE_LICENSE("GPL");
