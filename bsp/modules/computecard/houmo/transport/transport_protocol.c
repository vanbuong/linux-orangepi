// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/types.h>
#include <linux/vmalloc.h>
#include <xh2a_pcie_api.h>
#include "transport_protocol.h"

/* definition for MSI bit */
#define XH2A_TRANSPORT_MSI_BIT 0
/* definition for ELBI bit */
#define XH2A_TRANSPORT_ELBI_BIT 0

/* TODO: move base address to public header file */
#define AOSS_SCTRL_BASE		   0x70000000ULL
#define XH2A_AOSS_MSI_VLD	   (AOSS_SCTRL_BASE + 0x2DC)
#define XH2A_AOSS_MSI_MASKN	   (AOSS_SCTRL_BASE + 0x2D8)
#define XH2A_AOSS_MSI_TRIGGER_VLD  (AOSS_SCTRL_BASE + 0x2EC)
#define XH2A_AOSS_MSI_TRIGGER_MASK (AOSS_SCTRL_BASE + 0x2E8)
#define XH2A_AOSS_MSI_TRIGGER_CLR  (AOSS_SCTRL_BASE + 0x2E4)
#define XH2A_AOSS_MSI_TRIGGER_SET  (AOSS_SCTRL_BASE + 0x2E0)
#define XH2A_AOSS_MSI_MASKN_E2_BIT 12

#define XH2A_TRANSPORT_REG0_ADDR (AOSS_SCTRL_BASE + 0x290)
#define XH2A_TRANSPORT_REG1_ADDR (AOSS_SCTRL_BASE + 0x298)
#define XH2A_TRANSPORT_REG2_ADDR (AOSS_SCTRL_BASE + 0x2A0)
#define XH2A_TRANSPORT_REG3_ADDR (AOSS_SCTRL_BASE + 0x2A8)
#define XH2A_TRANSPORT_REG4_ADDR (AOSS_SCTRL_BASE + 0x2B0)
#define XH2A_TRANSPORT_REG5_ADDR (AOSS_SCTRL_BASE + 0x2B8)

/*
 * struct xh2a_transport_reg_addr - register address for transport protocol
 * @reg0_addr: register 0 address.
 *  reg0 is 32bit reg addressed in 64bit, used to store protocol command.
 * @reg1_addr: register 1 address.
 *  reg1 is 32bit reg addressed in 64bit, used to store protocol memid.
 * @reg2_addr: register 2 address.
 *  reg2 is 32bit reg addressed in 64bit, store low 32bit of mem address.
 * @reg3_addr: register 3 address
 *  reg3 is 32bit reg addressed in 64bit, store high 32bit of mem address.
 * @reg4_addr: register 4 address
 *  reg4 is 32bit reg addressed in 64bit, store low 32bit of mem size.
 * @reg5_addr: register 5 address
 *  reg5 is 32bit reg addressed in 64bit, store high 32bit of mem size.
 */
struct xh2a_transport_reg_addr {
	union {
		uint64_t reg0_addr;
		uint64_t command_addr;
	};

	union {
		uint64_t reg1_addr;
		uint64_t memid_addr;
	};

	union {
		uint64_t reg2_addr;
		uint64_t memaddr_lo_addr;
	};

	union {
		uint64_t reg3_addr;
		uint64_t memaddr_hi_addr;
	};

	union {
		uint64_t reg4_addr;
		uint64_t memsize_lo_addr;
	};

	union {
		uint64_t reg5_addr;
		uint64_t memsize_hi_addr;
		uint64_t response_addr;
	};
};

enum xh2a_host_transport_cmd_id {
	XH2A_TRANSPORT_CMD_SAVE_MEM,
	XH2A_TRANSPORT_CMD_RESTORE_MEM,
	XH2A_TRANSPORT_CMD_COUNT,
};

#define XH2A_TRANSPORT_CMD_MAGIC (0x434D4400)

enum xh2a_host_transport_resp_id {
	XH2A_TRANSPORT_RESP_SUCCESS,
	XH2A_TRANSPORT_RESP_FAIL,
	XH2A_TRANSPORT_RESP_COUNT,
};

