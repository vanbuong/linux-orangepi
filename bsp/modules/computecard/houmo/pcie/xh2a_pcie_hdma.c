// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include "xh2a_pcie.h"

static inline void
xh2a_pcie_hdma_chan_irq_enable(struct xh2a_pcie_hdma_chan *chan,
			       uint32_t irq_mask)
{
	uint32_t val;

	val = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
	val |= irq_mask;
	writel(val, chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
}

static inline void
xh2a_pcie_hdma_chan_irq_disable(struct xh2a_pcie_hdma_chan *chan,
				uint32_t irq_mask)
{
	uint32_t val;

	val = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
	val &= ~irq_mask;
	writel(val, chan->ch_regs + PCIE_HDMA_CHAN_INT_SET);
}

static inline void
xh2a_pcie_hdma_chan_irq_clear(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_INT_ABORT_CLR | PCIE_HDMA_INT_WM_CLR |
		       PCIE_HDMA_INT_STOP_CLR,
	       chan->ch_regs + PCIE_HDMA_CHAN_INT_CLR);
}

static inline void xh2a_pcie_hdma_chan_enable(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_CHAN_EN_BIT, chan->ch_regs + PCIE_HDMA_CHAN_EN);
}

static inline void xh2a_pcie_hdma_chan_disable(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_CHAN_DIS, chan->ch_regs + PCIE_HDMA_CHAN_EN);
}

static inline void xh2a_pcie_hdma_chan_start(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_CHAN_START_BIT, chan->ch_regs + PCIE_HDMA_CHAN_DB);
}

static inline void xh2a_pcie_hdma_chan_stop(struct xh2a_pcie_hdma_chan *chan)
{
	writel(PCIE_HDMA_CHAN_STOP_BIT, chan->ch_regs + PCIE_HDMA_CHAN_DB);
}

static inline void xh2a_pcie_hdma_chan_prepare(struct xh2a_pcie_hdma_chan *chan,
					       dma_addr_t dma_dst,
					       dma_addr_t dma_src, size_t len)
{
	xh2a_pcie_hdma_chan_enable(chan);
	writel(0, chan->ch_regs + PCIE_HDMA_CHAN_CTRL1);
	writel(cpu_to_le32(dma_src & 0xffffffff),
	       chan->ch_regs + PCIE_HDMA_CHAN_SAR_LO);
	writel(cpu_to_le32(dma_src >> 32),
	       chan->ch_regs + PCIE_HDMA_CHAN_SAR_HI);
	writel(cpu_to_le32(dma_dst & 0xffffffff),
	       chan->ch_regs + PCIE_HDMA_CHAN_DAR_LO);
	writel(cpu_to_le32(dma_dst >> 32),
	       chan->ch_regs + PCIE_HDMA_CHAN_DAR_HI);
	writel(len, chan->ch_regs + PCIE_HDMA_CHAN_TRSIZE);
	writel(chan->hdma->msi->address_hi,
	       chan->ch_regs + PCIE_HDMA_CHAN_STOP_MSI_HI);
	writel(chan->hdma->msi->address_lo,
	       chan->ch_regs + PCIE_HDMA_CHAN_STOP_MSI_LO);
	writel(chan->hdma->msi->address_hi,
	       chan->ch_regs + PCIE_HDMA_CHAN_ABRT_MSI_HI);
	writel(chan->hdma->msi->address_lo,
	       chan->ch_regs + PCIE_HDMA_CHAN_ABRT_MSI_LO);
	writel(chan->hdma->msi->data, chan->ch_regs + PCIE_HDMA_CHAN_MSI_DATA);
	xh2a_pcie_hdma_chan_irq_enable(chan, PCIE_HDMA_RAIE | PCIE_HDMA_RSIE |
						     PCIE_HDMA_LAIE |
						     PCIE_HDMA_LSIE);
}

