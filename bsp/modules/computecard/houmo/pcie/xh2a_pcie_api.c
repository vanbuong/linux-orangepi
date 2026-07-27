// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>

#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include <version_compat.h>
#include "xh2a_pcie.h"

static inline int xh2a_pcie_iatu_bar_map(struct xh2a_pcie_dev *p_xh2a,
					 uint64_t base, int barid)
{
	int retries = 0;
	uint32_t val;

	writel((barid << 8),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL2));
	writel((base & 0xffffffff),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_LOWER_TARGET));
	writel(((base >> 32) & 0xffffffff),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_UPPER_TARGET));
	writel((0 | PCIE_ATU_FUNC_NUM(0)),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL1));
	writel((PCIE_ATU_ENABLE | PCIE_ATU_BAR_MODE_ENABLE | (barid << 8)),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL2));

	do {
		val = readl(XH2A_PCIE_IATU_IB_REG(barid,
						  PCIE_ATU_UNR_REGION_CTRL2));
		retries++;
	} while ((retries < 100) && !(val & PCIE_ATU_ENABLE));

	if (retries < 100)
		return 0;

	return -1;
}

static inline void xh2a_pcie_iatu_bar_unmap(struct xh2a_pcie_dev *p_xh2a,
					    int barid)
{
	writel((barid << 8),
	       XH2A_PCIE_IATU_IB_REG(barid, PCIE_ATU_UNR_REGION_CTRL2));
}

#define XH2A_RAW_COPY_DIR_HOST_TO_DEV 0
#define XH2A_RAW_COPY_DIR_DEV_TO_HOST 1

static inline void raw_copy(char *data, char *addr, uint64_t tz, int dir)
{
	if (dir == XH2A_RAW_COPY_DIR_DEV_TO_HOST)
		memcpy_fromio(data, addr, tz);
	else if (dir == XH2A_RAW_COPY_DIR_HOST_TO_DEV)
		memcpy_toio(addr, data, tz);
	else
		pr_err("%s: wrong parameter dir %d", __func__, dir);
}

static inline int xh2a_pcie_iatu_regbar_copy(struct xh2a_pcie_dev *p_xh2a,
					     uint64_t paddr, char *user_data,
					     uint64_t size, int dir)
{
	unsigned long flags;
	uint64_t offset;
	uint64_t base;
	uint64_t used;
	uint64_t tz;
	int ret;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);
	used = 0;

	if (size + offset <= p_xh2a->bar0_len) {
		tz = size;
		ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

		if (ret != 0)
			goto out;

		raw_copy(user_data, p_xh2a->bar0_mem + offset, tz, dir);
		used += tz;
	} else {
		do {
			tz = p_xh2a->bar0_len - offset;
			ret = xh2a_pcie_iatu_bar_map(p_xh2a, base,
						     XH2A_PCIE_REGBAR);

			if (ret != 0)
				goto out;

			raw_copy(user_data + used, p_xh2a->bar0_mem + offset,
				 tz, dir);
			used += tz;
			base += p_xh2a->bar0_len;
			offset = 0;
		} while (size - used > p_xh2a->bar0_len);

		tz = size - used;
		ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

		if (ret != 0)
			goto out;

		raw_copy(user_data + used, p_xh2a->bar0_mem + offset, tz, dir);
		used += tz;
		base += p_xh2a->bar0_len;
		offset = 0;
	}

out:
	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return ret;
}

int xh2a_pcie_membar_getinfo(void *handle, uint64_t *bar_addr,
			     uint64_t *bar_size)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	*bar_addr = p_xh2a->bar2_base;
	*bar_size = p_xh2a->bar2_len;

	return 0;
}

