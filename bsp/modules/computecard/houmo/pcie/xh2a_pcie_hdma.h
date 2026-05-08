// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_PCIE_HDMA_H
#define _XH2A_PCIE_HDMA_H
#include <linux/pci.h>
#include <linux/msi.h>
#include <linux/irq.h>

#define XH2A_PCIE_HDMA_NWRCHAN (8)
#define XH2A_PCIE_HDMA_NRDCHAN (XH2A_PCIE_HDMA_NWRCHAN)

#define XH2A_PCIE_HDMA_TIMEOUT_MS (1000)
#define XH2A_PCIE_HDMA_BUF_SIZE	  (2 * 1024 * 1024)

/* HDMA */
#define PCIE_HDMA_REGS_OFFSET 0x2000

#define PCIE_HDMA_CHAN_BLK_SIZE	   0x100
#define PCIE_HDMA_CHAN_EN	   0x0
#define PCIE_HDMA_CHAN_DB	   0x4
#define PCIE_HDMA_CHAN_PF	   0x8
#define PCIE_HDMA_CHAN_HS	   0xC
#define PCIE_HDMA_CHAN_LLP_LO	   0x10
#define PCIE_HDMA_CHAN_LLP_HI	   0x14
#define PCIE_HDMA_CHAN_CYCLE	   0x18
#define PCIE_HDMA_CHAN_TRSIZE	   0x1C
#define PCIE_HDMA_CHAN_SAR_LO	   0x20
#define PCIE_HDMA_CHAN_SAR_HI	   0x24
#define PCIE_HDMA_CHAN_DAR_LO	   0x28
#define PCIE_HDMA_CHAN_DAR_HI	   0x2C
#define PCIE_HDMA_CHAN_LL_WM	   0x30
#define PCIE_HDMA_CHAN_CTRL1	   0x34
#define PCIE_HDMA_CHAN_FUNC	   0x38
#define PCIE_HDMA_CHAN_QOS	   0x3C
#define PCIE_HDMA_CHAN_STS	   0x80
#define PCIE_HDMA_CHAN_INT_STS	   0x84
#define PCIE_HDMA_CHAN_INT_SET	   0x88
#define PCIE_HDMA_CHAN_INT_CLR	   0x8C
#define PCIE_HDMA_CHAN_STOP_MSI_LO 0x90
#define PCIE_HDMA_CHAN_STOP_MSI_HI 0x94
#define PCIE_HDMA_CHAN_WM_MSI_LO   0x98
#define PCIE_HDMA_CHAN_WM_MSI_HI   0x9C
#define PCIE_HDMA_CHAN_ABRT_MSI_LO 0xA0
#define PCIE_HDMA_CHAN_ABRT_MSI_HI 0xA4
#define PCIE_HDMA_CHAN_MSI_DATA	   0xA8

#define PCIE_HDMA_CHAN_EN_BIT BIT(0)
#define PCIE_HDMA_CHAN_DIS    0

#define PCIE_HDMA_CHAN_START_BIT BIT(0)
#define PCIE_HDMA_CHAN_STOP_BIT	 BIT(1)

#define PCIE_HDMA_CHAN_STS_RUNNING 0x1
#define PCIE_HDMA_CHAN_STS_ABORTED 0x2
#define PCIE_HDMA_CHAN_STS_STOPPED 0x3

#define PCIE_HDMA_LAIE BIT(6)
#define PCIE_HDMA_RAIE BIT(5)
#define PCIE_HDMA_LSIE BIT(4)
#define PCIE_HDMA_RSIE BIT(3)

#define PCIE_HDMA_INT_ABORT_CLR BIT(2)
#define PCIE_HDMA_INT_WM_CLR	BIT(1)
#define PCIE_HDMA_INT_STOP_CLR	BIT(0)

#define PCIE_HDMA_CTRL1_LLEN	BIT(0)
#define PCIE_HDMA_CTRL1_MEMTYPE BIT(1)

#define PCIE_HDMA_LL_LLP BIT(2)
#define PCIE_HDMA_LL_TCB BIT(1)

/*
 * XH2A_PCIE_HDMA_CHAN_TYPE_WR - write channel type
 * XH2A_PCIE_HDMA_CHAN_TYPE_RD - read channel type
 */
#define XH2A_PCIE_HDMA_CHAN_TYPE_WR 100
#define XH2A_PCIE_HDMA_CHAN_TYPE_RD 200

