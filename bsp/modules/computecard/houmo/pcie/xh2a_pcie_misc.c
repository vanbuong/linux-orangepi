// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <xh2a_pcie_api.h>
#include "xh2a_pcie.h"

#define XH2A_PCIE_MSI_MAPPING_ADDR 0x60500008ULL

#define XH2A_PCIE_MSI_QUIRK
#ifdef XH2A_PCIE_MSI_QUIRK
static int xh2a_pcie_msi_quirk(struct xh2a_pcie_dev *p_xh2a)
{
	int cap_offset;
	uint16_t msi_control;

	cap_offset = pci_find_capability(p_xh2a->pdev, PCI_CAP_ID_MSI);

	if (cap_offset == 0)
		return -1;

	pci_read_config_word(p_xh2a->pdev, cap_offset + PCI_MSI_FLAGS,
			     &msi_control);

	if (msi_control & PCI_MSI_FLAGS_64BIT)
		pci_write_config_dword(
			p_xh2a->pdev, cap_offset + PCI_MSI_MASK_64, 0x00000000);
	else
		pci_write_config_dword(
			p_xh2a->pdev, cap_offset + PCI_MSI_MASK_32, 0x00000000);

	return 0;
}
#endif

void xh2a_pcie_probe_post(struct xh2a_pcie_dev *p_xh2a)
{
#ifdef XH2A_PCIE_MSI_QUIRK
	xh2a_pcie_msi_quirk(p_xh2a);
#endif

	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 0,
			     XH2A_PCIE_MSI_MAPPING_3_0);
	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 4,
			     XH2A_PCIE_MSI_MAPPING_7_4);
	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 8,
			     XH2A_PCIE_MSI_MAPPING_11_8);
	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 12,
			     XH2A_PCIE_MSI_MAPPING_15_12);
	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 16,
			     XH2A_PCIE_MSI_MAPPING_19_16);
	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 20,
			     XH2A_PCIE_MSI_MAPPING_23_20);
	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 24,
			     XH2A_PCIE_MSI_MAPPING_27_24);
	xh2a_pcie_pio_writel(p_xh2a, XH2A_PCIE_MSI_MAPPING_ADDR + 28,
			     XH2A_PCIE_MSI_MAPPING_31_28);
}