int xh2a_pcie_membar_map(void *handle, uint64_t paddr)
{
	unsigned long flags;
	int ret = -1;
	uint64_t base;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_MEMBAR], flags);

	if (p_xh2a->membar_lockmap != 0)
		goto out;

	p_xh2a->membar_lockmap = 1;

	base = paddr & ~(p_xh2a->bar2_len - 1);
	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_MEMBAR);

	if (ret != 0) {
		p_xh2a->membar_lockmap = 0;
		goto out;
	}

out:
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_MEMBAR], flags);

	if (ret) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map bar%d, %d\n",
			__func__, XH2A_PCIE_MEMBAR, ret);
		return -EBUSY;
	}

	return ret;
}

int xh2a_pcie_membar_unmap(void *handle)
{
	unsigned long flags;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_MEMBAR], flags);

	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_MEMBAR);
	p_xh2a->membar_lockmap = 0;

	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_MEMBAR], flags);

	return 0;
}

int xh2a_pcie_membar_map_no_lock(void *handle, uint64_t paddr)
{
	int ret;
	uint64_t base;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	base = paddr & ~(p_xh2a->bar2_len - 1);
	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_MEMBAR);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map iatu bar%d, %d\n",
			__func__, XH2A_PCIE_MEMBAR, ret);
		ret = -EBUSY;
	}

	return ret;
}

int xh2a_pcie_membar_unmap_no_lock(void *handle)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_MEMBAR);

	return 0;
}

static int xh2a_pcie_pio_read(void *handle, uint64_t paddr, uint32_t nbytes,
			      uint64_t *data)
{
	unsigned long flags;
	uint64_t base, offset;
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

	if (ret != 0)
		goto out;

	switch (nbytes) {
	case 8:
		*data = readq(p_xh2a->bar0_mem + (offset & (uint64_t)(-8)));
		break;

	case 2:
		*data = readw(p_xh2a->bar0_mem + (offset & (uint64_t)(-2)));
		break;

	case 1:
		*data = readb(p_xh2a->bar0_mem + offset);
		break;

	case 4:
	default:
		*data = readl(p_xh2a->bar0_mem + (offset & (uint64_t)(-4)));
		break;
	}

out:
	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map iatu bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return ret;
}

