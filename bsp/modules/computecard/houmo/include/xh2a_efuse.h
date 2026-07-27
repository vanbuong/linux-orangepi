// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_EFUSE_H_
#define _XH2A_EFUSE_H_

#include <xh2a_efuse_macro.h>

/*
 * xh2a_pcie_get_fuse_data() - get data from efuse
 * @handle: the pcie handle
 * @row_offset: the row offset
 * @bit_offset: the bit offset
 * @bit_length: the bit length
 * @efuse_data: the efuse data
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_get_efuse_data(void *handle, uint32_t row_offset,
			     uint32_t bit_offset, uint32_t bit_length,
			     uint32_t *efuse_data);

/*
 * xh2a_pcie_efuse_update_ddr_size() - update ddr size
 * @handle: the pcie handle
 * @size: the ddr size
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_efuse_update_ddr_size(void *handle, uint64_t *size);

#endif