static inline void xh2a_pcie_hdma_enable(struct xh2a_pcie_hdma *hdma)
{
	int i;

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		xh2a_pcie_hdma_chan_enable(&hdma->wrchan[i]);
		xh2a_pcie_hdma_chan_irq_disable(
			&hdma->wrchan[i], PCIE_HDMA_LAIE | PCIE_HDMA_RAIE |
						  PCIE_HDMA_LSIE |
						  PCIE_HDMA_RSIE);
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		xh2a_pcie_hdma_chan_enable(&hdma->rdchan[i]);
		xh2a_pcie_hdma_chan_irq_disable(
			&hdma->rdchan[i], PCIE_HDMA_LAIE | PCIE_HDMA_RAIE |
						  PCIE_HDMA_LSIE |
						  PCIE_HDMA_RSIE);
	}
}

static inline void xh2a_pcie_hdma_disable(struct xh2a_pcie_hdma *hdma)
{
	int i;

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++)
		xh2a_pcie_hdma_chan_disable(&hdma->wrchan[i]);

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++)
		xh2a_pcie_hdma_chan_disable(&hdma->rdchan[i]);
}

static inline void
xh2a_pcie_hdma_chan_controller_dump(struct xh2a_pcie_hdma_chan *chan)
{
#ifdef PCIE_HDMA_DEBUG_VERBOSE
	dev_dbg(chan->hdma->dev, "%s: dump hdma channel %d status:\n", __func__,
		chan->id);

	dev_dbg(chan->hdma->dev,
		"\tEN[0x%x] DB[0x%x] LLP_LO[0x%x] LLP_HI[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_EN),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_DB),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_LLP_LO),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_LLP_HI));
	dev_dbg(chan->hdma->dev,
		"\tSAR_LO[0x%x] SAR_HI[0x%x] DAR_LO[0x%x] DAR_HI[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_SAR_LO),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_SAR_HI),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_DAR_LO),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_DAR_HI));
	dev_dbg(chan->hdma->dev, "\tINT_STS[0x%x] INT_SET[0x%x] STS[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_STS),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_SET),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_STS));
	dev_dbg(chan->hdma->dev, "\tTRSZ[0x%x] CTRL1[0x%x]",
		readl(chan->ch_regs + PCIE_HDMA_CHAN_TRSIZE),
		readl(chan->ch_regs + PCIE_HDMA_CHAN_CTRL1));
#else
	(void)chan;
#endif
}

static inline void
xh2a_pcie_hdma_chan_handle_err(struct xh2a_pcie_hdma_chan *chan)
{
	xh2a_pcie_hdma_chan_stop(chan);
	xh2a_pcie_hdma_chan_disable(chan);
}

static inline void
xh2a_pcie_hdma_chan_block_xfer_complete(struct xh2a_pcie_hdma_chan *chan)
{
	complete(&chan->xfer_completion);
}

static void xh2a_pcie_hdma_work_handler(struct work_struct *work)
{
	int i;
	uint32_t chan_status, int_status;
	struct xh2a_pcie_hdma_chan *chan;
	struct xh2a_pcie_hdma *hdma =
		container_of(work, struct xh2a_pcie_hdma, hdma_work);
	dev_dbg(hdma->dev, "%s: %p\n", __func__, hdma);

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		chan = &hdma->wrchan[i];
		chan_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_STS);
		int_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_STS);

		xh2a_pcie_hdma_chan_controller_dump(chan);

		if ((chan_status == PCIE_HDMA_CHAN_STS_STOPPED) &&
		    (int_status & PCIE_HDMA_INT_STOP_CLR)) {
			dev_dbg(hdma->dev, "%s: wr channel %d stopped",
				__func__, i);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			xh2a_pcie_hdma_chan_block_xfer_complete(chan);
		} else if ((chan_status == PCIE_HDMA_CHAN_STS_ABORTED) &&
			   (int_status & PCIE_HDMA_INT_ABORT_CLR)) {
			dev_dbg(hdma->dev, "%s: wr channel %d aborted %x",
				__func__, i, int_status);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			xh2a_pcie_hdma_chan_handle_err(chan);
		} else {
			/* supperess debug outputs */
			/* dev_dbg(hdma->dev, "%s: ignore wr channel %d",
			   __func__, i); */
		}
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		chan = &hdma->rdchan[i];
		chan_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_STS);
		int_status = readl(chan->ch_regs + PCIE_HDMA_CHAN_INT_STS);

		xh2a_pcie_hdma_chan_controller_dump(chan);

		if ((chan_status == PCIE_HDMA_CHAN_STS_STOPPED) &&
		    (int_status & PCIE_HDMA_INT_STOP_CLR)) {
			dev_dbg(hdma->dev, "%s: rd channel %d stopped",
				__func__, i);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			xh2a_pcie_hdma_chan_block_xfer_complete(chan);
		} else if ((chan_status == PCIE_HDMA_CHAN_STS_ABORTED) &&
			   (int_status & PCIE_HDMA_INT_ABORT_CLR)) {
			dev_dbg(hdma->dev, "%s: rd channel %d aborted %x",
				__func__, i, int_status);
			xh2a_pcie_hdma_chan_irq_clear(chan);
			xh2a_pcie_hdma_chan_handle_err(chan);
		} else {
			/* supperess debug outputs */
			/* dev_dbg(hdma->dev, "%s: ignore rd channel %d",
			   __func__, i); */
		}
	}
}