static int xh2a_pcie_pio_write(void *handle, uint64_t paddr, uint32_t nbytes,
			       uint64_t data)
{
	unsigned long flags;
	uint64_t base, offset;
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	base = paddr & ~(p_xh2a->bar0_len - 1);
	offset = paddr & (p_xh2a->bar0_len - 1);

	spin_lock_irqsave(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	ret = xh2a_pcie_iatu_bar_map(p_xh2a, base, XH2A_PCIE_REGBAR);

	if (ret != 0)
		goto out;

	switch (nbytes) {
	case 8:
		writeq(data, p_xh2a->bar0_mem + (offset & (uint64_t)(-8)));
		break;

	case 2:
		writew(data & 0xffff,
		       p_xh2a->bar0_mem + (offset & (uint64_t)(-2)));
		break;

	case 1:
		writeb(data & 0xff, p_xh2a->bar0_mem + offset);
		break;

	case 4:
	default:
		writel(data & 0xffffffff,
		       p_xh2a->bar0_mem + (offset & (uint64_t)(-4)));
		break;
	}

out:
	xh2a_pcie_iatu_bar_unmap(p_xh2a, XH2A_PCIE_REGBAR);
	spin_unlock_irqrestore(&p_xh2a->iatu_lock[XH2A_PCIE_REGBAR], flags);

	if (ret != 0) {
		dev_err(&p_xh2a->pdev->dev, "%s failed to map iatu bar%d, %d\n",
			__func__, XH2A_PCIE_REGBAR, ret);
		return -EBUSY;
	}

	return ret;
}

int xh2a_pcie_pio_readb(void *handle, uint64_t paddr, uint8_t *value)
{
	int ret;
	uint64_t v;

	ret = xh2a_pcie_pio_read(handle, paddr, 1, &v);
	*value = v & 0xff;
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readb);

int xh2a_pcie_pio_readw(void *handle, uint64_t paddr, uint16_t *value)
{
	int ret;
	uint64_t v;

	ret = xh2a_pcie_pio_read(handle, paddr, 2, &v);
	*value = v & 0xffff;
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readw);

int xh2a_pcie_pio_readl(void *handle, uint64_t paddr, uint32_t *value)
{
	int ret;
	uint64_t v;

	ret = xh2a_pcie_pio_read(handle, paddr, 4, &v);
	*value = v & 0xffffffff;
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readl);

int xh2a_pcie_pio_readq(void *handle, uint64_t paddr, uint64_t *value)
{
	int ret;

	ret = xh2a_pcie_pio_read(handle, paddr, 8, value);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_readq);

int xh2a_pcie_pio_writeb(void *handle, uint64_t paddr, uint8_t value)
{
	int ret;
	uint64_t v;

	v = value & 0xff;
	ret = xh2a_pcie_pio_write(handle, paddr, 1, v);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writeb);

int xh2a_pcie_pio_writew(void *handle, uint64_t paddr, uint16_t value)
{
	int ret;
	uint64_t v;

	v = value & 0xffff;
	ret = xh2a_pcie_pio_write(handle, paddr, 2, v);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writew);

int xh2a_pcie_pio_writel(void *handle, uint64_t paddr, uint32_t value)
{
	int ret;
	uint64_t v;

	v = value & 0xffffffff;
	ret = xh2a_pcie_pio_write(handle, paddr, 4, v);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writel);

int xh2a_pcie_pio_writeq(void *handle, uint64_t paddr, uint64_t value)
{
	int ret;

	ret = xh2a_pcie_pio_write(handle, paddr, 8, value);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_writeq);

int xh2a_pcie_pio_read_mem(void *handle, uint64_t paddr, void *host_dst_addr,
			   uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_dst_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, host_dst_addr, size,
					 XH2A_RAW_COPY_DIR_DEV_TO_HOST);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_read_mem);

int xh2a_pcie_pio_write_mem(void *handle, uint64_t paddr, void *host_src_addr,
			    uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_src_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, host_src_addr, size,
					 XH2A_RAW_COPY_DIR_HOST_TO_DEV);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_mem);

int xh2a_pcie_pio_write_mem_sync(void *handle, uint64_t paddr,
				 void *host_src_addr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	char *rb;
	char rbstack[128];

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_src_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, host_src_addr, size,
					 XH2A_RAW_COPY_DIR_HOST_TO_DEV);

	if (ret)
		return ret;

	if (size <= sizeof(rbstack))
		rb = rbstack;
	else {
		pr_warn("%s: size too large.\n", __func__);
		rb = kmalloc(size, GFP_KERNEL);
		if (!rb) {
			pr_err("%s: kmalloc failed\n", __func__);
			return -ENOMEM;
		}
	}

	ret = xh2a_pcie_iatu_regbar_copy(p_xh2a, paddr, rb, size,
					 XH2A_RAW_COPY_DIR_DEV_TO_HOST);

	if (rb != rbstack)
		kfree(rb);
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_pio_write_mem_sync);

int xh2a_pcie_dma_read_mem(void *handle, uint64_t paddr, void *host_dst_addr,
			   uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_wr = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_dst_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	pd_wr = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_WR);

	if (pd_wr == NULL) {
		dev_err(&p_xh2a->pdev->dev, "%s: get channel failed", __func__);
		return -ERESTARTSYS;
	}

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		ret = xh2a_pcie_hdma_chan_transfer(pd_wr, pd_wr->dma_cfg_phys,
						   dev_phy_addr + done_size,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_WR,
						pd_wr);
			goto out;
		}

		memcpy((uint8_t *)host_dst_addr + done_size, pd_wr->dma_cfg_buf,
		       xfer_size);

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_WR,
				pd_wr);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_read_mem);

