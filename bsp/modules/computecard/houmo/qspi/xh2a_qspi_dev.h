// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */

#ifndef _XH2A_QSPI_DEV_H_
#define _XH2A_QSPI_DEV_H_

#include <xh2a_qspi_internal.h>

/*
 * Manufacturer IDs for SPI NOR flash devices
 * These IDs are used to identify the manufacturer of the NOR flash device.
 */
#define DEVICE_NOR_MICRON_MAN_ID  0x20 /* MICRON */
#define DEVICE_NOR_GIGA_MAN_ID	  0xC8 /* GIGADEVICE */
#define DEVICE_NOR_WINBOND_MAN_ID 0xEF /* WINBOND */

enum xh2a_qspi_norflash_manufacturer {
	XH2A_QSPI_NORFLASH_MICRON = 0, /* MICRON */
	XH2A_QSPI_NORFLASH_GIGADEVICE, /* GIGADEVICE */
	XH2A_QSPI_NORFLASH_WINBOND, /* WINBOND */
	XH2A_QSPI_NORFLASH_UNKNOWN,
};

#endif