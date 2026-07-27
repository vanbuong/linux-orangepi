// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <linux/kref.h>
#include <linux/pm_runtime.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>

#include "xh2a_qspi.h"
#include "xh2a_qspi_dev.h"
#include "nor_flash/xh2a_flash.h"
#include "xh2a_sat.h"
#include "axi_crc/xh2a_axi_crc.h"

int xh2a_qspi_writel(struct xh2a_qspi_dev *qspi_dev, uint32_t reg, uint32_t val)
{
	uint64_t addr = qspi_dev->xh2a_xspi_base + reg;

	return xh2a_pcie_pio_writel(qspi_dev->private_data, addr, val);
}

int xh2a_qspi_readl(struct xh2a_qspi_dev *qspi_dev, uint32_t reg, uint32_t *val)
{
	uint64_t addr = qspi_dev->xh2a_xspi_base + reg;

	return xh2a_pcie_pio_readl(qspi_dev->private_data, addr, val);
}

/*
 * xh2a_map_system() - Convert the ddr address of u7 to the ddr address of e2
 * @qspi_dev: qspi handle
 * @paddr: the ddr address of u7
 * @bytes: DDR address size
 * Return: SAT converted address
 */
static uintptr_t xh2a_map_system(struct xh2a_qspi_dev *qspi_dev, uint64_t paddr,
				 uint32_t bytes)
{
	uintptr_t v_paddr;
	uint32_t upper;

	if (bytes > AOSS_SAT_MAP_AREA_SIZE) {
		dev_err(qspi_dev->miscdev.this_device,
			"map mem size more than 0x%lx\n",
			AOSS_SAT_MAP_AREA_SIZE);
		return 0;
	}

	v_paddr = (paddr & AOSS_SAT_MAP_AREA_MASK) + E21_EXT_SYS_BASE;
	upper = (paddr >> AOSS_SAT_LOWER_ADDR_BITS);

	xh2a_pcie_pio_writel(qspi_dev->private_data,
			     AOSS_SAT_BASE_Q + AOSS_SAT_REG_OFFSET_XSPI, upper);

	return v_paddr;
}

static uint32_t xh2a_unmap_system(struct xh2a_qspi_dev *qspi_dev)
{
	return xh2a_pcie_pio_writel(qspi_dev->private_data,
				    AOSS_SAT_BASE_Q + AOSS_SAT_REG_OFFSET_XSPI,
				    0);
}

static void xh2a_qspi_safe_release(struct kref *kref)
{
	struct xh2a_qspi_dev *qspi_dev =
		container_of(kref, struct xh2a_qspi_dev, refcount);

	kfree(qspi_dev);
}

static int xh2a_qspi_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct mm_struct *mm = current->mm;
	struct xh2a_qspi_dev *qspi_dev = NULL;
	int major = MAJOR(inode->i_rdev);
	int minor = MINOR(inode->i_rdev);

	qspi_dev = container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	if (!qspi_dev) {
		dev_err(miscdev->this_device, "%s: invalid device\n", __func__);
		return -ENODEV;
	}

	if (atomic_read(&qspi_dev->removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&qspi_dev->refcount) == 0) {
		pr_err("%s: device is removing...\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&qspi_dev->qspi_mutex);
	if (atomic_read(&qspi_dev->opened)) {
		mutex_unlock(&qspi_dev->qspi_mutex);
		dev_err(miscdev->this_device, "%s: device is already opened\n",
			__func__);
		kref_put(&qspi_dev->refcount, xh2a_qspi_safe_release);
		return -EBUSY;
	}
	mutex_unlock(&qspi_dev->qspi_mutex);

	mutex_lock(&qspi_dev->qspi_mutex);
	atomic_set(&qspi_dev->opened, 1);
	mutex_unlock(&qspi_dev->qspi_mutex);

	dev_dbg(miscdev->this_device, "%s: major=%d, minor=%d\n", __func__,
		major, minor);
	dev_dbg(miscdev->this_device, "%s: client: %s (%d)\n", __func__,
		current->comm, current->pid);
	dev_dbg(miscdev->this_device, "%s: mmap  section: s: 0x%lx\n", __func__,
		mm->mmap_base);

	return 0;
}

static int xh2a_qspi_release(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct xh2a_qspi_dev *qspi_dev = NULL;
	int major = MAJOR(inode->i_rdev);
	int minor = MINOR(inode->i_rdev);

	filp->private_data = NULL;

	if (!miscdev) {
		pr_err("%s: miscdev is NULL\n", __func__);
		return -EINVAL;
	}

	qspi_dev = container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	if (!qspi_dev) {
		dev_err(miscdev->this_device, "%s: invalid device\n", __func__);
		return -ENODEV;
	}

	dev_dbg(miscdev->this_device, "%s: major=%d, minor=%d\n", __func__,
		major, minor);

	mutex_lock(&qspi_dev->qspi_mutex);
	atomic_set(&qspi_dev->opened, 0);
	mutex_unlock(&qspi_dev->qspi_mutex);

	kref_put(&qspi_dev->refcount, xh2a_qspi_safe_release);

	return 0;
}

/*
 * xh2a_dwc_ssi_enable() - Enable or disable the SSI
 * (Synchronous Serial Interface)
 * @qspi_dev: QSPI device handle
 * @enable: Flag to enable or disable the SSI interface (0 to disable, non-zero
 *          to enable)
 * Return: 0 on success, or a negative error code on failure
 *
 * This function writes the provided enable/disable flag to the SSIENR register
 * , and then verifies that the register was successfully updated by polling
 * the register until the expected value is set, or a timeout occurs.
 */
static int xh2a_dwc_ssi_enable(struct xh2a_qspi_dev *qspi_dev, uint32_t enable)
{
	uint32_t data = 0x0;
	unsigned long timeout =
		jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);

	xh2a_qspi_writel(qspi_dev, DW_SPI_SSIENR, enable);

	/* DMA mode does not read back */
	if (!qspi_dev->is_dma) {
		/* Poll the register to check if the operation was successful */
		do {
			xh2a_qspi_readl(qspi_dev, DW_SPI_SSIENR, &data);

			if (enable == data)
				return 0;

		} while (time_before(jiffies, timeout));

		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to set SSIENR register to %u within %d "
			"ms\n",
			__func__, enable, qspi_dev->poll_timeout_ms);

		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * xh2a_dwc_ssi_tx_finish() - Wait for the TX (transmission) to finish
 * on the QSPI bus by polling the status register (SR)
 * @qspi_dev: QSPI device handle
 * Return: 0 on success, or a negative error code on failure
 *
 * This function continuously polls the QSPI bus status register (SR) to check
 * if the TX operation has completed successfully. The function waits for the
 * specific status bits to indicate the completion of the transmission.
 */
static int xh2a_dwc_ssi_tx_finish(struct xh2a_qspi_dev *qspi_dev)
{
	uint32_t status = 0x0;
	unsigned long timeout =
		jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);

	/*
	 * Poll the QSPI status register until the transmission is complete or
	 * a timeout occurs
	 */
	do {
		xh2a_qspi_readl(qspi_dev, DW_SPI_SR, &status);

		/*
		 * Check if the status indicates that the TX operation is
		 * complete (status bits 2 and 0 must be 1 and 0 respectively)
		 */
		if ((status & 0x5) == 0x4)
			return 0;

		if (qspi_dev->is_dma)
			usleep_range(1, 5);
		else
			usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);

	} while (time_before(jiffies, timeout));

	dev_err(qspi_dev->miscdev.this_device,
		"%s: Failed to wait qspi bus SR register to %d within %d ms\n",
		__func__, status, qspi_dev->poll_timeout_ms);

	return -ETIMEDOUT;
}

/*
 * xh2a_dwc_ssi_dma_tx_finish() - Wait for the DMA-based TX
 * (transmission) to finish by polling the SSIENR register
 * @qspi_dev: QSPI device handle
 * Return: 0 on success, or a negative error code on failure
 */