int xh2a_pcie_dma_write_mem(void *handle, uint64_t paddr, void *host_src_addr,
			    uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_rd = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(host_src_addr)) {
		pr_err("%s: host addr is null\n", __func__);
		return -EFAULT;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	pd_rd = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_RD);

	if (pd_rd == NULL) {
		dev_err(&p_xh2a->pdev->dev, "%s: get channel failed", __func__);
		return -ERESTARTSYS;
	}

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		memcpy(pd_rd->dma_cfg_buf, (uint8_t *)host_src_addr + done_size,
		       xfer_size);

		ret = xh2a_pcie_hdma_chan_transfer(pd_rd,
						   dev_phy_addr + done_size,
						   pd_rd->dma_cfg_phys,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_RD,
						pd_rd);
			goto out;
		}

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_RD,
				pd_rd);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_write_mem);

int xh2a_pcie_dma_mrd_direct(void *handle, uint64_t src_paddr,
			     uint64_t dst_paddr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_hdma_chan *pd_rd = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	pd_rd = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_RD);

	if (pd_rd == NULL) {
		dev_err(&p_xh2a->pdev->dev, "%s: get channel failed", __func__);
		return -ERESTARTSYS;
	}

	ret = xh2a_pcie_hdma_chan_transfer(pd_rd, dst_paddr, src_paddr, size);

	if (ret != 0)
		dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d", __func__,
			ret);

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_RD,
				pd_rd);

	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_mrd_direct);

int xh2a_pcie_dma_mwr_direct(void *handle, uint64_t src_paddr,
			     uint64_t dst_paddr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_hdma_chan *pd_wr = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	pd_wr = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_WR);

	if (pd_wr == NULL) {
		dev_err(&p_xh2a->pdev->dev, "%s: get channel failed", __func__);
		return -ERESTARTSYS;
	}

	ret = xh2a_pcie_hdma_chan_transfer(pd_wr, dst_paddr, src_paddr, size);

	if (ret != 0)
		dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d", __func__,
			ret);

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_WR,
				pd_wr);

	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_mwr_direct);

int xh2a_pcie_dma_read_mem_userspace(void *handle, uint64_t paddr,
				     void __user *host_dst_addr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_wr = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	if (!access_write_ok(host_dst_addr, size)) {
		dev_err(&p_xh2a->pdev->dev, "%s: access write failed",
			__func__);
		ret = -EFAULT;
		goto out;
	}

	pd_wr = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_WR);

	if (pd_wr == NULL) {
		dev_err(&p_xh2a->pdev->dev, "%s: get channel failed", __func__);
		return -ERESTARTSYS;
	}

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		ret = xh2a_pcie_hdma_chan_transfer(pd_wr, pd_wr->dma_cfg_phys,
						   dev_phy_addr + done_size,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_WR,
						pd_wr);
			goto out;
		}

		ret = copy_to_user((uint8_t *)host_dst_addr + done_size,
				   pd_wr->dma_cfg_buf, xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: copy to user fail %d",
				__func__, ret);
			ret = -EFAULT;
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_WR,
						pd_wr);
			goto out;
		}

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_WR,
				pd_wr);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_read_mem_userspace);

