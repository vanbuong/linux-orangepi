// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_qspi_internal.h

Abstract:

	This module contains the common declarations shared by driver
	and HAL for QSPI module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/
#ifndef _XH2A_QSPI_INTERNAL_H_
#define _XH2A_QSPI_INTERNAL_H_

/*
 * XH2A_QSPI_DEVICE_NAME - unique name of xh2a qspi device
 */
#define XH2A_QSPI_DEVICE_NAME       "xh2a_qspi_device"

#define SPI_NOR_MAX_ID_LEN 6

#define SZ_512  0x00000200
#define SZ_4K   0x00001000
#define SZ_32K  0x00008000
#define SZ_64K  0x00010000
#define SZ_256K 0x00040000
#define SZ_16M  0x01000000
#define SZ_32M  0x02000000
#define SZ_64M  0x04000000

 /*
  * struct xh2a_qspi_cfg - xh2a qspi transport configuration
  * @is_dma: CPU or DMA mode
  * @device_type: Refer to enum spi_nor_manufacturer definition
  * @device_paddr: DDR physical address as seen by u7
  */
struct xh2a_qspi_cfg {
	uint32_t is_dma;
	uint64_t device_paddr;
};

/*
 * struct xh2a_qspi_op - Parameters for read/write/erase ioctl
 * @offset: Flash offset address
 * @size: The size that needs to be read, written, or erased
 * @data: Buffer for read and write operations, this field is not used
 * for erasing
 */
struct xh2a_qspi_op {
	uint32_t offset;
	uint32_t size;
	uint8_t data[SZ_512];
};

#define XH2A_QSPI_NORFLASH_IOCTL_MAGIC 'E'

#define IOCTL_XH2A_QSPI_NORFLASH_PROGRAM_CMD \
	_IOWR(XH2A_QSPI_NORFLASH_IOCTL_MAGIC, 0, struct xh2a_qspi_op)
#define IOCTL_XH2A_QSPI_NORFLASH_READ_CMD \
	_IOWR(XH2A_QSPI_NORFLASH_IOCTL_MAGIC, 1, struct xh2a_qspi_op)
#define IOCTL_XH2A_QSPI_NORFLASH_ERASE_CMD \
	_IOW(XH2A_QSPI_NORFLASH_IOCTL_MAGIC, 2, struct xh2a_qspi_op)
#define IOCTL_XH2A_QSPI_NORFLASH_READ_ID_CMD \
	_IOWR(XH2A_QSPI_NORFLASH_IOCTL_MAGIC, 3, uint8_t[SPI_NOR_MAX_ID_LEN])
#define IOCTL_XH2A_QSPI_NORFLASH_CONFIG_CMD \
	_IOW(XH2A_QSPI_NORFLASH_IOCTL_MAGIC, 4, struct xh2a_qspi_cfg)


#endif // !_XH2A_QSPI_INTERNAL_H_