struct xh2a_pcie_hdma;

/*
 * struct xh2a_pcie_hdma_chan - hdma channel structure
 * @hdma: pointer to hdma structure
 * @id: channel id
 * @ch_regs: channel registers base address
 * @dma_cfg_phys: dma config buffer physical address
 * @dma_cfg_buf: dma config buffer virtual address
 * @xfer_completion: completion structure for hdma transfer
 */
struct xh2a_pcie_hdma_chan {
	struct xh2a_pcie_hdma *hdma;
	int id;
	void __iomem *ch_regs;
	uint64_t dma_cfg_phys;
	void *dma_cfg_buf;
	struct completion xfer_completion;
	atomic_t chan_removed;
};

/*
 * struct xh2a_pcie_hdma - hdma structure
 * @dev: pointer to device structure
 * @num_chans: number of hdma channels
 * @msi: pointer to msi structure
 * @reg_base: hdma registers base address
 * @wr_mutex: mutex for write channels
 * @rd_mutex: mutex for read channels
 * @wr_bitmap: bitmap of write channels in use
 * @rd_bitmap: bitmap of read channels in use
 * @wr_wq: wait queueue for write channels
 * @rd_wq: wait queue for read channels
 * @wrchan: array of write channels struct
 * @rdchan: array of read channels struct
 * @hdma_work: work_struct for hdma
 * @pcie_dma_read: counter for pcie dma read
 * @pcie_dma_write: counter for pcie dma write
 * @pcie_dma_xfer_complete: counter for pcie dma xfer complete
 */
struct xh2a_pcie_hdma {
	struct device *dev;
	int num_chans;

	struct msi_msg *msi;
	void __iomem *reg_base;

	struct mutex wr_mutex;
	struct mutex rd_mutex;

	unsigned long wr_bitmap[BITS_TO_LONGS(XH2A_PCIE_HDMA_NWRCHAN)];
	unsigned long rd_bitmap[BITS_TO_LONGS(XH2A_PCIE_HDMA_NRDCHAN)];

	struct wait_queue_head wr_wq;
	struct wait_queue_head rd_wq;

	struct xh2a_pcie_hdma_chan wrchan[XH2A_PCIE_HDMA_NWRCHAN];
	struct xh2a_pcie_hdma_chan rdchan[XH2A_PCIE_HDMA_NRDCHAN];

	struct work_struct hdma_work;

	atomic_t pcie_dma_read;
	atomic_t pcie_dma_write;
	atomic_t pcie_dma_xfer_complete;
};

/*
 * hdma_init() - Initialize hdma. internal api for xh2a_pcie driver
 * @hdma: pointer to hdma structure
 * @trgt0_mem: target memory base address
 * @msi: pointer to msi structure
 * @device: pointer to device structure
 * Return: 0 on success, negative error code on failure
 */
int hdma_init(struct xh2a_pcie_hdma *hdma, void __iomem *trgt0_mem,
	      struct msi_msg *msi, struct device *device);

/*
 * hdma_deinit() - Deinitialize hdma. internal api for xh2a_pcie driver
 * @hdma: pointer to hdma structure
 */
void hdma_deinit(struct xh2a_pcie_hdma *hdma);

/*
 * xh2a_pcie_hdma_get_chan() - Get hdma channel. internal api.
 * @hdma: pointer to hdma structure
 * @chan_type: channel type
 * Return: pointer to hdma channel structure on success, NULL on failure
 */
struct xh2a_pcie_hdma_chan *xh2a_pcie_hdma_get_chan(struct xh2a_pcie_hdma *hdma,
						    int chan_type);

/*
 * xh2a_pcie_hdma_put_chan() - Put hdma channel. internal api.
 * @hdma: pointer to hdma structure
 * @chan_type: channel type
 * @chan: pointer to hdma channel structure
 */
void xh2a_pcie_hdma_put_chan(struct xh2a_pcie_hdma *hdma, int chan_type,
			     struct xh2a_pcie_hdma_chan *chan);

/*
 * xh2a_pcie_hdma_chan_transfer() - Transfer data using hdma channel.
 * @chan: pointer to hdma channel structure
 * @dma_dst: destination dma address
 * @dma_src: source dma address
 * @len: length of data to be transferred
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_hdma_chan_transfer(struct xh2a_pcie_hdma_chan *chan,
				 dma_addr_t dma_dst, dma_addr_t dma_src,
				 size_t len);

#endif