int xh2a_pcie_dma_write_mem_userspace(void *handle, uint64_t paddr,
				      void __user *host_src_addr, uint32_t size)
{
	int ret;
	struct xh2a_pcie_dev *p_xh2a;
	uint64_t dev_phy_addr;
	uint32_t xfer_size = 0;
	uint32_t done_size = 0;
	struct xh2a_pcie_hdma_chan *pd_rd = NULL;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	dev_phy_addr = paddr;

	if (!access_read_ok(host_src_addr, size)) {
		dev_err(&p_xh2a->pdev->dev, "%s: access read failed", __func__);
		ret = -EFAULT;
		goto out;
	}

	pd_rd = xh2a_pcie_hdma_get_chan(&p_xh2a->hdma,
					XH2A_PCIE_HDMA_CHAN_TYPE_RD);

	if (pd_rd == NULL) {
		dev_err(&p_xh2a->pdev->dev, "%s: get channel failed", __func__);
		return -ERESTARTSYS;
	}

	while (done_size != size) {
		if ((size - done_size) < XH2A_PCIE_HDMA_BUF_SIZE)
			xfer_size = size - done_size;
		else
			xfer_size = XH2A_PCIE_HDMA_BUF_SIZE;

		ret = copy_from_user(pd_rd->dma_cfg_buf,
				     (uint8_t *)host_src_addr + done_size, xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev,
				"%s: copy from user fail %d\n", __func__, ret);
			ret = -EFAULT;
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_RD,
						pd_rd);
			goto out;
		}

		ret = xh2a_pcie_hdma_chan_transfer(pd_rd,
						   dev_phy_addr + done_size,
						   pd_rd->dma_cfg_phys,
						   xfer_size);

		if (ret != 0) {
			dev_err(&p_xh2a->pdev->dev, "%s: hdma xfer fail %d",
				__func__, ret);
			xh2a_pcie_hdma_put_chan(&p_xh2a->hdma,
						XH2A_PCIE_HDMA_CHAN_TYPE_RD,
						pd_rd);
			goto out;
		}

		done_size += xfer_size;
	}

	xh2a_pcie_hdma_put_chan(&p_xh2a->hdma, XH2A_PCIE_HDMA_CHAN_TYPE_RD,
				pd_rd);

out:
	return ret;
}
EXPORT_SYMBOL(xh2a_pcie_dma_write_mem_userspace);

/*
 * ELBI msg register (PCIE_ELBI_MSG_SET) is 32bit wide. We divide it into
 * two 16bit fields. Lower 16bit is for E2 and upper 16bit is for U7.
 * to generate ELBI msg, we need to set the appropriate mask for both E2
 * and U7, and shift the msg to the correct offset, aka. lower 16bit for E2,
 * and upper 16bit for U7.
 * to clear ELBI msg, we need to write msgclr register (PCIE_ELBI_MSG_CLR) with
 * correct mask.
 */
#define XH2A_PCIE_ELBI_MSG_OFFSET_E2   0
#define XH2A_PCIE_ELBI_MSG_OFFSET_U7   16
#define XH2A_PCIE_ELBI_MSG_MASK_E2_VAL 0xFFFF0000
#define XH2A_PCIE_ELBI_MSG_MASK_U7_VAL 0x0000FFFF
#define XH2A_PCIE_ELBI_MSG_BIT_MAX     16

