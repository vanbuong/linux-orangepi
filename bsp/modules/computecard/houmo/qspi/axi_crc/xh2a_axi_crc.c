// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 *
 * Note: This file is only valid for QSPI DMA transmission
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <xh2a_pcie_api.h>

#include "../xh2a_qspi.h"
#include "xh2a_axi_crc.h"

#define AOSS_SCTRL_BASE         (0x70000000U)
#define AXI_CRC_EN              (0x148)
#define AXI_CRC_CONFIG          (0x14c)
#define AXI_CRC_OUT_VALID       (0x150)
#define AXI_CRC_OUT             (0x154)

/**
 * xh2a_axi_crc_config - Configures the AXI CRC module.
 * @qspi_dev: Pointer to the QSPI device structure.
 * @len: Length of the data to be processed by the CRC module (in bytes).
 * @channel_sel: Channel selection mask for the CRC module.
 *
 * This function sets up the AXI CRC configuration register with the given
 * parameters. The length is divided by 4 (since it's assumed to be in bytes)
 * and written along with the channel selection mask into the configuration
 * register.
 *
 * Return: success returns 0.
 */
int xh2a_axi_crc_config(struct xh2a_qspi_dev *qspi_dev, uint32_t len,
	uint32_t channel_sel)
{
	uint32_t config = 0x0;

	if (len & 0x3) {
		dev_err(qspi_dev->miscdev.this_device,
			"len is not 4-byte aligned\n");
		return -1;
	}

	config |= channel_sel & AXI_CRC_CHANNEL_SEL_MASK;
	config |= (len >> 2);

	xh2a_pcie_pio_writel(qspi_dev->private_data,
		AOSS_SCTRL_BASE + AXI_CRC_CONFIG, config);

	return 0;
}

/**
 * xh2a_axi_crc_get_result - Retrieves the CRC result from the AXI CRC module.
 * @qspi_dev: Pointer to the QSPI device structure.
 * @crc: Pointer to store the resulting CRC value.
 *
 * This function waits for the AXI CRC module to signal that the CRC result
 * is ready. If the result becomes available within the timeout period, the
 * function retrieves the CRC value and returns 0. If the operation times out,
 * it returns -ETIMEDOUT.
 *
 * Return: 0 on success, -ETIMEDOUT on timeout.
 */
int xh2a_axi_crc_get_result(struct xh2a_qspi_dev *qspi_dev, uint32_t *crc)
{
	uint32_t crc_valid;
	unsigned long timeout = jiffies + msecs_to_jiffies(POLL_TIMEOUT_MS);

	while (1) {
		xh2a_pcie_pio_readl(qspi_dev->private_data,
			AOSS_SCTRL_BASE + AXI_CRC_OUT_VALID, &crc_valid);

		/* Break loop if the CRC result is valid */
		if (crc_valid == 0x1)
			break;

		/* Check for timeout */
		if (time_after(jiffies, timeout)) {
			dev_err(qspi_dev->miscdev.this_device,
				"Waiting timeout\n");
			return -ETIMEDOUT;
		}

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	}

	return xh2a_pcie_pio_readl(qspi_dev->private_data,
		AOSS_SCTRL_BASE + AXI_CRC_OUT, crc);
}

/**
 * xh2a_axi_crc_enable - Enables the AXI CRC module.
 * @qspi_dev: Pointer to the QSPI device structure.
 *
 * This function reads the AXI CRC enable register, sets the enable bit
 * (bit 0), and writes the updated value back to the register. This action
 * enables the CRC module for subsequent operations.
 */
void xh2a_axi_crc_enable(struct xh2a_qspi_dev *qspi_dev)
{
	uint32_t crc_ctrl = 0x0;

	xh2a_pcie_pio_readl(qspi_dev->private_data,
		AOSS_SCTRL_BASE + AXI_CRC_EN, &crc_ctrl);

	crc_ctrl |= BIT(0);

	xh2a_pcie_pio_writel(qspi_dev->private_data,
		AOSS_SCTRL_BASE + AXI_CRC_EN, crc_ctrl);
}

/**
 * xh2a_axi_crc_disable - Disables the AXI CRC module.
 * @qspi_dev: Pointer to the QSPI device structure.
 *
 * This function disables the AXI CRC module by reading the enable register,
 * clearing the enable bit (bit 0), and writing the updated value back.
 * It ensures the CRC functionality is turned off to prevent unnecessary
 * operations when not required.
 */
void xh2a_axi_crc_disable(struct xh2a_qspi_dev *qspi_dev)
{
	uint32_t crc_ctrl = 0x0;

	xh2a_pcie_pio_readl(qspi_dev->private_data,
		AOSS_SCTRL_BASE + AXI_CRC_EN, &crc_ctrl);

	crc_ctrl &= ~BIT(0);

	xh2a_pcie_pio_writel(qspi_dev->private_data,
		AOSS_SCTRL_BASE + AXI_CRC_EN, crc_ctrl);
}