static int xh2a_dwc_ssi_dma_tx_finish(struct xh2a_qspi_dev *qspi_dev)
{
	uint32_t status = 0x0;
	unsigned long timeout =
		jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);

	do {
		xh2a_qspi_readl(qspi_dev, DW_SPI_SSIENR, &status);

		/*
		 * Check if the status indicates that the DMA transmission is
		 * complete (the register should be cleared)
		 */
		if ((status & 0x1) == 0)
			return 0;

	} while (time_before(jiffies, timeout));

	dev_err(qspi_dev->miscdev.this_device,
		"%s: Timeout waiting for DW_SPI_SSIENR to clear within %d ms\n",
		__func__, qspi_dev->poll_timeout_ms);

	return -ETIMEDOUT;
}

/*
 * xh2a_dwc_ssi_internal_dma_init() - Initialize the internal DMA settings
 * for the QSPI device
 * @qspi_dev: QSPI device handle
 * @dmacr_atw: Value for the ATW field in the DMA control register
 * @dir: Direction of the DMA transfer (either read or write)
 * Return: 0 on success, or a negative error code on failure
 *
 * This function initializes the DMA control register (DMACR) and the AXI
 * address width register based on the provided parameters.
 */
static int xh2a_dwc_ssi_internal_dma_init(struct xh2a_qspi_dev *qspi_dev,
					  uint32_t dmacr_atw, uint32_t dir)
{
	uint32_t reg = 0x0;

	xh2a_qspi_readl(qspi_dev, DW_SPI_DMACR, &reg);
	/* reg = 0x2010; */
	reg &= ~DW_SPI_DMACR_ATW_MASK;
	reg = reg | DW_SPI_DMACR_IDMAE |
	      (dmacr_atw << DW_SPI_DMACR_ATW_OFFSET) | DW_SPI_DMACR_AINC;
	xh2a_qspi_writel(qspi_dev, DW_SPI_DMACR, reg);

	if (dir == DW_SPI_DMA_READ)
		xh2a_qspi_writel(qspi_dev, DW_SPI_AXIAWLEN,
				 DW_SPI_AXIAWLEN_AWLEN << 8);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_AXIARLEN,
				 DW_SPI_AXIARLEN_ARLEN << 8);

	return 0;
}

/*
 * xh2a_spi_nor_write_enable() - Enable write operation for SPI NOR flash.
 * @qspi_dev: QSPI device handle
 * Return: 0 on success, or a negative error code on failure
 *
 * This function enables the write operation for the SPI NOR flash by:
 * 1. Disabling the QSPI controller.
 * 2. Configuring various control registers for the write operation.
 * 3. Enabling the QSPI controller.
 * 4. Sending the write enable (WREN) command to the flash.
 */
static int xh2a_spi_nor_write_enable(struct xh2a_qspi_dev *qspi_dev)
{
	int ret = 0;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		goto err_out;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_STANDARD_TX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, DW_SPI_CTRLR1_NDF);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0, DW_SPI_SPI_CTRLR0_REG);
	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR, DW_SPI_TXFTLR_TFT);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable DWC_ssi\n", __func__);
		goto err_out;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_WREN);

	ret = xh2a_dwc_ssi_tx_finish(qspi_dev);

err_out:
	return ret;
}

/*
 * xh2a_spi_nor_write_disable() - Disable write operation for SPI NOR flash.
 * @qspi_dev: QSPI device handle
 * Return: 0 on success, or a negative error code on failure
 *
 * This function disables the write operation for the SPI NOR flash by:
 * 1. Disabling the QSPI controller.
 * 2. Configuring various control registers for the write disable operation.
 * 3. Enabling the QSPI controller.
 * 4. Sending the write disable (WRDI) command to the flash.
 */
static int xh2a_spi_nor_write_disable(struct xh2a_qspi_dev *qspi_dev)
{
	int ret = 0x0;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		goto err_out;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_STANDARD_TX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, DW_SPI_CTRLR1_NDF);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0, DW_SPI_SPI_CTRLR0_REG);
	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR, DW_SPI_TXFTLR_TFT);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable DWC_ssi\n", __func__);
		goto err_out;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_WRDI);

	ret = xh2a_dwc_ssi_tx_finish(qspi_dev);

err_out:
	return ret;
}

/*
 * xh2a_spi_nor_read_sr_reg() - Read status register from SPI NOR flash.
 * @qspi_dev: QSPI device handle
 * @rx_buff: Buffer to store received data
 * @rx_len: Length of data to read
 * Return: 0 on success, or a negative error code on failure
 *
 * This function reads the status register (RDSR) from the SPI NOR flash by:
 * 1. Sending the RDSR command.
 * 2. Reading the data into the provided buffer in chunks.
 * 3. Handling FIFO to transfer data efficiently.
 */
static int xh2a_spi_nor_read_sr_reg(struct xh2a_qspi_dev *qspi_dev,
				    uint8_t *rx_buff, uint32_t rx_len)
{
	uint32_t index, fifo_len;
	uint32_t v_rx_len;
	uint32_t rxflr = 0x0;
	uint32_t data = 0x0;
	int i = 0;
	unsigned long timeout =
		jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_RDSR);
	v_rx_len = rx_len;

	while (v_rx_len) {
		if (time_after(jiffies, timeout)) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: timeout while reading data\n", __func__);
			return -ETIMEDOUT;
		}

		xh2a_qspi_readl(qspi_dev, DW_SPI_RXFLR, &rxflr);
		fifo_len = min(rxflr, v_rx_len);

		for (index = 0; index < fifo_len; index++) {
			xh2a_qspi_readl(qspi_dev, DW_SPI_DR, &data);
			rx_buff[i++] = (uint8_t)data;
		}

		v_rx_len -= fifo_len;

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	}

	return 0;
}

/*
 * xh2a_spi_nor_sr_ready() - Wait for the SPI NOR flash status register
 * to be ready.
 * @qspi_dev: QSPI device handle
 * Return: 0 if the status register indicates readiness,
 *      or a negative error code.
 *
 * This function waits for the status register (SR) of the SPI NOR flash to be
 * ready by polling the status register until it clears the busy flag (bit 0).
 */
static int xh2a_spi_nor_sr_ready(struct xh2a_qspi_dev *qspi_dev)
{
	int ret = 0;
	uint8_t rx_buff[1] = { 0 };
	unsigned long timeout;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_STANDARD_RX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, DW_SPI_CTRLR1_NDF);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0, DW_SPI_SPI_CTRLR0_REG);
	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR, DW_SPI_TXFTLR_TFT);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable DWC_ssi\n", __func__);
		return ret;
	}

	timeout = jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);

	do {
		ret = xh2a_spi_nor_read_sr_reg(qspi_dev, rx_buff, 1);

		if (ret < 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: Failed to read status register\n",
				__func__);
			return ret;
		}

		/* Check if the status register bit 0 (busy flag) is cleared */
		if ((rx_buff[0] & 0x1) == 0x0)
			return 0;

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	} while (time_before(jiffies, timeout));

	dev_err(qspi_dev->miscdev.this_device,
		"%s: poll status register timeout, status = 0x%x\n", __func__,
		rx_buff[0]);

	return -ETIMEDOUT;
}

/*
 * xh2a_spi_nor_dma_write_data() - Write data to SPI NOR flash using DMA.
 * @qspi_dev: QSPI device handle
 * @offset: Memory offset where data should be written
 * @buf: Pointer to the data buffer to be written
 * @len: Length of the data to be written
 * Return: 0 on success, negative error code on failure.
 *
 * This function writes data to the SPI NOR flash memory using DMA. It handles
 * both 3-byte and 4-byte addressing modes and configures the necessary
 * DMA registers to transfer data efficiently.
 */