int xh2a_pcie_write_bar_msgbit(void *handle, uint32_t msgbit, uint32_t tocpu)
{
	uint32_t val;
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (msgbit >= XH2A_PCIE_ELBI_MSG_BIT_MAX) {
		dev_err(&p_xh2a->pdev->dev, "%s: msgbit %d is out of range",
			__func__, msgbit);
		return -EINVAL;
	}

	val = readl(p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
	writel(XH2A_PCIE_ELBI_MSG_MASK_E2_VAL,
	       p_xh2a->trgt0_mem + PCIE_ELBI_MSG_MASK_E2);
#if 0
	writel(XH2A_PCIE_ELBI_MSG_MASK_U7_VAL,
	       p_xh2a->trgt0_mem + PCIE_ELBI_MSG_MASK_U7);
#endif

	if (tocpu == XH2A_PCIE_MSG_TO_U7) {
		val |= (BIT(msgbit) << XH2A_PCIE_ELBI_MSG_OFFSET_U7);
		writel(val, p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
	} else if (tocpu == XH2A_PCIE_MSG_TO_E2) {
		val |= (BIT(msgbit) << XH2A_PCIE_ELBI_MSG_OFFSET_E2);
		writel(val, p_xh2a->trgt0_mem + PCIE_ELBI_MSG_SET);
	} else {
		dev_err(&p_xh2a->pdev->dev, "%s: invalid cpu parameter %d",
			__func__, tocpu);
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_write_bar_msgbit);

int xh2a_pcie_check_pwrsts(void *handle, uint32_t *pwrsts)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;
	*pwrsts = readl(p_xh2a->trgt0_mem + PCIE_ELBI_PWRSTS);
	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_check_pwrsts);

int xh2a_pcie_update_pwrsts(void *handle, uint32_t pwrsts)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;
	writel(pwrsts, p_xh2a->trgt0_mem + PCIE_ELBI_PWRSTS);
	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_update_pwrsts);

int xh2a_pcie_register_client(void *handle, struct xh2a_pcie_client *client)
{
	struct xh2a_pcie_dev *p_xh2a;
	int i;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	if (client->work_num > XH2A_PCIE_CLIENT_MAX_WORK_NUM) {
		dev_err(&p_xh2a->pdev->dev, "%s: invalid work_num %d\n",
			__func__, client->work_num);
		return -EINVAL;
	}

	for (i = 0; i < client->work_num; i++) {
		if (IS_ERR_OR_NULL(client->work[i].msi_work)) {
			dev_err(&p_xh2a->pdev->dev, "%s: invalid work %d\n",
				__func__, i);
			return -EINVAL;
		}
	}

	mutex_lock(&p_xh2a->client_list_mutex);
	INIT_LIST_HEAD(&client->node);
	list_add_tail(&client->node, &p_xh2a->client_list);
	mutex_unlock(&p_xh2a->client_list_mutex);
	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_register_client);

int xh2a_pcie_unregister_client(void *handle, struct xh2a_pcie_client *client)
{
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_client *pos, *n;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	mutex_lock(&p_xh2a->client_list_mutex);
	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (pos == client)
			list_del(&pos->node);
	}
	mutex_unlock(&p_xh2a->client_list_mutex);

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_unregister_client);

int xh2a_pcie_get_client(void *handle, struct xh2a_pcie_client **client,
			 const char *name)
{
	struct xh2a_pcie_dev *p_xh2a;
	struct xh2a_pcie_client *pos, *n;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	mutex_lock(&p_xh2a->client_list_mutex);
	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (strcmp(pos->name, name) == 0) {
			*client = pos;
			break;
		}
	}
	mutex_unlock(&p_xh2a->client_list_mutex);

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_get_client);

int xh2a_pcie_device_index(void *handle)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	return p_xh2a->minor;
}
EXPORT_SYMBOL(xh2a_pcie_device_index);

int xh2a_pcie_device_dbdf(void *handle, uint32_t *dbdf)
{
	struct xh2a_pcie_dev *p_xh2a;
	uint32_t domain;
	uint32_t bus;
	uint32_t devfn;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return -EINVAL;
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	domain = pci_domain_nr(p_xh2a->pdev->bus);
	bus = p_xh2a->pdev->bus->number;
	devfn = p_xh2a->pdev->devfn;

	*dbdf = ((domain & 0xffff) << 16) | ((bus & 0xff) << 8) |
		(devfn & 0xff);

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_device_dbdf);

struct device *xh2a_pcie_device_ptr(void *handle)
{
	struct xh2a_pcie_dev *p_xh2a;

	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	p_xh2a = (struct xh2a_pcie_dev *)handle;

	return &p_xh2a->pdev->dev;
}
EXPORT_SYMBOL(xh2a_pcie_device_ptr);

void xh2a_pcie_stop_runtime_pm(void *handle)
{
	struct xh2a_pcie_dev *p_xh2a;
	if (IS_ERR_OR_NULL(handle)) {
		pr_err("%s: handle is null\n", __func__);
		return;
	}
	p_xh2a = (struct xh2a_pcie_dev *)handle;
	pm_runtime_forbid(&p_xh2a->pdev->dev);
}
EXPORT_SYMBOL(xh2a_pcie_stop_runtime_pm);