#define XH2A_TRANSPORT_RESPONSE_MAGIC (0x52455300)

struct xh2a_transport_reg_addr trans_reg_addr = {
	.reg0_addr = XH2A_TRANSPORT_REG0_ADDR,
	.reg1_addr = XH2A_TRANSPORT_REG1_ADDR,
	.reg2_addr = XH2A_TRANSPORT_REG2_ADDR,
	.reg3_addr = XH2A_TRANSPORT_REG3_ADDR,
	.reg4_addr = XH2A_TRANSPORT_REG4_ADDR,
	.reg5_addr = XH2A_TRANSPORT_REG5_ADDR,
};

static int
xh2a_transport_protocol_recv_cmd(struct xh2a_transport_protocol_handle *handle)
{
	int ret;
	uint32_t cmd, memid, addr_lo, addr_hi, size_lo, size_hi;
	uint64_t addr, size;

	ret = xh2a_pcie_pio_readl(handle->private_data,
				  trans_reg_addr.command_addr, &cmd);

	if (ret != 0) {
		pr_err("%s: read cmd fail\n", __func__);
		return ret;
	}

	switch (cmd) {
	case XH2A_TRANSPORT_CMD_MAGIC + XH2A_TRANSPORT_CMD_SAVE_MEM:
		xh2a_pcie_pio_readl(handle->private_data,
				    trans_reg_addr.memid_addr, &memid);
		xh2a_pcie_pio_readl(handle->private_data,
				    trans_reg_addr.memaddr_lo_addr, &addr_lo);
		xh2a_pcie_pio_readl(handle->private_data,
				    trans_reg_addr.memaddr_hi_addr, &addr_hi);
		xh2a_pcie_pio_readl(handle->private_data,
				    trans_reg_addr.memsize_lo_addr, &size_lo);
		xh2a_pcie_pio_readl(handle->private_data,
				    trans_reg_addr.memsize_hi_addr, &size_hi);

		if (memid > XH2A_TRANSPORT_MEMORY_ID_COUNT) {
			pr_err("%s: memid %d is invalid\n", __func__, memid);
			return -EINVAL;
		}

		addr = ((uint64_t)addr_hi << 32) | addr_lo;
		size = size_lo;

		if (handle->mem[memid].status ==
		    XH2A_TRANSPORT_MEMORY_STATUS_UNINITIALIZED) {
			handle->mem[memid].hostmem = vmalloc(size);

			if (handle->mem[memid].hostmem == NULL) {
				pr_err("%s: vmalloc fail\n", __func__);
				return -ENOMEM;
			}
		}

		ret = xh2a_pcie_dma_read_mem(handle->private_data, addr,
					     handle->mem[memid].hostmem, size);

		if (ret != 0) {
			pr_err("%s: dma read fail\n", __func__);
			return ret;
		}

		handle->mem[memid].size = size;
		handle->mem[memid].paddr = addr;
		handle->mem[memid].status = XH2A_TRANSPORT_MEMORY_STATUS_SAVED;
		break;

	case XH2A_TRANSPORT_CMD_MAGIC + XH2A_TRANSPORT_CMD_RESTORE_MEM:
		xh2a_pcie_pio_readl(handle->private_data,
				    trans_reg_addr.memid_addr, &memid);

		if (memid > XH2A_TRANSPORT_MEMORY_ID_COUNT) {
			pr_err("%s: memid %d is invalid\n", __func__, memid);
			return -EINVAL;
		}

		if (handle->mem[memid].status !=
		    XH2A_TRANSPORT_MEMORY_STATUS_SAVED) {
			pr_err("%s: memid %d is not saved\n", __func__, memid);
			return -EINVAL;
		}

		if (handle->mem[memid].hostmem == NULL) {
			pr_err("%s: hostmem is NULL\n", __func__);
			return -EINVAL;
		}

		ret = xh2a_pcie_dma_write_mem(handle->private_data,
					      handle->mem[memid].paddr,
					      handle->mem[memid].hostmem,
					      handle->mem[memid].size);

		if (ret != 0) {
			pr_err("%s: dma write fail\n", __func__);
			return ret;
		}

		handle->mem[memid].status =
			XH2A_TRANSPORT_MEMORY_STATUS_RESTORED;
		break;

	default:
		pr_err("%s: unknown cmd: 0x%x\n", __func__, cmd);
		break;
	}

	return 0;
}

