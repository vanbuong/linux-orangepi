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

#include <xh2a_pcie_api.h>

#include "../xh2a_qspi.h"
#include "../xh2a_qspi_dev.h"
#include "xh2a_flash.h"

#define MT25_COMMAND_READ_FLAG_STATUS_REG   0x70

#define MT25_P_FAIL             (0x1 << 4)
#define MT25_IS_PROGRAM_FAIL(sec) (((sec)&MT25_P_FAIL) != 0)

#define MT25_E_FAIL             (0x1 << 5)
#define MT25_IS_ERASE_FAIL(sec) (((sec)&MT25_E_FAIL) != 0)

/*
 * mt25_nor_get_program_result() - Check the programming result of
 * the SPI NOR flash.
 * @qspi_dev: QSPI device handle
 * Return: 0 if programming succeeded, -1 if programming failed,
 *      or a negative error code.
 *
 * This function checks the result of the previous SPI NOR flash programming
 * operation by reading the status register and interpreting the result based
 * on the device type.
 */
int mt25_nor_get_program_result(struct xh2a_qspi_dev *qspi_dev)
{
	uint32_t status = 0x0;

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, MT25_COMMAND_READ_FLAG_STATUS_REG);

	usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);

	xh2a_qspi_readl(qspi_dev, DW_SPI_DR, &status);

	if (MT25_IS_PROGRAM_FAIL(status)) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Program falsh error\n", __func__);
		return -1;
	}

	return 0;
}

/*
 * mt25_nor_get_erase_result() - Check the result of the SPI NOR flash
 * erase operation.
 * @qspi_dev: QSPI device handle
 * Return: 0 if erase succeeded, -1 if erase failed, or a negative error code.
 *
 * This function checks the result of a previous SPI NOR flash erase operation
 * by reading the status register and interpreting the result based on the
 * device type.
 */
int mt25_nor_get_erase_result(struct xh2a_qspi_dev *qspi_dev)
{
	uint32_t status = 0x0;

	xh2a_qspi_writel(qspi_dev, DW_SPI_DR, MT25_COMMAND_READ_FLAG_STATUS_REG);

	usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);

	xh2a_qspi_readl(qspi_dev, DW_SPI_DR, &status);

	if (MT25_IS_ERASE_FAIL(status)) {
		dev_err(qspi_dev->miscdev.this_device,
			"%s: Erase falsh error\n", __func__);
		return -1;
	}

	return 0;
}
