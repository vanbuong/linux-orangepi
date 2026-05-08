// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */

#ifndef _XH2A_AXI_CRC_H_
#define _XH2A_AXI_CRC_H_

#include <linux/bits.h>

#define AXI_CRC_CHANNEL_SEL_READ    (0x00000000U)
#define AXI_CRC_CHANNEL_SEL_WRITE   (0x80000000U)
#define AXI_CRC_CHANNEL_SEL_MASK    BIT(31)

#define AXI_CRC_STATE_RESET     (0x0)
#define AXI_CRC_STATE_RUN       (0x1)
#define AXI_CRC_STATE_FINISH    (0x2)
#define AXI_CRC_STATE_MASK      (0x3)

int xh2a_axi_crc_config(struct xh2a_qspi_dev *qspi_dev, uint32_t len,
    uint32_t channel_sel);
int xh2a_axi_crc_get_result(struct xh2a_qspi_dev *qspi_dev, uint32_t *crc);
void xh2a_axi_crc_enable(struct xh2a_qspi_dev *qspi_dev);
void xh2a_axi_crc_disable(struct xh2a_qspi_dev *qspi_dev);

#endif /* _XH2A_AXI_CRC_H_ */