static int
xh2a_transport_protocol_send_resp(struct xh2a_transport_protocol_handle *handle,
				  int status)
{
	int ret;

	ret = xh2a_pcie_pio_writel(handle->private_data,
				   trans_reg_addr.response_addr,
				   status + XH2A_TRANSPORT_RESPONSE_MAGIC);

	if (ret != 0) {
		pr_err("%s: write resp fail\n", __func__);
		return ret;
	}

	ret = xh2a_pcie_write_bar_msgbit(handle->private_data,
					 XH2A_TRANSPORT_ELBI_BIT,
					 XH2A_PCIE_MSG_TO_E2);

	if (ret != 0) {
		pr_err("%s: write msgbit fail\n", __func__);
		return ret;
	}

	return 0;
}

static void transport_work_handler(struct work_struct *work)
{
	int ret;
	uint32_t val;

	struct xh2a_transport_protocol_handle *handle = container_of(
		work, struct xh2a_transport_protocol_handle, transport_work);

	pr_debug("%s: %p\n", __func__, handle);

	ret = xh2a_pcie_pio_readl(handle->private_data, XH2A_AOSS_MSI_VLD,
				  &val);

	if (ret != 0 || val == 0xffffffff) {
		pr_err("%s: read msival fail\n", __func__);
		return;
	}

	if (!(val & BIT(XH2A_AOSS_MSI_MASKN_E2_BIT))) {
		pr_debug("%s: msi is not for e2\n", __func__);
		return;
	}

	ret = xh2a_pcie_pio_readl(handle->private_data,
				  XH2A_AOSS_MSI_TRIGGER_VLD, &val);

	if (ret != 0 || val == 0xffffffff) {
		pr_err("%s: read trigger val fail\n", __func__);
		return;
	}

	if (!(val & BIT(XH2A_TRANSPORT_MSI_BIT))) {
		pr_debug("%s: msi is not for transport\n", __func__);
		return;
	}

	ret = xh2a_transport_protocol_recv_cmd(handle);

	xh2a_pcie_pio_writel(handle->private_data, XH2A_AOSS_MSI_TRIGGER_CLR,
			     BIT(XH2A_TRANSPORT_MSI_BIT));

	if (ret != 0) {
		pr_err("%s: protocl process failed %d\n", __func__, ret);
		xh2a_transport_protocol_send_resp(handle,
						  XH2A_TRANSPORT_RESP_FAIL);
	} else {
		pr_info("%s: protocl process success %d\n", __func__, ret);

		xh2a_transport_protocol_send_resp(handle,
						  XH2A_TRANSPORT_RESP_SUCCESS);
	}
}

int xh2a_transport_protocol_init(struct xh2a_transport_protocol_handle *handle,
				 void *private_data)
{
	int i;

	INIT_WORK(&handle->transport_work, transport_work_handler);

	for (i = 0; i < XH2A_TRANSPORT_MEMORY_ID_COUNT; i++) {
		handle->mem[i].status =
			XH2A_TRANSPORT_MEMORY_STATUS_UNINITIALIZED;
		handle->mem[i].size = 0;
		handle->mem[i].paddr = 0;
	}

	handle->private_data = private_data;
	return 0;
}

int xh2a_transport_protocol_deinit(struct xh2a_transport_protocol_handle *handle)
{
	int i;

	cancel_work_sync(&handle->transport_work);

	for (i = 0; i < XH2A_TRANSPORT_MEMORY_ID_COUNT; i++) {
		if (handle->mem[i].status !=
		    XH2A_TRANSPORT_MEMORY_STATUS_UNINITIALIZED)
			vfree(handle->mem[i].hostmem);
	}

	return 0;
}