static int xh2a_spi_nor_dma_write_data(struct xh2a_qspi_dev *qspi_dev,
				       uint32_t offset, const void *buf,
				       uint32_t len)
{
	int ret = 0;
	uint32_t v_tx_len;
	uint32_t dmacr_reg = 0x0;

	v_tx_len = len / 0x4;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_QUAD_TX);
	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, v_tx_len - 1);
	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to configure QSPI controller\n", __func__);
		return ret;
	}

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0,
				 DW_SPI_SPI_CTRLR0_ADDR_L32);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0,
				 DW_SPI_SPI_CTRLR0_ADDR_L24);

	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR,
			 DW_SPI_TXFTLR_TXFTHR_ENTRIES_1);

	/* Initialize DMA for write operation */
	xh2a_dwc_ssi_internal_dma_init(qspi_dev, 0x2, DW_SPI_DMA_WRITE);

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPIDR, SPINOR_OP_PP_1_1_4_4B);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPIDR, SPINOR_OP_PP_1_1_4);

	xh2a_qspi_writel(qspi_dev, DW_SPI_SPIAR, offset);
	xh2a_qspi_writel(qspi_dev, DW_SPI_AXIAR0, (uint32_t)(uintptr_t)buf);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable SPI\n", __func__);
		return ret;
	}

	/* Wait for DMA transmission to complete */
	ret = xh2a_dwc_ssi_dma_tx_finish(qspi_dev);
	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to complete DMA transmission\n", __func__);
	}

	ret = xh2a_dwc_ssi_tx_finish(qspi_dev);
	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to complete TX transmission\n", __func__);
	}

	/* Disable the DMA transfer by clearing the IDMAE bit */
	xh2a_qspi_readl(qspi_dev, DW_SPI_DMACR, &dmacr_reg);
	dmacr_reg &= ~DW_SPI_DMACR_IDMAE;
	xh2a_qspi_writel(qspi_dev, DW_SPI_DMACR, dmacr_reg);

	return ret;
}

/*
 * xh2a_spi_nor_cpu_write_data() - Write data to SPI NOR flash.
 * @qspi_dev: QSPI device handle
 * @offset: Memory offset where data should be written
 * @buf: Pointer to the data buffer to be written
 * @len: Length of the data to be written
 * Return: 0 on success, negative error code on failure.
 *
 * This function writes data to the SPI NOR flash memory starting from the
 * given offset. It handles both 3-byte and 4-byte addressing modes and
 * manages the SPI FIFO during the write process.
 */
static int xh2a_spi_nor_cpu_write_data(struct xh2a_qspi_dev *qspi_dev,
				       uint32_t offset, const void *buf,
				       uint32_t len)
{
	int ret = 0;
	uint32_t index, fifo_len;
	uint32_t tx_len, v_tx_len;
	uint32_t txflr = 0x0;
	uint32_t i = 0;
	uint32_t rx_buf = 0x0;
	unsigned long timeout;

	v_tx_len = len / 0x4;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_QUAD_TX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, v_tx_len - 1);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0,
				 DW_SPI_SPI_CTRLR0_ADDR_L32);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0,
				 DW_SPI_SPI_CTRLR0_ADDR_L24);

	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR,
			 DW_SPI_TXFTLR_TXFTHR_ENTRIES_2);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable DWC_ssi\n", __func__);
		return ret;
	}

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_PP_1_1_4_4B);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_PP_1_1_4);

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, offset);

	timeout = jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);

	tx_len = len;

	while (tx_len) {
		if (time_after(jiffies, timeout)) {
			dev_err(qspi_dev->miscdev.this_device, "spi_nor_write_"
							       "data: Timeout "
							       "while waiting "
							       "for FIFO "
							       "space\n");
			return -ETIMEDOUT;
		}

		xh2a_qspi_readl(qspi_dev, DW_SPI_TXFLR, &txflr);
		fifo_len = min(32 - txflr, tx_len);

		fifo_len = fifo_len / 4 * 4;

		for (index = 0; index < fifo_len / 4; index++) {
			rx_buf = ((uint32_t *)buf)[i++];
			xh2a_qspi_writel(qspi_dev, DW_SPI_DR, rx_buf);
		}

		tx_len -= fifo_len;

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	}

	return ret;
}

/*
 * xh2a_spi_nor_erase_data() - Erase data from SPI NOR flash memory.
 * @qspi_dev: QSPI device handle
 * @offset: Memory offset where the erase operation should start
 * @len: Length of the data to be erased (not used in this function)
 * Return: 0 on success, negative error code on failure.
 *
 * This function erases data from the SPI NOR flash memory, either using
 * 3-byte or 4-byte addressing depending on the device configuration.
 * It sends the appropriate erase command to the flash device and waits
 * for the operation to finish.
 */
static int xh2a_spi_nor_erase_data(struct xh2a_qspi_dev *qspi_dev,
				   uint32_t offset, uint32_t len)
{
	int ret = 0;
	uint32_t erase_cmd = 0;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_STANDARD_TX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0,
				 DW_SPI_SPI_CTRLR0_ADDR_L32);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0,
				 DW_SPI_SPI_CTRLR0_ADDR_L24);

	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR,
			 DW_SPI_TXFTLR_TXFTHR_ENTRIES_1);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable SPI\n", __func__);
		return ret;
	}

	if (qspi_dev->is_4byte_address) {
		switch (len) {
		case SZ_64K:
			erase_cmd = SPINOR_OP_SE_4B;
			break;
		case SZ_32K:
			if (qspi_dev->flash_id == XH2A_QSPI_NORFLASH_GIGADEVICE)
				erase_cmd = SPINOR_OP_BE_32K_4B;
			else
				erase_cmd = SPINOR_OP_BE_32K;
			break;
		case SZ_4K:
			erase_cmd = SPINOR_OP_BE_4K_4B;
			break;
		default:
			dev_err(qspi_dev->miscdev.this_device,
				"%s: Unsupported erase command length: %d\n",
				__func__, len);
			return -EINVAL;
		}
		xh2a_qspi_writel(qspi_dev, DW_SPI_DR, erase_cmd);
	} else {
		switch (len) {
		case SZ_64K:
			erase_cmd = SPINOR_OP_SE;
			break;
		case SZ_32K:
			erase_cmd = SPINOR_OP_BE_32K;
			break;
		case SZ_4K:
			erase_cmd = SPINOR_OP_BE_4K;
			break;
		default:
			dev_err(qspi_dev->miscdev.this_device,
				"%s: Unsupported erase command length: %d\n",
				__func__, len);
			return -EINVAL;
		}
		xh2a_qspi_writel(qspi_dev, DW_SPI_DR, erase_cmd);
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, offset);

	return ret;
}

/*
 * spi_nor_read_data() - Read data from the SPI NOR flash memory.
 * @qspi_dev: QSPI device handle
 * @offset: Memory offset from where to start reading
 * @buf: Buffer to store the read data
 * @len: Length of data to read
 * Return: 0 on success, negative error code on failure.
 *
 * This function reads data from the SPI NOR flash memory into the provided
 * buffer. It configures the SPI controller to use either 3-byte or 4-byte
 * addressing, depending on the device configuration. It then reads the
 * requested data from the flash and stores it in the buffer.
 */
