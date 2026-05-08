// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */

#ifndef _XH2A_FLASH_H_
#define _XH2A_FLASH_H_

#if defined(DEVICE_NOR_MICRON)

#define XH2A_NOR_DEVICE_ID 0x0

int mt25_nor_get_program_result(struct xh2a_qspi_dev *qspi_dev);
int mt25_nor_get_erase_result(struct xh2a_qspi_dev *qspi_dev);

#define xh2a_nor_get_program_result mt25_nor_get_program_result
#define xh2a_nor_get_erase_result   mt25_nor_get_erase_result

#elif defined(DEVICE_NOR_GIGADEVICE)

#define XH2A_NOR_DEVICE_ID 0x1

int gd25_nor_get_program_result(struct xh2a_qspi_dev *qspi_dev);
int gd25_nor_get_erase_result(struct xh2a_qspi_dev *qspi_dev);

#define xh2a_nor_get_program_result gd25_nor_get_program_result
#define xh2a_nor_get_erase_result   gd25_nor_get_erase_result

#else

#define XH2A_NOR_DEVICE_ID 0x2

#define xh2a_nor_get_program_result NULL
#define xh2a_nor_get_erase_result   NULL

#endif

#endif