struct xh2a_pcie_hdma_chan *xh2a_pcie_hdma_get_chan(struct xh2a_pcie_hdma *hdma,
						    int chan_type)
{
	int ret;

	if (chan_type == XH2A_PCIE_HDMA_CHAN_TYPE_RD) {
		ret = mutex_lock_interruptible(&hdma->rd_mutex);

		if (ret != 0) {
			dev_err(hdma->dev,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			return NULL;
		}

		while (find_first_zero_bit(hdma->rd_bitmap,
					   XH2A_PCIE_HDMA_NRDCHAN) >=
		       XH2A_PCIE_HDMA_NRDCHAN) {
			mutex_unlock(&hdma->rd_mutex);

			ret = wait_event_interruptible(
				hdma->rd_wq,
				find_first_zero_bit(hdma->rd_bitmap,
						    XH2A_PCIE_HDMA_NRDCHAN) <
					XH2A_PCIE_HDMA_NRDCHAN);

			if (ret < 0) {
				dev_err(hdma->dev,
					"%s: wait event interrupted %d\n",
					__func__, ret);
				return NULL;
			}

			ret = mutex_lock_interruptible(&hdma->rd_mutex);

			if (ret != 0) {
				dev_err(hdma->dev,
					"%s: mutex_lock_interruptible failed\n",
					__func__);
				return NULL;
			}
		}

		ret = find_first_zero_bit(hdma->rd_bitmap,
					  XH2A_PCIE_HDMA_NRDCHAN);

		__set_bit(ret, hdma->rd_bitmap);
		mutex_unlock(&hdma->rd_mutex);

		return &hdma->rdchan[ret];
	} else if (chan_type == XH2A_PCIE_HDMA_CHAN_TYPE_WR) {
		ret = mutex_lock_interruptible(&hdma->wr_mutex);

		if (ret != 0) {
			dev_err(hdma->dev,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			return NULL;
		}

		while (find_first_zero_bit(hdma->wr_bitmap,
					   XH2A_PCIE_HDMA_NWRCHAN) >=
		       XH2A_PCIE_HDMA_NWRCHAN) {
			mutex_unlock(&hdma->wr_mutex);

			ret = wait_event_interruptible(
				hdma->wr_wq,
				find_first_zero_bit(hdma->wr_bitmap,
						    XH2A_PCIE_HDMA_NWRCHAN) <
					XH2A_PCIE_HDMA_NWRCHAN);

			if (ret < 0) {
				dev_err(hdma->dev,
					"%s: wait event interrupted %d\n",
					__func__, ret);
				return NULL;
			}

			ret = mutex_lock_interruptible(&hdma->wr_mutex);

			if (ret != 0) {
				dev_err(hdma->dev,
					"%s: mutex_lock_interruptible failed\n",
					__func__);
				return NULL;
			}
		}

		ret = find_first_zero_bit(hdma->wr_bitmap,
					  XH2A_PCIE_HDMA_NWRCHAN);

		__set_bit(ret, hdma->wr_bitmap);
		mutex_unlock(&hdma->wr_mutex);

		return &hdma->wrchan[ret];
	}

	return NULL;
}

void xh2a_pcie_hdma_put_chan(struct xh2a_pcie_hdma *hdma, int chan_type,
			     struct xh2a_pcie_hdma_chan *chan)
{
	int ret;

	if (chan_type == XH2A_PCIE_HDMA_CHAN_TYPE_RD) {
		ret = mutex_lock_interruptible(&hdma->rd_mutex);

		if (ret != 0) {
			dev_err(hdma->dev,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			return;
		}

		__clear_bit(chan->id, hdma->rd_bitmap);

		wake_up_interruptible(&hdma->rd_wq);

		mutex_unlock(&hdma->rd_mutex);
	} else if (chan_type == XH2A_PCIE_HDMA_CHAN_TYPE_WR) {
		ret = mutex_lock_interruptible(&hdma->wr_mutex);

		if (ret != 0) {
			dev_err(hdma->dev,
				"%s: mutex_lock_interruptible failed\n",
				__func__);
			return;
		}

		__clear_bit(chan->id, hdma->wr_bitmap);

		wake_up_interruptible(&hdma->wr_wq);

		mutex_unlock(&hdma->wr_mutex);
	}
}

int xh2a_pcie_hdma_chan_transfer(struct xh2a_pcie_hdma_chan *chan,
				 dma_addr_t dma_dst, dma_addr_t dma_src,
				 size_t len)
{
	int ret;

	if (atomic_read(&chan->chan_removed)) {
		return -ENODEV;
	}

	xh2a_pcie_hdma_chan_prepare(chan, dma_dst, dma_src, len);

	reinit_completion(&chan->xfer_completion);

	xh2a_pcie_hdma_chan_start(chan);

	ret = wait_for_completion_interruptible_timeout(
		&chan->xfer_completion,
		msecs_to_jiffies(XH2A_PCIE_HDMA_TIMEOUT_MS));

	if (atomic_read(&chan->chan_removed)) {
		return -ENODEV;
	}

	if (ret <= 0) {
		dev_err(chan->hdma->dev, "%s: timeout or interrupted %d\n",
			__func__, ret);
		xh2a_pcie_hdma_chan_stop(chan);
		return -ERESTARTSYS;
	}

	return 0;
}

int hdma_init(struct xh2a_pcie_hdma *hdma, void __iomem *trgt0_mem,
	      struct msi_msg *msi, struct device *device)
{
	int ret, i;

	hdma->dev = device;

	atomic_set(&hdma->pcie_dma_xfer_complete, 0);
	atomic_set(&hdma->pcie_dma_read, 0);
	atomic_set(&hdma->pcie_dma_write, 0);
	hdma->reg_base = (uint8_t *)trgt0_mem + PCIE_HDMA_REGS_OFFSET;
	hdma->num_chans = XH2A_PCIE_HDMA_NWRCHAN + XH2A_PCIE_HDMA_NRDCHAN;
	hdma->msi = msi;

	dev_dbg(hdma->dev, "%s: 0x%llx, 0x%llx, 0x%llx\n", __func__,
		dma_get_required_mask(device), DMA_BIT_MASK(64),
		DMA_BIT_MASK(32));
	dma_set_coherent_mask(device, DMA_BIT_MASK(64));

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		hdma->wrchan[i].hdma = hdma;
		hdma->wrchan[i].id = i;
		hdma->wrchan[i].ch_regs =
			hdma->reg_base + i * PCIE_HDMA_CHAN_BLK_SIZE * 2;
		hdma->wrchan[i].dma_cfg_buf = dma_alloc_coherent(
			hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
			&hdma->wrchan[i].dma_cfg_phys, GFP_KERNEL);

		if (hdma->wrchan[i].dma_cfg_buf == NULL) {
			dev_err(hdma->dev,
				"%s: dma_alloc_coherent failed for wr channel "
				"%d\n",
				__func__, i);
			ret = -ENOMEM;
			goto dma_alloc_fail;
		}

		dev_dbg(hdma->dev, "%s: dma_alloc_coherent %p, 0x%llx\n",
			__func__, hdma->wrchan[i].dma_cfg_buf,
			hdma->wrchan[i].dma_cfg_phys);

		init_completion(&hdma->wrchan[i].xfer_completion);
		atomic_set(&hdma->wrchan[i].chan_removed, 0);
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		hdma->rdchan[i].hdma = hdma;
		hdma->rdchan[i].id = i;
		hdma->rdchan[i].ch_regs = hdma->reg_base +
					  i * PCIE_HDMA_CHAN_BLK_SIZE * 2 +
					  PCIE_HDMA_CHAN_BLK_SIZE;
		hdma->rdchan[i].dma_cfg_buf = dma_alloc_coherent(
			hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
			&hdma->rdchan[i].dma_cfg_phys, GFP_KERNEL);

		if (hdma->rdchan[i].dma_cfg_buf == NULL) {
			dev_err(hdma->dev,
				"%s: dma_alloc_coherent failed for rd channel "
				"%d\n",
				__func__, i);
			ret = -ENOMEM;
			goto dma_alloc_fail;
		}

		dev_dbg(hdma->dev, "%s: dma_alloc_coherent %p, 0x%llx\n",
			__func__, hdma->rdchan[i].dma_cfg_buf,
			hdma->rdchan[i].dma_cfg_phys);

		init_completion(&hdma->rdchan[i].xfer_completion);
		atomic_set(&hdma->rdchan[i].chan_removed, 0);
	}

	mutex_init(&hdma->wr_mutex);
	mutex_init(&hdma->rd_mutex);

	init_waitqueue_head(&hdma->wr_wq);
	init_waitqueue_head(&hdma->rd_wq);

	bitmap_zero(hdma->wr_bitmap, XH2A_PCIE_HDMA_NWRCHAN);
	bitmap_zero(hdma->rd_bitmap, XH2A_PCIE_HDMA_NRDCHAN);

	xh2a_pcie_hdma_enable(hdma);

	INIT_WORK(&hdma->hdma_work, xh2a_pcie_hdma_work_handler);
	return 0;

dma_alloc_fail:

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		if (hdma->wrchan[i].dma_cfg_buf != NULL)
			dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
					  hdma->wrchan[i].dma_cfg_buf,
					  hdma->wrchan[i].dma_cfg_phys);
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		if (hdma->rdchan[i].dma_cfg_buf != NULL)
			dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
					  hdma->rdchan[i].dma_cfg_buf,
					  hdma->rdchan[i].dma_cfg_phys);
	}

	return ret;
}