static int xh2a_spi_nor_cpu_read_data(struct xh2a_qspi_dev *qspi_dev,
				      uint32_t offset, void *buf, uint32_t len)
{
	int ret = 0;
	uint32_t index, fifo_len, i = 0;
	uint32_t rxflr = 0x0;
	uint32_t data = 0x0;
	uint32_t v_rx_len;
	unsigned long timeout;

	v_rx_len = len / 0x4;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_QUAD_RX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, v_rx_len - 1);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(
			qspi_dev, DW_SPI_SPI_CTRLR0,
			DW_SPI_SPI_CTRLR0_ADDR_L32 |
				(DW_SPI_SPI_CTRLR0_WAIT_CYCLES
				 << DW_SPI_SPI_CTRLR0_WAIT_CYCLES_OFFSET));
	else
		xh2a_qspi_writel(
			qspi_dev, DW_SPI_SPI_CTRLR0,
			DW_SPI_SPI_CTRLR0_ADDR_L24 |
				(DW_SPI_SPI_CTRLR0_WAIT_CYCLES
				 << DW_SPI_SPI_CTRLR0_WAIT_CYCLES_OFFSET));

	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR,
			 DW_SPI_TXFTLR_TXFTHR_ENTRIES_1);
	xh2a_qspi_writel(qspi_dev, DW_SPI_RXFTLR, 0xf);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable SPI\n", __func__);
		return ret;
	}

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_READ_1_1_4_4B);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_READ_1_1_4);

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, offset);

	timeout = jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);
	/* Read the data into the buffer */
	while (v_rx_len) {
		if (time_after(jiffies, timeout)) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: Timeout while waiting for FIFO space\n",
				__func__);
			return -ETIMEDOUT;
		}

		xh2a_qspi_readl(qspi_dev, DW_SPI_RXFLR, &rxflr);
		fifo_len = min(rxflr, v_rx_len);

		for (index = 0; index < fifo_len; index++) {
			xh2a_qspi_readl(qspi_dev, DW_SPI_DR, &data);
			((uint32_t *)buf)[i++] = (uint32_t)data;
		}

		v_rx_len -= fifo_len;

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	}

	return 0;
}

/*
 * xh2a_spi_nor_dma_read_data() - Read data from SPI NOR flash using DMA.
 * @qspi_dev: QSPI device handle
 * @offset: Memory offset from where to start reading
 * @buf: Buffer to store the read data
 * @len: Length of data to read
 * Return: 0 on success, negative error code on failure.
 */
static int xh2a_spi_nor_dma_read_data(struct xh2a_qspi_dev *qspi_dev,
				      uint32_t offset, void *buf, uint32_t len)
{
	int ret = 0;
	uint32_t v_rx_len;
	uint32_t dmacr_reg = 0x0;

	v_rx_len = len / 0x4;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_QUAD_RX);
	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, v_rx_len - 1);
	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	if (qspi_dev->is_4byte_address)
		ret |= xh2a_qspi_writel(
			qspi_dev, DW_SPI_SPI_CTRLR0,
			DW_SPI_SPI_CTRLR0_ADDR_L32 |
				(DW_SPI_SPI_CTRLR0_WAIT_CYCLES
				 << DW_SPI_SPI_CTRLR0_WAIT_CYCLES_OFFSET));
	else
		ret |= xh2a_qspi_writel(
			qspi_dev, DW_SPI_SPI_CTRLR0,
			DW_SPI_SPI_CTRLR0_ADDR_L24 |
				(DW_SPI_SPI_CTRLR0_WAIT_CYCLES
				 << DW_SPI_SPI_CTRLR0_WAIT_CYCLES_OFFSET));

	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR,
				DW_SPI_TXFTLR_TXFTHR_ENTRIES_1);
	ret |= xh2a_qspi_writel(qspi_dev, DW_SPI_RXFTLR, 0xf);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to configure SPI registers\n", __func__);
		return ret;
	}

	/*
	 * Initialize the internal DMA settings for the QSPI device
	 * and enable the DMA transfer
	 */
	xh2a_dwc_ssi_internal_dma_init(qspi_dev, 0x2, DW_SPI_DMA_READ);

	if (qspi_dev->is_4byte_address)
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPIDR,
				 SPINOR_OP_READ_1_1_4_4B);
	else
		xh2a_qspi_writel(qspi_dev, DW_SPI_SPIDR, SPINOR_OP_READ_1_1_4);

	xh2a_qspi_writel(qspi_dev, DW_SPI_SPIAR, offset);
	xh2a_qspi_writel(qspi_dev, DW_SPI_AXIAR0, (uint32_t)(uintptr_t)buf);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable SPI\n", __func__);
		return ret;
	}

	/* Wait for DMA transmission to complete */
	ret = xh2a_dwc_ssi_dma_tx_finish(qspi_dev);

	/* Disable the DMA transfer by clearing the IDMAE bit */
	xh2a_qspi_readl(qspi_dev, DW_SPI_DMACR, &dmacr_reg);
	dmacr_reg &= ~DW_SPI_DMACR_IDMAE;
	xh2a_qspi_writel(qspi_dev, DW_SPI_DMACR, dmacr_reg);

	return ret;
}

/*
 * xh2a_check_result() - Check the programming/erasing result of the SPI NOR
 * flash.
 * @qspi_dev: QSPI device handle
 * Return: 0 if programming succeeded, -1 if programming failed,
 *      or a negative error code.
 *
 * This function checks the result of the previous SPI NOR flash programming
 * operation by reading the status register and interpreting the result based
 * on the device type.
 */
static int xh2a_check_result(struct xh2a_qspi_dev *qspi_dev,
			     bool is_programming)
{
	int ret = 0;
	struct xh2a_flash_ops *flash_ops = &qspi_dev->ops;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_STANDARD_RX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, DW_SPI_CTRLR1_NDF);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0, DW_SPI_SPI_CTRLR0_REG);
	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR, DW_SPI_TXFTLR_TFT);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable DWC_ssi\n", __func__);
		return ret;
	}

	if (is_programming)
		return flash_ops->program_result ?
			       flash_ops->program_result(qspi_dev) :
			       0;
	else
		return flash_ops->erase_result ?
			       flash_ops->erase_result(qspi_dev) :
			       0;
}

/*
 * xh2a_spi_nor_read_chip() - Read data from the SPI NOR flash memory.
 * @qspi_dev: QSPI device handle
 * @param: Read operation parameters
 * Return: 0 on success, negative error code on failure.
 *
 * This function reads data from the SPI NOR flash memory into the provided
 * buffer. It configures the SPI controller to use either 3-byte or 4-byte
 * addressing, depending on the device configuration. It then reads the
 * requested data from the flash and stores it in the buffer.
 */
static int xh2a_spi_nor_read_chip(struct xh2a_qspi_dev *qspi_dev,
				  struct xh2a_qspi_op *param)
{
	int ret = -EINVAL;
	void *buf;
	uint32_t read_len = 0;
	uint32_t remaining_len = param->size;
	uint32_t current_offset = param->offset;

	if (qspi_dev->is_dma) {
		qspi_dev->device_paddr = xh2a_map_system(
			qspi_dev, qspi_dev->device_paddr, param->size);

		if (qspi_dev->device_paddr == 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: sat failed\n", __func__);
			return -EINVAL;
		}
	}

	if (qspi_dev->is_dma == 0)
		buf = param->data;
	else
		buf = (void *)(uintptr_t)qspi_dev->device_paddr;

	while (remaining_len) {
		read_len = min(remaining_len, (uint32_t)SZ_256K);

		if (qspi_dev->is_dma)
			ret = xh2a_spi_nor_dma_read_data(
				qspi_dev, current_offset, buf, read_len);
		else
			ret = xh2a_spi_nor_cpu_read_data(
				qspi_dev, current_offset, buf, read_len);

		if (ret < 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: Read data failed\n", __func__);

			if (qspi_dev->is_dma)
				xh2a_unmap_system(qspi_dev);

			return ret;
		}

		remaining_len -= read_len;
		current_offset += read_len;
		buf = (uint8_t *)buf + read_len;
	}

	if (qspi_dev->is_dma)
		xh2a_unmap_system(qspi_dev);

	return ret;
}

static int xh2a_spi_nor_program(struct xh2a_qspi_dev *qspi_dev, uint32_t offset,
				void *buf, uint32_t len)
{
	int ret;

	ret = xh2a_spi_nor_write_enable(qspi_dev);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Write enable failed\n", __func__);
		return ret;
	}

	if (qspi_dev->is_dma) {
		ret = xh2a_spi_nor_dma_write_data(qspi_dev, offset, buf, len);

		if (ret < 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: DMA program failed\n", __func__);
			return ret;
		}
	} else {
		ret = xh2a_spi_nor_cpu_write_data(qspi_dev, offset, buf, len);

		if (ret < 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: CPU program failed\n", __func__);
			return ret;
		}

		ret = xh2a_dwc_ssi_tx_finish(qspi_dev);

		if (ret < 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: TX finish failed\n", __func__);
		}
	}

	ret = xh2a_spi_nor_sr_ready(qspi_dev);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Waiting for bus idle failed\n", __func__);
		return ret;
	}

	return xh2a_check_result(qspi_dev, true);
}

