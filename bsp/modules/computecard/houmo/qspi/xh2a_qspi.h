// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */

#ifndef _XH2A_QSPI_H_
#define _XH2A_QSPI_H_

struct xh2a_qspi_dev;

struct xh2a_flash_ops {
	int (*program_result)(struct xh2a_qspi_dev *qspi_dev);
	int (*erase_result)(struct xh2a_qspi_dev *qspi_dev);
};

#define XH2A_QSPI_NAME_LEN 64

/*
 * struct xh2a_qspi_dev - xh2a qspi device
 * @miscdev: misc device
 * @name_buf: name buffer
 * @qspi_mutex: mutex for open/release/read/write/ioctl
 * @xh2a_xspi_base: QSPI base address on e2
 * @is_4byte_address: 1: 4-byte address, 0: 3-byte address
 * @is_dma: 1: dma mode, 0: cpu mode
 * @flash_id: Flash Model
 * @device_paddr: E2 DDR physical address
 * @private_data: handle for pcie
 * @block_ioctl_flag: flag for block ioctl
 * @block_ioctl_wq: wait queue for block ioctl
 * @opened: opened count
 * @removed: removed flag
 * @refcount: reference count
 */
struct xh2a_qspi_dev {
	struct miscdevice miscdev;
	char name_buf[XH2A_QSPI_NAME_LEN];
	struct mutex qspi_mutex;
	struct xh2a_pcie_client client;
	uint64_t xh2a_xspi_base;
	uint64_t device_paddr;
	void *private_data;
	uint32_t flash_id;
	struct xh2a_flash_ops ops;
	uint32_t poll_timeout_ms;
	bool is_4byte_address;
	bool is_dma;
	atomic_t block_ioctl_flag;
	struct wait_queue_head block_ioctl_wq;
	atomic_t opened;
	atomic_t removed;
	struct kref refcount;
};

#define XH2A_SUPPORTED_FLASH_NUM 10

#define QSPI_BUFFER_SIZE	 SZ_2K
#define PROGRAM_PAGE_SIZE	 (0x1U << 8)
#define NOR_PAGE_MASK(page_size) ((page_size) - 1)

#define XSPI_BASE_ADDR	  (0x70004000U)
#define DW_SPI_CTRLR0	  0x00
#define DW_SPI_CTRLR1	  0x04
#define DW_SPI_SSIENR	  0x08
#define DW_SPI_SER	  0x10
#define DW_SPI_TXFTLR	  0x18
#define DW_SPI_RXFTLR	  0x1c
#define DW_SPI_TXFLR	  0x20
#define DW_SPI_RXFLR	  0x24
#define DW_SPI_SR	  0x28
#define DW_SPI_DMACR	  0x4c
#define DW_SPI_AXIAWLEN	  0x50
#define DW_SPI_AXIARLEN	  0x54
#define DW_SPI_DR	  0x60
#define DW_SPI_SPI_CTRLR0 0xf4
#define DW_SPI_SPIDR	  0x120
#define DW_SPI_SPIAR	  0x124
#define DW_SPI_AXIAR0	  0x128

/* Flash opcodes. */
#define SPINOR_OP_WRDI		0x04
#define SPINOR_OP_WREN		0x06
#define SPINOR_OP_RDSR		0x05
#define SPINOR_OP_PP_1_1_4	0x32
#define SPINOR_OP_READ_1_1_4	0x6b
#define SPINOR_OP_PP_1_1_4_4B	0x34
#define SPINOR_OP_READ_1_1_4_4B 0x6c
#define SPINOR_OP_BE_4K		0x20
#define SPINOR_OP_BE_4K_4B	0x21
#define SPINOR_OP_RDID		0x9f
#define SPINOR_OP_BE_32K	0x52
#define SPINOR_OP_BE_32K_4B	0x5c
#define SPINOR_OP_SE		0xd8
#define SPINOR_OP_SE_4B		0xdc

#define SPINOR_OP_EN4B 0xb7 /* Enable 4-byte address mode */
#define SPINOR_OP_EX4B 0xe9 /* Exit 4-byte mode */

/* POLL */
#define POLL_TIMEOUT_MS 1000
#define POLL_DELAY_MIN	50
#define POLL_DELAY_MAX	100

/* DW_SPI_SSIENR value */
#define DW_SPI_SSIENR_ENABLE  0x1U
#define DW_SPI_SSIENR_DISABLE 0x0U

/* DW_SPI_DMACR value */
#define DW_SPI_DMACR_IDMAE	0x4U
#define DW_SPI_DMACR_AINC	0x40U
#define DW_SPI_DMACR_ATW_MASK	0x18
#define DW_SPI_DMACR_ATW_OFFSET 3

/* DW_SPI_AXIAWLEN */
#define DW_SPI_AXIAWLEN_AWLEN 0x3f /* 0x1f */
/* DW_SPI_AXIARLEN */
#define DW_SPI_AXIARLEN_ARLEN 0x3f /* 0x1f */

enum {
	DW_SPI_DMA_READ,
	DW_SPI_DMA_WRITE,
	DW_SPI_DMA_MAX,
};

/* DW_SPI_CTRLR0 value */
#define DW_SPI_CTRLR0_STANDARD_TX 0x407U
#define DW_SPI_CTRLR0_STANDARD_RX 0x807U
#define DW_SPI_CTRLR0_QUAD_TX	  0x80041fU
#define DW_SPI_CTRLR0_QUAD_RX	  0x80081fU

/* DW_SPI_SPI_CTRLR0 value */
#define DW_SPI_SPI_CTRLR0_REG		     0x40000200U
#define DW_SPI_SPI_CTRLR0_ADDR_L24	     0x40000218U
#define DW_SPI_SPI_CTRLR0_ADDR_L32	     0x40000220U
#define DW_SPI_SPI_CTRLR0_WAIT_CYCLES_OFFSET 11
#define DW_SPI_SPI_CTRLR0_WAIT_CYCLES	     0x8U

/* DW_SPI_TXFTLR value */
#define DW_SPI_TXFTLR_TFT	       0xfU
#define DW_SPI_TXFTLR_TXFTHR_ENTRIES_2 (0x2U << 16 | 0xfU)
#define DW_SPI_TXFTLR_TXFTHR_ENTRIES_1 (0x1U << 16 | 0xfU)

/* DW_SPI_CTRLR1 value */
#define DW_SPI_CTRLR1_NDF 0x0U

/* DW_SPI_SER value */
#define DW_SPI_SER_ENABLE 0x1U

int xh2a_register_flash_driver(uint32_t flash_id, const char *name,
			       struct xh2a_flash_ops *ops);
int xh2a_qspi_writel(struct xh2a_qspi_dev *qspi_dev, uint32_t reg,
		     uint32_t val);
int xh2a_qspi_readl(struct xh2a_qspi_dev *qspi_dev, uint32_t reg,
		    uint32_t *val);

#endif