void hdma_deinit(struct xh2a_pcie_hdma *hdma)
{
	int i;

	wake_up_interruptible(&hdma->wr_wq);
	wake_up_interruptible(&hdma->rd_wq);

	cancel_work_sync(&hdma->hdma_work);

	for (i = 0; i < XH2A_PCIE_HDMA_NWRCHAN; i++) {
		atomic_set(&hdma->wrchan[i].chan_removed, 1);
		complete_all(&hdma->wrchan[i].xfer_completion);
		if (hdma->wrchan[i].dma_cfg_buf != NULL)
			dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
					  hdma->wrchan[i].dma_cfg_buf,
					  hdma->wrchan[i].dma_cfg_phys);
	}

	for (i = 0; i < XH2A_PCIE_HDMA_NRDCHAN; i++) {
		atomic_set(&hdma->rdchan[i].chan_removed, 1);
		complete_all(&hdma->rdchan[i].xfer_completion);
		if (hdma->rdchan[i].dma_cfg_buf != NULL)
			dma_free_coherent(hdma->dev, XH2A_PCIE_HDMA_BUF_SIZE,
					  hdma->rdchan[i].dma_cfg_buf,
					  hdma->rdchan[i].dma_cfg_phys);
	}

	xh2a_pcie_hdma_disable(hdma);
}