static int xh2a_spi_nor_erase(struct xh2a_qspi_dev *qspi_dev, uint32_t offset,
			      uint32_t len)
{
	int ret;

	ret = xh2a_spi_nor_write_enable(qspi_dev);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Write enable failed\n", __func__);
		return ret;
	}

	ret = xh2a_spi_nor_erase_data(qspi_dev, offset, len);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: erase flash failed\n", __func__);
		return ret;
	}

	ret = xh2a_dwc_ssi_tx_finish(qspi_dev);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device, "%s: TX finish failed\n",
			__func__);
	}

	ret = xh2a_spi_nor_sr_ready(qspi_dev);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Waiting for bus idle failed\n", __func__);
		return ret;
	}

	ret = xh2a_spi_nor_write_disable(qspi_dev);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Write disable failed\n", __func__);
		return ret;
	}

	return xh2a_check_result(qspi_dev, false);
}

/*
 * xh2a_spi_nor_set_4b_mode() - Set the SPI NOR flash to 4-byte addressing mode.
 * @qspi_dev: QSPI device handle
 * @enable: Boolean to enable or disable 4-byte addressing mode
 * Return: 0 on success, negative error code on failure.
 */
static int xh2a_spi_nor_set_4b_mode(struct xh2a_qspi_dev *qspi_dev, bool enable)
{
	int ret;
	uint32_t command = enable ? SPINOR_OP_EN4B : SPINOR_OP_EX4B;

	if (!qspi_dev->is_4byte_address ||
	    qspi_dev->flash_id == XH2A_QSPI_NORFLASH_GIGADEVICE)
		return 0;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_STANDARD_TX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, DW_SPI_CTRLR1_NDF);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0, DW_SPI_SPI_CTRLR0_REG);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);
	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR, DW_SPI_TXFTLR_TFT);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable SPI\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, command);

	ret = xh2a_dwc_ssi_tx_finish(qspi_dev);

	return ret;
}

/*
 * xh2a_spi_nor_read_id() - Read the ID of the SPI NOR flash memory.
 * @qspi_dev: QSPI device handle
 * @id: Buffer to store the ID read from the flash
 * @len: Length of the ID to read
 * Return: 0 on success, negative error code on failure.
 *
 * This function sends the Read ID (RDID) command to the SPI NOR flash,
 * retrieves the flash ID, and stores it in the provided buffer.
 */
static int xh2a_spi_nor_read_id(struct xh2a_qspi_dev *qspi_dev, uint8_t *id,
				uint32_t len)
{
	int ret;
	uint32_t index, fifo_len, i = 0;
	uint32_t rxflr = 0x0;
	uint32_t data = 0x0;
	uint32_t v_rx_len = len;
	unsigned long timeout;

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_DISABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Disabling DWC_ssi fails\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR0, DW_SPI_CTRLR0_STANDARD_RX);
	xh2a_qspi_writel(qspi_dev, DW_SPI_CTRLR1, v_rx_len - 1);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SPI_CTRLR0, DW_SPI_SPI_CTRLR0_REG);
	xh2a_qspi_writel(qspi_dev, DW_SPI_SER, DW_SPI_SER_ENABLE);
	xh2a_qspi_writel(qspi_dev, DW_SPI_TXFTLR, DW_SPI_TXFTLR_TFT);

	ret = xh2a_dwc_ssi_enable(qspi_dev, DW_SPI_SSIENR_ENABLE);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to enable SPI\n", __func__);
		return ret;
	}

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, SPINOR_OP_RDID);

	timeout = jiffies + msecs_to_jiffies(qspi_dev->poll_timeout_ms);

	while (v_rx_len) {
		if (time_after(jiffies, timeout)) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: Timeout while reading data\n", __func__);
			return -ETIMEDOUT;
		}

		xh2a_qspi_readl(qspi_dev, DW_SPI_RXFLR, &rxflr);
		fifo_len = min(rxflr, v_rx_len);

		for (index = 0; index < fifo_len; index++) {
			xh2a_qspi_readl(qspi_dev, DW_SPI_DR, &data);
			id[i++] = data;
		}

		v_rx_len -= fifo_len;

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	}

	return 0;
}

static int xh2a_spi_nor_erase_chip(struct xh2a_qspi_dev *qspi_dev,
				   struct xh2a_qspi_op *param)
{
	uint32_t total_len = 0;
	uint32_t remaining = param->size;
	uint32_t offset = param->offset;
	uint32_t erase_size;
	int status = 0;

	status = xh2a_spi_nor_set_4b_mode(qspi_dev, true);
	if (status < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Failed to set 4-byte addressing mode\n", __func__);
		return status;
	}

	while (remaining > 0) {
		if (remaining >= SZ_64K && (offset & (SZ_64K - 1)) == 0) {
			erase_size = SZ_64K;
		} else if (remaining >= SZ_32K &&
			   (offset & (SZ_32K - 1)) == 0) {
			erase_size = SZ_32K;
		} else if (remaining >= SZ_4K && (offset & (SZ_4K - 1)) == 0) {
			erase_size = SZ_4K;
		} else {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: Unsupported erase size at offset 0x%x\n",
				__func__, offset);
			xh2a_spi_nor_set_4b_mode(qspi_dev, false);
			return -EINVAL;
		}

		status = xh2a_spi_nor_erase(qspi_dev, offset, erase_size);
		if (status != 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: SPI NOR erase failed at 0x%x (size: "
				"0x%x)\n",
				__func__, offset, erase_size);
			xh2a_spi_nor_set_4b_mode(qspi_dev, false);
			return status;
		}

		offset += erase_size;
		total_len += erase_size;
		remaining -= erase_size;
	}

	dev_dbg(qspi_dev->miscdev.this_device,
		"%s: Successfully erased 0x%x bytes (start: 0x%x)\n", __func__,
		total_len, param->offset);

	status = xh2a_spi_nor_set_4b_mode(qspi_dev, false);

	return status;
}

/*
 * xh2a_spi_nor_program_chip() - Program data to the SPI NOR flash chip.
 * @qspi_dev: QSPI device handle
 * @param: A structure containing the programming parameters
 * (offset, size, data)
 * Return: 0 on success, negative error code on failure.
 *
 * This function programs data to the NOR flash chip in pages. It handles both
 * direct memory access (DMA) and non-DMA programming. The programming occurs
 * in chunks, each corresponding to a page size.
 */
static int xh2a_spi_nor_program_chip(struct xh2a_qspi_dev *qspi_dev,
				     struct xh2a_qspi_op *param)
{
	uint32_t len, total_len;
	uint32_t req_len, offset;
	uint32_t page_size, page_mask;
	void *buf;
	int status = 0;

	/* Handle DMA programming if enabled */
	if (qspi_dev->is_dma) {
		qspi_dev->device_paddr = xh2a_map_system(
			qspi_dev, qspi_dev->device_paddr, param->size);

		if (qspi_dev->device_paddr == 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: sat failed\n", __func__);
			return -EINVAL;
		}
	}

	/* If DMA is not used, use the provided buffer directly */
	if (qspi_dev->is_dma == 0)
		buf = param->data;
	else
		buf = (void *)(uintptr_t)qspi_dev->device_paddr;

	req_len = param->size;
	offset = param->offset;
	total_len = 0;
	page_mask = NOR_PAGE_MASK(PROGRAM_PAGE_SIZE);
	page_size = PROGRAM_PAGE_SIZE - (offset & page_mask);

