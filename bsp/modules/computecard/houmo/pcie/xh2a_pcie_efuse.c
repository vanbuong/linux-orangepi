// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <xh2a_pcie_api.h>
#include <xh2a_address.h>
#include <xh2a_efuse.h>

static bool xh2a_pcie_efuse_row_16bit_duplicated(uint32_t row_offset)
{
	return (row_offset > 200);
}

int xh2a_pcie_get_efuse_data(void *handle, uint32_t row_offset,
			     uint32_t bit_offset, uint32_t bit_length,
			     uint32_t *efuse_data)
{
	int ret;
	uint32_t row_data_primary;
	uint32_t row_data_duplicate;
	uint32_t row_data_final;

	if ((row_offset > 256) || (bit_offset > 31) || (bit_length > 32)) {
		pr_err("%s: Invalid efuse row offset or bit offset / length\n",
		       __func__);
		return -EINVAL;
	}

	ret = xh2a_pcie_pio_readl(handle,
				  XH2A_DEVICE_EFUSE_MEM_BASE + row_offset * 4,
				  &row_data_primary);

	if (ret || (row_data_primary == 0xFFFFFFFF)) {
		pr_err("%s: Failed to read efuse row primary data\n", __func__);
		*efuse_data = 0;
		return ret;
	}

	if (xh2a_pcie_efuse_row_16bit_duplicated(row_offset)) {
		pr_debug("%s: Efuse row %d is 16bit duplicated\n", __func__,
			 row_offset);
		row_data_duplicate = (row_data_primary >> 16);
	} else {
		ret = xh2a_pcie_pio_readl(
			handle, XH2A_DEVICE_EFUSE_MEM_BASE + row_offset * 4 + 4,
			&row_data_duplicate);

		if (ret || (row_data_duplicate == 0xFFFFFFFF)) {
			pr_err("%s: Failed to read efuse row duplicate data\n",
			       __func__);
			*efuse_data = 0;
			return ret;
		}
	}

	row_data_final = row_data_primary | row_data_duplicate;

	if ((bit_length == 32) && (bit_offset == 0))
		*efuse_data = row_data_final;
	else
		*efuse_data = (row_data_final >> bit_offset) &
			      ((1 << bit_length) - 1);

	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_get_efuse_data);

int xh2a_pcie_efuse_update_ddr_size(void *handle, uint64_t *size)
{
	int ret;
	uint32_t ddr_chip_quantity, ddr_chip_capacity;

	ret = xh2a_pcie_get_efuse_data(handle, XH2A_EFUSE_SUBTYPE_DDR_0_ROW,
				       XH2A_EFUSE_SUBTYPE_DDR_0_BIT,
				       XH2A_EFUSE_SUBTYPE_DDR_0_LENGTH,
				       &ddr_chip_quantity);

	if (ret != 0) {
		pr_err("%s: get efuse data failed\n", __func__);
		*size = 0;
		return ret;
	}

	if (ddr_chip_quantity > 6) {
		pr_err("%s: get invalid ddr chip quantity %d\n", __func__,
		       ddr_chip_quantity);
		*size = 0;
		return -1;
	}

	ret = xh2a_pcie_get_efuse_data(handle, XH2A_EFUSE_SUBTYPE_DDR_1_ROW,
				       XH2A_EFUSE_SUBTYPE_DDR_1_BIT,
				       XH2A_EFUSE_SUBTYPE_DDR_1_LENGTH,
				       &ddr_chip_capacity);

	if (ret != 0) {
		pr_err("%s: get efuse data failed\n", __func__);
		*size = 0;
		return ret;
	}

	if (ddr_chip_capacity > 16) {
		pr_err("%s: get invalid ddr chip capacity %d\n", __func__,
		       ddr_chip_capacity);
		*size = 0;
		return -1;
	}

	*size = ddr_chip_quantity * ddr_chip_capacity * 0x40000000ULL;
	return 0;
}
EXPORT_SYMBOL(xh2a_pcie_efuse_update_ddr_size);