	/* Write data in pages */
	do {
		if ((req_len - total_len) < page_size)
			len = req_len - total_len;
		else
			len = page_size;

		status = xh2a_spi_nor_program(qspi_dev, offset, buf, len);

		if (status != 0) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: program at 0x%x failed (size: "
				"0x%x)\n",
				__func__, offset, len);

			if (qspi_dev->is_dma)
				xh2a_unmap_system(qspi_dev);

			return status;
		}

		offset += len;
		buf = (char *)buf + len;
		total_len += len;
		page_size = PROGRAM_PAGE_SIZE;
	} while (total_len < req_len);

	if (qspi_dev->is_dma)
		xh2a_unmap_system(qspi_dev);

	dev_dbg(qspi_dev->miscdev.this_device,
		"%s: Successfully programmed %u bytes (start: 0x%x)\n",
		__func__, total_len, param->offset);

	return 0;
}

static int xh2a_qspi_program(struct xh2a_qspi_dev *qspi_dev,
			     const struct xh2a_qspi_op __user *arg)
{
	struct xh2a_qspi_op ioc = { 0 };

	if (!arg) {
		dev_err(qspi_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);
		return -EINVAL;
	}

	if (copy_from_user(&ioc, arg, sizeof(ioc))) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	dev_dbg(qspi_dev->miscdev.this_device,
		"%s: Received offset=%u, size=%u\n", __func__, ioc.offset,
		ioc.size);

	if (ioc.size == 0 || ioc.size % 4 != 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Invalid size (size=%u)\n", __func__, ioc.size);
		return -EINVAL;
	}

	if (ioc.offset + ioc.size > SZ_32M) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Offset+size out of range (offset=%u, size=%u)\n",
			__func__, ioc.offset, ioc.size);
		return -EINVAL;
	}

	qspi_dev->is_4byte_address = (ioc.offset + ioc.size > SZ_16M);
	dev_dbg(qspi_dev->miscdev.this_device, "%s: is_4byte_address=%d\n",
		__func__, qspi_dev->is_4byte_address);

	return xh2a_spi_nor_program_chip(qspi_dev, &ioc);
}

static int xh2a_qspi_read(struct xh2a_qspi_dev *qspi_dev,
			  struct xh2a_qspi_op __user *arg)
{
	int ret;
	struct xh2a_qspi_op ioc = { 0 };

	if (!arg) {
		dev_err(qspi_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);
		return -EINVAL;
	}

	if (copy_from_user(&ioc, arg, sizeof(ioc))) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	dev_dbg(qspi_dev->miscdev.this_device,
		"%s: Received offset=%u, size=%u\n", __func__, ioc.offset,
		ioc.size);

	if (ioc.size == 0 || ioc.size % 4 != 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Invalid size (size=%u)\n", __func__, ioc.size);
		return -EINVAL;
	}

	if (ioc.offset + ioc.size > SZ_32M) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Offset+size out of range (offset=%u, size=%u)\n",
			__func__, ioc.offset, ioc.size);
		return -EINVAL;
	}

	qspi_dev->is_4byte_address = (ioc.offset + ioc.size > SZ_16M);
	dev_dbg(qspi_dev->miscdev.this_device, "%s: is_4byte_address=%d\n",
		__func__, qspi_dev->is_4byte_address);

	ret = xh2a_spi_nor_read_chip(qspi_dev, &ioc);

	if (ret < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: xh2a_spi_nor_read_chip failed,"
			" retval=%d\n",
			__func__, ret);
		return ret;
	}

	if (!qspi_dev->is_dma) {
		if (copy_to_user(arg, &ioc, sizeof(ioc))) {
			dev_err(qspi_dev->miscdev.this_device,
				"%s: copy_to_user failed\n", __func__);
			return -EFAULT;
		}
	}

	return 0;
}

static int xh2a_qspi_erase(struct xh2a_qspi_dev *qspi_dev,
			   const struct xh2a_qspi_op __user *arg)
{
	struct xh2a_qspi_op ioc = { 0 };

	if (!arg) {
		dev_err(qspi_dev->miscdev.this_device, "%s: Invalid arg\n",
			__func__);
		return -EINVAL;
	}

	if (copy_from_user(&ioc, arg, sizeof(ioc))) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	dev_dbg(qspi_dev->miscdev.this_device,
		"%s: Received offset=%u, size=%u\n", __func__, ioc.offset,
		ioc.size);

	if (ioc.size == 0 || ioc.size % SZ_4K != 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Invalid size (size=%u)\n", __func__, ioc.size);
		return -EINVAL;
	}

	if (ioc.offset + ioc.size > SZ_32M) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Offset+size out of range (offset=%u, size=%u)\n",
			__func__, ioc.offset, ioc.size);
		return -EINVAL;
	}

	qspi_dev->is_4byte_address = (ioc.offset + ioc.size > SZ_16M);
	dev_dbg(qspi_dev->miscdev.this_device, "%s: is_4byte_address=%d\n",
		__func__, qspi_dev->is_4byte_address);

	return xh2a_spi_nor_erase_chip(qspi_dev, &ioc);
}

static int xh2a_qspi_config(struct xh2a_qspi_dev *qspi_dev,
			    const struct xh2a_qspi_cfg __user *arg)
{
	struct xh2a_qspi_cfg ioc_cfg = { 0 };

	if (!arg) {
		dev_err(qspi_dev->miscdev.this_device, "%s: Invalid argument\n",
			__func__);
		return -EINVAL;
	}

	if (copy_from_user(&ioc_cfg, arg, sizeof(ioc_cfg))) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	dev_dbg(qspi_dev->miscdev.this_device,
		"%s: is_dma=%d, device_paddr=0x%llx\n", __func__,
		ioc_cfg.is_dma, ioc_cfg.device_paddr);

	qspi_dev->is_dma = (ioc_cfg.is_dma == 1);
	qspi_dev->device_paddr = ioc_cfg.device_paddr;

	return 0;
}

static int xh2a_qspi_crc_enable(struct xh2a_qspi_dev *qspi_dev,
				uint32_t channel,
				const struct xh2a_qspi_op __user *arg)
{
	struct xh2a_qspi_op ioc = { 0 };

	if (!arg) {
		dev_err(qspi_dev->miscdev.this_device, "%s: Invalid arg\n",
			__func__);
		return -EINVAL;
	}

	if (copy_from_user(&ioc, arg, sizeof(ioc))) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	if (xh2a_axi_crc_config(qspi_dev, ioc.size, channel) < 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: axi crc config failed\n", __func__);
		return -EFAULT;
	}

	xh2a_axi_crc_enable(qspi_dev);

	return 0;
}

static int xh2a_qspi_crc_disable(struct xh2a_qspi_dev *qspi_dev,
				 struct xh2a_qspi_op __user *arg)
{
	int ret;
	struct xh2a_qspi_op ioc = { 0 };
	uint32_t qspi_crc = 0;

	if (!arg) {
		dev_err(qspi_dev->miscdev.this_device, "%s: Invalid arg\n",
			__func__);
		return -EINVAL;
	}

	if (copy_from_user(&ioc, arg, sizeof(ioc))) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	ret = xh2a_axi_crc_get_result(qspi_dev, &qspi_crc);

	if (ret != 0) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: get axi crc value failed\n", __func__);
		return -EFAULT;
	}

	xh2a_axi_crc_disable(qspi_dev);
	memcpy(ioc.data, &qspi_crc, sizeof(uint32_t));

	if (copy_to_user(arg, &ioc, sizeof(ioc))) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: copy_to_user failed\n", __func__);
		return -EFAULT;
	}

	return 0;
}

static long xh2a_qspi_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	int retval = 0;
	struct miscdevice *miscdev;
	struct xh2a_qspi_dev *qspi_dev = NULL;

	miscdev = filp->private_data;
	qspi_dev = container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	if (!qspi_dev) {
		dev_err(miscdev->this_device, "%s: invalid device\n", __func__);
		return -EINVAL;
	}

	if (atomic_read(&qspi_dev->removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (atomic_read(&qspi_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: device try enter sleep, wait...\n", __func__);

		retval = wait_event_interruptible(
			qspi_dev->block_ioctl_wq,
			atomic_read(&qspi_dev->block_ioctl_flag) == 0);

		if (retval < 0) {
			dev_err(miscdev->this_device,
				"%s: wait_event_interruptible failed %d\n",
				__func__, retval);
			return -EINTR;
		}
	}

	pm_runtime_get_sync(miscdev->parent);

	mutex_lock(&qspi_dev->qspi_mutex);

	switch (cmd) {
	case IOCTL_XH2A_QSPI_NORFLASH_PROGRAM_CMD: {
		uint32_t tmp;

		tmp = _IOC_SIZE(cmd);

		if ((tmp % sizeof(struct xh2a_qspi_op)) != 0) {
			dev_err(miscdev->this_device,
				"%s: Invalid IOCTL size\n", __func__);
			retval = -EINVAL;
			break;
		}

		if (qspi_dev->is_dma) {
			if (xh2a_qspi_crc_enable(
				    qspi_dev, AXI_CRC_CHANNEL_SEL_WRITE,
				    (struct xh2a_qspi_op __user *)arg) != 0) {
				dev_err(miscdev->this_device,
					"%s: axi crc enable failed\n",
					__func__);
				retval = -EINVAL;
				break;
			}
		}

		retval = xh2a_qspi_program(qspi_dev,
					   (struct xh2a_qspi_op __user *)arg);

		if (retval != 0) {
			dev_err(miscdev->this_device,
				"%s: qspi program failed\n", __func__);
			break;
		}

		if (qspi_dev->is_dma) {
			if (xh2a_qspi_crc_disable(
				    qspi_dev,
				    (struct xh2a_qspi_op __user *)arg) != 0) {
				dev_err(miscdev->this_device,
					"%s: axi crc disable failed\n",
					__func__);
				retval = -EINVAL;
				break;
			}
		}

		break;
	}

	case IOCTL_XH2A_QSPI_NORFLASH_READ_CMD: {
		uint32_t tmp;

		tmp = _IOC_SIZE(cmd);

		if ((tmp % sizeof(struct xh2a_qspi_op)) != 0) {
			dev_err(miscdev->this_device,
				"%s: Invalid IOCTL size\n", __func__);
			retval = -EINVAL;
			break;
		}

		if (qspi_dev->is_dma) {
			if (xh2a_qspi_crc_enable(
				    qspi_dev, AXI_CRC_CHANNEL_SEL_READ,
				    (struct xh2a_qspi_op __user *)arg) != 0) {
				dev_err(miscdev->this_device,
					"%s: axi crc enable failed\n",
					__func__);
				retval = -EINVAL;
				break;
			}
		}

		retval = xh2a_qspi_read(qspi_dev,
					(struct xh2a_qspi_op __user *)arg);
		if (retval != 0) {
			dev_err(miscdev->this_device, "%s: qspi read failed\n",
				__func__);
			break;
		}

		if (qspi_dev->is_dma) {
			if (xh2a_qspi_crc_disable(
				    qspi_dev,
				    (struct xh2a_qspi_op __user *)arg) != 0) {
				dev_err(miscdev->this_device,
					"%s: axi crc disable failed\n",
					__func__);
				retval = -EINVAL;
				break;
			}
		}

		break;
	}

	case IOCTL_XH2A_QSPI_NORFLASH_ERASE_CMD: {
		uint32_t tmp;
		uint8_t id[SPI_NOR_MAX_ID_LEN] = { 0 };

		tmp = _IOC_SIZE(cmd);

		if ((tmp % sizeof(struct xh2a_qspi_op)) != 0) {
			dev_err(miscdev->this_device,
				"%s: Invalid IOCTL size\n", __func__);
			retval = -EINVAL;
			break;
		}

		retval = xh2a_spi_nor_read_id(qspi_dev, id, SPI_NOR_MAX_ID_LEN);
		if (retval < 0) {
			dev_err(miscdev->this_device,
				"%s: xh2a_spi_nor_read_id failed, retval=%d\n",
				__func__, retval);
			break;
		}

		if (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF) {
			dev_err(miscdev->this_device,
				"%s: Invalid SPI NOR flash ID : 0x%02x 0x%02x "
				"0x%02x\n",
				__func__, id[0], id[1], id[2]);
			retval = -ENODEV;
			break;
		}

		if (id[0] == DEVICE_NOR_MICRON_MAN_ID)
			qspi_dev->flash_id = XH2A_QSPI_NORFLASH_MICRON;
		else if (id[0] == DEVICE_NOR_GIGA_MAN_ID)
			qspi_dev->flash_id = XH2A_QSPI_NORFLASH_GIGADEVICE;
		else if (id[0] == DEVICE_NOR_WINBOND_MAN_ID)
			qspi_dev->flash_id = XH2A_QSPI_NORFLASH_WINBOND;
		else
			qspi_dev->flash_id = XH2A_QSPI_NORFLASH_UNKNOWN;

		retval = xh2a_qspi_erase(qspi_dev,
					 (struct xh2a_qspi_op __user *)arg);
		break;
	}

	case IOCTL_XH2A_QSPI_NORFLASH_READ_ID_CMD: {
		uint8_t id[SPI_NOR_MAX_ID_LEN];

		retval = xh2a_spi_nor_read_id(qspi_dev, id, SPI_NOR_MAX_ID_LEN);

		if (retval < 0) {
			dev_err(miscdev->this_device,
				"%s: xh2a_spi_nor_read_id failed, retval=%d\n",
				__func__, retval);
			break;
		}

		if (copy_to_user((uint8_t __user *)arg, id, sizeof(id))) {
			dev_err(miscdev->this_device,
				"%s: copy_to_user failed\n", __func__);
			retval = -EFAULT;
		}

		break;
	}

	case IOCTL_XH2A_QSPI_NORFLASH_CONFIG_CMD: {
		uint32_t tmp;

		tmp = _IOC_SIZE(cmd);

		if ((tmp % sizeof(struct xh2a_qspi_cfg)) != 0) {
			dev_err(miscdev->this_device,
				"%s: Invalid IOCTL size\n", __func__);
			retval = -EINVAL;
			break;
		}

		retval = xh2a_qspi_config(qspi_dev,
					  (struct xh2a_qspi_cfg __user *)arg);
		break;
	}

	default:
		retval = -EINVAL;
		break;
	}

	mutex_unlock(&qspi_dev->qspi_mutex);

	if ((retval != 0) && atomic_read(&qspi_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: ioctl failed in sleep process.\n", __func__);
		retval = -EINTR;
	}

	pm_runtime_mark_last_busy(miscdev->parent);
	pm_runtime_put_sync(miscdev->parent);

	return retval;
}

static const struct file_operations xh2a_qspi_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_qspi_open,
	.release = xh2a_qspi_release,
	.unlocked_ioctl = xh2a_qspi_ioctl,
};

static ssize_t xh2a_device_type_show(struct device *dev,
				     struct device_attribute *devattr,
				     char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_qspi_dev *qspi_dev =
		container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	return sprintf(buf, "%u\n", qspi_dev->flash_id);
}

static DEVICE_ATTR_RO(xh2a_device_type);

static ssize_t xh2a_device_paddr_show(struct device *dev,
				      struct device_attribute *devattr,
				      char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_qspi_dev *qspi_dev =
		container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	return sprintf(buf, "0x%llx\n", (unsigned long long)qspi_dev->device_paddr);
}

static DEVICE_ATTR_RO(xh2a_device_paddr);

static ssize_t xh2a_qspi_4b_addr_show(struct device *dev,
				      struct device_attribute *devattr,
				      char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_qspi_dev *qspi_dev =
		container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	return sprintf(buf, "%d\n", qspi_dev->is_4byte_address);
}

static DEVICE_ATTR_RO(xh2a_qspi_4b_addr);

static ssize_t xh2a_qspi_mode_show(struct device *dev,
				   struct device_attribute *devattr, char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_qspi_dev *qspi_dev =
		container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	return sprintf(buf, "%d\n", qspi_dev->is_dma);
}

static DEVICE_ATTR_RO(xh2a_qspi_mode);

static ssize_t xh2a_qspi_timeout_show(struct device *dev,
				      struct device_attribute *devattr,
				      char *buf)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_qspi_dev *qspi_dev =
		container_of(miscdev, struct xh2a_qspi_dev, miscdev);

	return sprintf(buf, "%u\n", qspi_dev->poll_timeout_ms);
}

static ssize_t xh2a_qspi_timeout_store(struct device *dev,
				       struct device_attribute *devattr,
				       const char *buf, size_t count)
{
	struct miscdevice *miscdev = dev_get_drvdata(dev);
	struct xh2a_qspi_dev *qspi_dev =
		container_of(miscdev, struct xh2a_qspi_dev, miscdev);
	unsigned int new_timeout;
	int ret;

	ret = kstrtouint(buf, 10, &new_timeout);
	if (ret < 0)
		return ret;

	qspi_dev->poll_timeout_ms = new_timeout;

	return count;
}

static DEVICE_ATTR_RW(xh2a_qspi_timeout);

static struct attribute *xh2a_qspi_attrs[] = {
	&dev_attr_xh2a_device_type.attr,  &dev_attr_xh2a_device_paddr.attr,
	&dev_attr_xh2a_qspi_4b_addr.attr, &dev_attr_xh2a_qspi_mode.attr,
	&dev_attr_xh2a_qspi_timeout.attr, NULL,
};
ATTRIBUTE_GROUPS(xh2a_qspi);

/*
 * xh2a_qspi_pm_notifier_prepare()
 *     - before pm prepare the qspi device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_qspi_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_qspi_dev *qspi_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_QSPI_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	qspi_dev = client->client_data;

	/* reject sleep while qspi is working */
	if (atomic_read(&qspi_dev->opened)) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: qspi is busy, reject sleep\n", __func__);
		return -EBUSY;
	}

	if (rollback) {
		if (atomic_dec_and_test(&qspi_dev->block_ioctl_flag))
			wake_up_interruptible(&qspi_dev->block_ioctl_wq);
	} else {
		atomic_inc(&qspi_dev->block_ioctl_flag);
	}

	return 0;
}

/*
 * xh2a_qspi_pm_notifier_complete()
 *     - after pm complete the qspi device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_qspi_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_qspi_dev *qspi_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_QSPI_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	qspi_dev = client->client_data;

	(void)qspi_dev;

	return 0;
}

/*
 * xh2a_qspi_pm_prepare()
 *     - pm prepare the qspi device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_qspi_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_qspi_dev *qspi_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_QSPI_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	qspi_dev = client->client_data;

	(void)qspi_dev;

	return 0;
}

/*
 * xh2a_qspi_pm_complete()
 *     - pm complete the qspi device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_qspi_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_qspi_dev *qspi_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_QSPI_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	qspi_dev = client->client_data;

	if (atomic_dec_and_test(&qspi_dev->block_ioctl_flag))
		wake_up_interruptible(&qspi_dev->block_ioctl_wq);

	return 0;
}

/*
 * xh2a_qspi_init() - init function for qspi device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_qspi_init(void *handle)
{
	int ret;
	struct xh2a_qspi_dev *qspi_dev = NULL;

	pr_debug("%s: xh2a qspi driver init\n", __func__);

	qspi_dev = kzalloc(sizeof(struct xh2a_qspi_dev), GFP_KERNEL);

	if (!qspi_dev) {
		pr_err("%s: alloc qspi_dev failed\n", __func__);
		return -ENOMEM;
	}

	qspi_dev->xh2a_xspi_base = XSPI_BASE_ADDR;
	qspi_dev->private_data = handle;

	atomic_set(&qspi_dev->opened, 0);
	atomic_set(&qspi_dev->removed, 0);
	kref_init(&qspi_dev->refcount);

	atomic_set(&qspi_dev->block_ioctl_flag, 0);
	init_waitqueue_head(&qspi_dev->block_ioctl_wq);

	mutex_init(&qspi_dev->qspi_mutex);

	memset(qspi_dev->name_buf, 0, XH2A_QSPI_NAME_LEN);
	snprintf(qspi_dev->name_buf, XH2A_QSPI_NAME_LEN,
		 XH2A_QSPI_DEVICE_NAME "%d", xh2a_pcie_device_index(handle));

	qspi_dev->ops.erase_result = xh2a_nor_get_erase_result;
	qspi_dev->ops.program_result = xh2a_nor_get_program_result;
	qspi_dev->flash_id = XH2A_NOR_DEVICE_ID;
	qspi_dev->poll_timeout_ms = POLL_TIMEOUT_MS;

	qspi_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	qspi_dev->miscdev.name = qspi_dev->name_buf;
	qspi_dev->miscdev.fops = &xh2a_qspi_fops;
	qspi_dev->miscdev.mode = 0666;
	qspi_dev->miscdev.groups = xh2a_qspi_groups;
	qspi_dev->miscdev.parent = xh2a_pcie_device_ptr(handle);
	ret = misc_register(&qspi_dev->miscdev);

	if (ret) {
		pr_err("%s: misc_register failed\n", __func__);
		goto err_misc_register;
	}

	snprintf(qspi_dev->client.name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_QSPI_DEVICE_NAME);

	qspi_dev->client.work_num = 0;

	qspi_dev->client.client_data = qspi_dev;
	qspi_dev->client.private_data = handle;
	qspi_dev->client.prepare_cb = xh2a_qspi_pm_prepare;
	qspi_dev->client.complete_cb = xh2a_qspi_pm_complete;
	qspi_dev->client.notifier_prepare_cb = xh2a_qspi_pm_notifier_prepare;
	qspi_dev->client.notifier_complete_cb = xh2a_qspi_pm_notifier_complete;

	ret = xh2a_pcie_register_client(handle, &qspi_dev->client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_client_register;
	}

	return 0;

err_client_register:
	misc_deregister(&qspi_dev->miscdev);

err_misc_register:
	kfree(qspi_dev);

	return ret;
}

/*
 * xh2a_qspi_deinit() - remove the qspi device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_qspi_deinit(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_qspi_dev *qspi_dev;

	pr_debug("%s: xh2a qspi driver deinit\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_QSPI_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: Failed to get client for name '%s'\n", __func__,
			 XH2A_QSPI_DEVICE_NAME);
		return 0;
	}

	qspi_dev = client->client_data;

	atomic_set(&qspi_dev->removed, 1);

	misc_deregister(&qspi_dev->miscdev);

	xh2a_pcie_unregister_client(handle, client);

	qspi_dev->private_data = NULL;
	kref_put(&qspi_dev->refcount, xh2a_qspi_safe_release);

	return 0;
}

/*
 * xh2a_qspi_notifier_call() - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_qspi_notifier_call(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_qspi_init(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_qspi_deinit(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_qspi_notifier_block = {
	.notifier_call = xh2a_qspi_notifier_call,
};

/*
 * xh2a_qspi_register_driver() - register dummy driver
 */
int __init xh2a_qspi_register_driver(void)
{
	pr_debug("%s:Register XH2A QSPI driver\n", __func__);

	return xh2a_host_register_notifier_chain(&xh2a_qspi_notifier_block);
}

/*
 * xh2a_qspi_unregister_driver() - unregister dummy driver
 */
void __exit xh2a_qspi_unregister_driver(void)
{
	pr_debug("%s:Unregister XH2A QSPI driver\n", __func__);

	xh2a_host_unregister_notifier_chain(&xh2a_qspi_notifier_block);
}

MODULE_LICENSE("GPL");
