// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include <xh2a_pcie_api.h>
#include <xh2a_efuse.h>

#include "xh2a_ipu_device.h"
#include "xh2a_ipu_group.h"
#include "xh2a_ipu_kernel.h"
#include "xh2a_ipu_hw.h"
#include "xh2a_ipu_interrupt.h"
#include "xh2a_address.h"

/*!
 * \brief Read-only register.
 */
#define RO_REG const volatile

/*!
 * \brief Write-only register.
 */
#define WO_REG volatile

/*!
 * \brief Read/write register.
 */
#define RW_REG volatile

#define BOOTER_TILE_MAX	    (4)
#define IPUSS_CFG_BASE	    0x41000000
#define IPUSS_CRG_BASE	    0x41300000
#define IPUSS_CORE_OFFSET   0x100000
#define IPUSS_GNODE_OFFSET  0x40000
#define IPUSS_BOOTER_OFFSET 0x4000

#define AOSS_SYSCTRL_IPU_LOAD_BASE	0x700003C0
#define AOSS_SYSCTRL_IPUSS_RESET_STATUS 0x700003E0
#define IPUSS_RESET_REQUEST_BIT		8

#define AOSS_NIU_BASE	      0x70030000
#define IPU_NIU_CTRL_OFFSET   0x80
#define IPU_NIU_STATUS_OFFSET 0x84
#define IPU_NIU_MASK	      0xf00
#define IPU_NIU_DISCONNECT    0x500
#define IPU_NIU_CONNECT	      0xa00

#define IPUSS_CACHE_CFG_OFFSET	   0xD000
#define IPUSS_SW_INTERLEAVE_OFFSET 0x7000

#define IPUSS_TNODE_RV_PC(core_id, tile_id, index)                   \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x5000 + (index) * 0x400)

#define IPUSS_TNODE_RV_INFO(core_id, tile_id, index)                 \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x5038 + (index) * 0x400)

#define IPUSS_TNODE_RV_ERR_INFO(core_id, tile_id, index)             \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x5144 + (index) * 0x400)

#define IPUSS_TNODE_VP_ERR_INFO0(core_id, tile_id, index)            \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x3000 + (index) * 0x800)

#define IPUSS_TNODE_VP_ERR_INFO1(core_id, tile_id, index)            \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x300C + (index) * 0x800)

#define IPUSS_TNODE_NL_BASE(core_id, tile_id)                        \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x2000)

#define IPUSS_TNODE_TE_ERR(core_id, tile_id)                         \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x40)

#define IPUSS_CORE_TOP_ERR(core_id) \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + 0x50000)

#define IPUSS_GNODE_MBUS_ERR(core_id) \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + 0x43000)

#define IPUSS_GNODE_TOP_ERR(core_id) \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + 0x49000)

#define IPUSS_L2ICACHE_ERR(core_id, tile_id)                         \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0xD004)

/* idx=0 means load1 module; idx=1 means load0 module */
#define IPUSS_LOAD_ERR(core_id, idx)                                           \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + 0x40000 + \
	 (idx) * 0xC000)

#define IPUSS_RESIZER_ERR(core_id) \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + 0x42000)

#define IPUSS_STORE_ERR(core_id) \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + 0x41000)

#define IPUSS_TILE_TOP_ERR(core_id, tile_id)                         \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0xC000)

#define IPUSS_TNODE_BOOTER_ERR(core_id, tile_id)                     \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0xB018)

#define IPUSS_TNODE_MBUS_ERR(core_id, tile_id)                       \
	(IPUSS_CFG_BASE + IPUSS_CORE_OFFSET + (core_id) * 0x100000 + \
	 (tile_id) * 0x10000 + 0x9000)

#define IPU_KERNEL_ADDR_LOW(addr)  ((uint32_t)(addr))
#define IPU_KERNEL_ADDR_HIGH(addr) ((uint32_t)((addr) >> 32))
#define IPU_KERNEL_ADDR(hi, lo)	   (((uint64_t)(hi) << 32) | (lo))
#define IPU_KD_KERNEL_ADDR(kd) \
	IPU_KERNEL_ADDR((kd)->kernel_addr_hi, (kd)->kernel_addr_lo)

#define IPU_PARAMETER_ADDR_LOW(addr)  ((uint8_t)(addr))
#define IPU_PARAMETER_ADDR_HIGH(addr) ((uint32_t)((addr) >> 8))
#define IPU_PARAMETER_ADDR(hi, lo)    (((uint32_t)(hi) << 8) | (lo))
#define IPU_KD_PARAMETER_ADDR(kd) \
	IPU_PARAMETER_ADDR((kd)->parameter_addr_hi, (kd)->parameter_addr_lo)

union que_cfg_s {
	struct {
		uint32_t que_base_addr_hi : 8;
		uint32_t que_set : 1;
		uint32_t : 7;
		uint32_t que_depth : 11;
		uint32_t : 5;
	} u;
	uint32_t val;
};

union que_incr_s {
	struct {
		uint32_t que_incr_num : 8;
		uint32_t reserved : 24;
	} u;
	uint32_t val;
};

union que_ptr_s {
	struct {
		uint32_t que_write_ptr : 10;
		uint32_t : 6;
		uint32_t que_read_ptr : 10;
		uint32_t : 3;
		uint32_t que_full : 1;
		uint32_t que_empty : 1;
		uint32_t ker_done_intr : 1;
	} u;
	uint32_t val;
};

union debug_cfg_s {
	struct {
		uint32_t ker_cnt_enable : 1;
		uint32_t global_cnt_enable : 1;
		uint32_t all_cnt_clr : 1;
		uint32_t : 5;
		uint32_t global_cnt_working : 1;
		uint32_t mega_pending_exist : 1;
		uint32_t : 6;
		uint32_t mega_buf_use_cnt : 1;
		uint32_t : 15;
	} u;
	uint32_t val;
};

union debug_status_s {
	struct {
		uint32_t kd_buffered : 8;
		uint32_t ker_working : 8;
		uint32_t ilm_free : 16;
	} u;
	uint32_t val;
};

union debug_cnt_s {
	struct {
		uint32_t ker_launched_cnt : 16;
		uint32_t ker_done_cnt : 16;
	} u;
	uint32_t val;
};

struct booter_tile_reg_s {
	RW_REG uint32_t que_base_addr_lo;
	RW_REG union que_cfg_s que_cfg;
	RW_REG union que_incr_s que_incr;
	RO_REG union que_ptr_s que_ptr;
	RW_REG union debug_cfg_s debug_cfg;
	RO_REG union debug_status_s debug_status;
	RO_REG union debug_cnt_s debug_cnt;
	RO_REG uint32_t global_cnt;
};

struct booter_device_reg_s {
	struct booter_tile_reg_s booter_tile_reg[BOOTER_TILE_MAX];
	RO_REG uint32_t error_info;
	RW_REG uint32_t error_mask;
	RW_REG uint32_t error_clr;
};

struct __attribute__((packed)) kernel_desc_s {
	uint32_t kernel_addr_lo;
	uint32_t kernel_addr_hi : 8;
	uint32_t kernel_len : 16;
	uint32_t parameter_addr_lo : 8;
	uint32_t parameter_addr_hi;

	uint8_t interrupt_flag;
	uint8_t mode;
	uint8_t core_id;
	uint8_t reserve;
};

static int xh2a_ipu_writel(struct xh2a_ipu_device *ipu_dev, uint32_t addr,
			   uint32_t val)
{
	int ret;

	ret = xh2a_pcie_pio_writel(ipu_dev->private_data, addr, val);

	if (ret)
		dev_err(ipu_dev->miscdev.this_device, "%d: Ipu writel failed\n",
			ret);

	return ret;
}

static int xh2a_ipu_readl(struct xh2a_ipu_device *ipu_dev, uint32_t addr,
			  uint32_t *val)
{
	int ret;

	ret = xh2a_pcie_pio_readl(ipu_dev->private_data, addr, val);

	if (ret)
		dev_err(ipu_dev->miscdev.this_device, "%d: Ipu readl failed\n",
			ret);

	return ret;
}

void xh2a_booter_queue_trigger(struct xh2a_ipu_device *ipu_device,
			       struct xh2a_ipu_booter_queue *queue,
			       struct xh2a_ipu_group *group)
{
	uint32_t reg_offset;
	struct booter_tile_reg_s *tile_reg = NULL;
	union que_incr_s que_incr_value = { 0 };

	tile_reg = (struct booter_tile_reg_s *)queue->reg_base;
	reg_offset = (uintptr_t)&tile_reg->que_incr;
	que_incr_value.u.que_incr_num = group->kernel_num;

	/* already locked queue->tile_mutex, write incr_num to allocate KDs*/
	xh2a_ipu_writel(ipu_device, reg_offset, que_incr_value.val);
}

static void kernel_desc_core_id_generate(struct xh2a_ipu_group *group,
					 uint8_t *core_id)
{
	int i;
	uint32_t core_mask = 0;

	for (i = 0; i < group->core_num; i++) {
		core_mask |= (GENMASK(group->tile_num - 1, 0)
			      << (group->target[i].queue_id)
			      << (group->target[i].core_id << 2));
	}

	*core_id = core_mask;
}

static int xh2a_write_kd_entry(struct xh2a_ipu_device *ipu_dev,
			       void *queue_addr, struct kernel_desc_s *desc)
{
	int ret;
	struct kernel_desc_s kd_tmp;
	struct device *dev = ipu_dev->miscdev.this_device;

	dev_dbg(dev, "Kernel Descriptor:\n");
	dev_dbg(dev, "kernel_addr_lo    : 0x%08X\n", desc->kernel_addr_lo);
	dev_dbg(dev, "kernel_addr_hi    : 0x%02X\n", desc->kernel_addr_hi);
	dev_dbg(dev, "kernel_len        : 0x%04X (%d)\n", desc->kernel_len,
		desc->kernel_len);
	dev_dbg(dev, "parameter_addr_lo : 0x%02X\n", desc->parameter_addr_lo);
	dev_dbg(dev, "parameter_addr_hi : 0x%08X\n", desc->parameter_addr_hi);
	dev_dbg(dev, "interrupt_flag    : 0x%02X\n", desc->interrupt_flag);
	dev_dbg(dev, "mode              : 0x%02X\n", desc->mode);
	dev_dbg(dev, "core_id           : 0x%02X\n", desc->core_id);
	dev_dbg(dev, "reserve           : 0x%02X\n", desc->reserve);
	dev_dbg(dev, "queue_addr        : 0x%llX\n", (uint64_t)queue_addr);

	ret = xh2a_pcie_pio_write_mem(ipu_dev->private_data,
				      (uint64_t)queue_addr, (void *)desc,
				      sizeof(struct kernel_desc_s));
	if (ret) {
		dev_err(dev, "%s: write desc failed, ret=%d\n", __func__, ret);
		return ret;
	}

	ret = xh2a_pcie_pio_read_mem(ipu_dev->private_data,
				     (uint64_t)queue_addr, &kd_tmp,
				     sizeof(struct kernel_desc_s));
	if (ret) {
		dev_err(dev, "%s: read-back desc failed, ret=%d\n", __func__,
			ret);
		return ret;
	}

	ret = memcmp(desc, &kd_tmp, sizeof(struct kernel_desc_s));
	if (ret != 0) {
		dev_err(dev, "%s: desc mismatch after write-back check\n",
			__func__);
		dev_err(dev, "expected:");
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_OFFSET, 16, 1, desc,
			       sizeof(struct kernel_desc_s), false);

		dev_err(dev, "readback:");
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_OFFSET, 16, 1, &kd_tmp,
			       sizeof(struct kernel_desc_s), false);

		return -EIO;
	}

	return 0;
}

void xh2a_booter_queue_enqueue_group(struct xh2a_ipu_device *ipu_dev,
				     struct xh2a_ipu_booter_queue *queue,
				     struct xh2a_ipu_group *group)
{
	int ret;
	uint32_t spm_base, kernel_cnt = 0;
	uint8_t core_id, core_mask = 0;
	struct xh2a_ipu_kernel *kernel, *kernel_n;

	struct kernel_desc_s kernel_desc = { 0 };

	struct kernel_desc_s *queue_addr =
		(struct kernel_desc_s *)queue->queue_addr;

	kernel_desc_core_id_generate(group, &core_mask);

	core_id = queue->core_id;

	dev_dbg(ipu_dev->miscdev.this_device,
		"group %u %p write queue %d, wptr %d, kernels %u\n", group->id,
		group, core_id, queue->wptr, group->kernel_num);

	list_for_each_entry_safe(kernel, kernel_n, &group->kernel_list, node) {
		kernel_cnt++;
		/* write Kernel descriptor on queue buffer */
		kernel_desc.kernel_addr_lo =
			IPU_KERNEL_ADDR_LOW(kernel->kld.kernel_addr);

		kernel_desc.kernel_addr_hi =
			IPU_KERNEL_ADDR_HIGH(kernel->kld.kernel_addr);

		kernel_desc.kernel_len = (uint16_t)kernel->kld.kernel_size;

		if (group->param_type == XH2A_GROUP_PARAM_SPM) {
			spm_base = (core_id == 0) ? XH2A_DEVICE_SPM0_START :
						    XH2A_DEVICE_SPM1_START;
			kernel->kld.param_phy_addr =
				(kernel->param_spm_addr[core_id] - spm_base) +
				BOOTER_VIEW_SPM_BASE_ADDR;
		}

		kernel_desc.parameter_addr_lo =
			IPU_PARAMETER_ADDR_LOW(kernel->kld.param_phy_addr);

		kernel_desc.parameter_addr_hi =
			IPU_PARAMETER_ADDR_HIGH(kernel->kld.param_phy_addr);

		kernel_desc.mode = kernel->kld.ilm_mode;

		/* kernel size can not be 0 in cache mode */
		if (kernel_desc.mode == 1 && kernel_desc.kernel_len == 0)
			kernel_desc.kernel_len = 0xFFFF;

		kernel_desc.core_id = core_mask;

		if ((queue->flag & XH2A_IPU_TILE_QUEUE_FLAG_INTERRUPT) &&
		    (kernel_cnt == group->kernel_num))
			kernel_desc.interrupt_flag = 0x1;

		ret = xh2a_write_kd_entry(ipu_dev,
					  (void *)&queue_addr[queue->wptr],
					  &kernel_desc);
		if (ret) {
			dev_err(ipu_dev->miscdev.this_device,
				"%s: write kernel descriptor failed\n",
				__func__);
			return;
		}
		/* update wptr of queue */
		queue->wptr = (queue->wptr + 1) % queue->entry_capacity;
	}

	/* record wptr of the first queue and add group to tile list */
	if (queue->flag & XH2A_IPU_TILE_QUEUE_FLAG_INTERRUPT) {
		group->queue = queue;
		list_add_tail(&group->tile_list_node, &queue->group_list);
		queue->flag &= ~XH2A_IPU_TILE_QUEUE_FLAG_INTERRUPT;
		dev_dbg(ipu_dev->miscdev.this_device,
			"group %u %p link to queue %d\n", group->id, group,
			core_id);
	}
}

void xh2a_booter_queue_clear_intr(struct xh2a_ipu_device *ipu_device,
				  struct xh2a_ipu_booter_queue *queue)
{
	struct booter_tile_reg_s *tile_reg = NULL;
	union que_ptr_s que_ptr_value = { 0 };

	tile_reg = (struct booter_tile_reg_s *)queue->reg_base;

	que_ptr_value.u.ker_done_intr = 0x1;

	/* clear kernel done interrupt */
	xh2a_ipu_writel(ipu_device, (uintptr_t)&tile_reg->que_ptr,
			que_ptr_value.val);
}

bool xh2a_booter_queue_get_intr_flag(struct xh2a_ipu_device *ipu_device,
				     struct xh2a_ipu_booter_queue *queue)
{
	struct booter_tile_reg_s *tile_reg = NULL;
	union que_ptr_s que_ptr_value = { 0 };

	tile_reg = (struct booter_tile_reg_s *)queue->reg_base;

	xh2a_ipu_readl(ipu_device, (uintptr_t)&tile_reg->que_ptr,
		       &que_ptr_value.val);

	return que_ptr_value.u.ker_done_intr & 0x1;
}

uint32_t xh2a_booter_queue_get_rptr(struct xh2a_ipu_device *ipu_device,
				    struct xh2a_ipu_booter_queue *queue)
{
	struct booter_tile_reg_s *tile_reg = NULL;
	union que_ptr_s que_ptr_value = { 0 };

	tile_reg = (struct booter_tile_reg_s *)queue->reg_base;
	/* get read pointer of queue */
	xh2a_ipu_readl(ipu_device, (uintptr_t)&tile_reg->que_ptr,
		       &que_ptr_value.val);

	return que_ptr_value.u.que_read_ptr;
}

bool xh2a_booter_queue_is_full(struct xh2a_ipu_device *ipu_device,
			       struct xh2a_ipu_booter_queue *queue)
{
	struct booter_tile_reg_s *tile_reg = NULL;
	union que_ptr_s que_ptr_value = { 0 };

	tile_reg = (struct booter_tile_reg_s *)queue->reg_base;
	/* get read pointer of queue */
	xh2a_ipu_readl(ipu_device, (uintptr_t)&tile_reg->que_ptr,
		       &que_ptr_value.val);

	return que_ptr_value.u.que_full;
}

void xh2a_ipu_update_load(struct xh2a_ipu_device *ipu_device,
			  uint32_t load_value)
{
	xh2a_ipu_writel(ipu_device, (uintptr_t)AOSS_SYSCTRL_IPU_LOAD_BASE,
			load_value);
}

static inline bool xh2a_queue_wptr_is_later(uint32_t new_ptr, uint32_t cur_ptr)
{
	return ((XH2A_IPU_TILE_ENTRY_NUM + new_ptr - cur_ptr) %
		XH2A_IPU_TILE_ENTRY_NUM) < (XH2A_IPU_TILE_ENTRY_NUM / 2);
}

void xh2a_booter_update_other_queues_rptr(struct xh2a_ipu_device *ipu_dev,
					  struct xh2a_ipu_group *group)
{
	uint32_t i, j, core_id, tile_id;
	struct xh2a_ipu_booter_queue *queue = NULL;

	for (i = 0; i < group->core_num; i++) {
		/* j=1, skip the first tile queue */
		for (j = (i == 0 ? 1 : 0); j < group->tile_num; j++) {
			core_id = group->target[i].core_id;
			tile_id = group->target[i].queue_id + j;
			queue = &ipu_dev->tile_queues[core_id][tile_id];

			mutex_lock(&queue->tile_mutex);

			queue->last_rptr = queue->rptr;
			dev_dbg(ipu_dev->miscdev.this_device,
				"[C%uT%u]:end_wptr: %u, rptr: %u\n", core_id,
				tile_id, group->target[i].end_wptr[j],
				queue->rptr);
			if (xh2a_queue_wptr_is_later(
				    group->target[i].end_wptr[j], queue->rptr))
				queue->rptr = group->target[i].end_wptr[j];

			/* process core1 groups in 2-core-group situation */
			if (i == 1 && j == 0) {
				xh2a_ipu_group_done_handler(ipu_dev, queue,
							    queue->rptr, false);
				queue->last_rptr = queue->rptr;
			}

			mutex_unlock(&queue->tile_mutex);
		}
	}
}

uint32_t xh2a_booter_queue_remain_space(struct xh2a_ipu_booter_queue *queue)
{
	uint32_t space;

	mutex_lock(&queue->tile_mutex);

	if (queue->wptr >= queue->rptr)
		space = queue->entry_capacity - queue->wptr + queue->rptr;
	else
		space = queue->rptr - queue->wptr;

	mutex_unlock(&queue->tile_mutex);

	return space;
}

void xh2a_ipu_booter_get_hw_context(struct xh2a_ipu_device *ipu_device,
				    uint32_t *result)
{
	/* To do */
	*result = 0x0;
}

static bool xh2a_is_tile_available(uint32_t available_tiles_mask,
				   uint32_t core_id, uint32_t tile_id)
{
	uint32_t bit_pos;

	if (core_id >= XH2A_IPU_CORE_NUM || tile_id >= XH2A_TILE_NUM_PER_CORE) {
		return false;
	}

	bit_pos = core_id * XH2A_TILE_NUM_PER_CORE + tile_id;

	return (available_tiles_mask & (1U << bit_pos)) != 0;
}

static void xh2a_ipu_booter_queue_init(struct xh2a_ipu_device *ipu_device,
				       struct booter_device_reg_s *booter_reg,
				       uint32_t core_id, uint32_t tile_id)
{
	uint64_t reg_value;
	uint32_t reg_offset;
	union que_cfg_s que_cfg_value = { 0 };
	struct booter_tile_reg_s *tile_reg = NULL;
	struct miscdevice *miscdev = &ipu_device->miscdev;
	struct xh2a_ipu_booter_queue *queue =
		&ipu_device->tile_queues[core_id][tile_id];

	if (!xh2a_is_tile_available(ipu_device->available_tiles_mask, core_id,
				    tile_id)) {
		queue->flag |= XH2A_IPU_TILE_QUEUE_FLAG_BAD;
		return;
	}

	queue->wptr = 0;
	queue->rptr = 0;
	queue->last_rptr = 0;

	queue->core_id = core_id;
	queue->tile_id = tile_id;
	queue->reg_base = (void *)&booter_reg->booter_tile_reg[tile_id];

	queue->entry_capacity = XH2A_IPU_TILE_ENTRY_NUM;

	queue->queue_addr =
		(void *)XH2A_DEVICE_DDR_BOOTER_START +
		(core_id * 4 + tile_id) *
			(queue->entry_capacity * sizeof(struct kernel_desc_s));

	dev_dbg(miscdev->this_device, "queue->queue_addr = 0x%lx\n",
		(uintptr_t)queue->queue_addr);

	tile_reg = (struct booter_tile_reg_s *)queue->reg_base;

	/* queue base addr low 32bit, align to 16 Byte */
	reg_offset = (uintptr_t)&tile_reg->que_base_addr_lo;
	reg_value = IPU_IOMAP((uintptr_t)queue->queue_addr);
	xh2a_ipu_writel(ipu_device, reg_offset, (reg_value & 0xFFFFFFFF));
	/* queue base addr high 8bit */
	reg_offset = (uintptr_t)&tile_reg->que_cfg;
	que_cfg_value.u.que_base_addr_hi = (reg_value >> 32) & 0xFF;
	/* depth of queue, capacity of KDs */
	que_cfg_value.u.que_set = 0;
	que_cfg_value.u.que_depth = queue->entry_capacity;
	xh2a_ipu_writel(ipu_device, reg_offset, que_cfg_value.val);

	/* que_set should be written 1 after que_base
	and que_depth is configured */
	que_cfg_value.u.que_set = 0x1;
	xh2a_ipu_writel(ipu_device, reg_offset, que_cfg_value.val);

	/* set nl error mask */
	reg_offset = IPUSS_TNODE_NL_BASE(core_id, tile_id) + 4;
	xh2a_ipu_writel(ipu_device, reg_offset, 1);
}

static void xh2a_ipu_get_available_core(struct xh2a_ipu_device *ipu_device,
					uint32_t core_mask)
{
	uint32_t mask = 0;
	uint32_t core_num = 0;

	if (core_mask & 0x01) {
		mask |= 0x0F;
		core_num++;
	}

	if (core_mask & 0x10) {
		mask |= 0xF0;
		core_num++;
	}

	ipu_device->available_cores_num = core_num;
	ipu_device->available_tiles_mask = mask;
}

int xh2a_ipu_get_efuse_info(struct xh2a_ipu_device *ipu_device)
{
	int ret;
	uint32_t core_mask;

	ret = xh2a_pcie_get_efuse_data(ipu_device->private_data,
				       XH2A_EFUSE_SUBTYPE_IPU_0_ROW,
				       XH2A_EFUSE_SUBTYPE_IPU_0_BIT,
				       XH2A_EFUSE_SUBTYPE_IPU_0_LENGTH,
				       &core_mask);
	if (ret) {
		dev_err(ipu_device->miscdev.this_device, "get available core "
							 "id from efuse "
							 "failed!\n");
		return ret;
	}

	xh2a_ipu_get_available_core(ipu_device, core_mask);

	return 0;
}

static void ipuss_lcrg_enable(struct xh2a_ipu_device *ipu_dev)
{
	int ret;
	unsigned long timeout;
	uint32_t reg_val;
	uint32_t niu_val, trycnt;
	uint32_t niu_ctrl_addr = AOSS_NIU_BASE + IPU_NIU_CTRL_OFFSET;
	uint32_t niu_status_addr = AOSS_NIU_BASE + IPU_NIU_STATUS_OFFSET;

	/* disconnect niu for lowpower process */
	trycnt = 0;
	do {
		xh2a_ipu_writel(ipu_dev, niu_ctrl_addr, IPU_NIU_DISCONNECT);
		xh2a_ipu_readl(ipu_dev, niu_status_addr, &niu_val);
		trycnt++;
	} while (((niu_val & IPU_NIU_MASK) == 0) && (trycnt < 1000));

	if (trycnt >= 1000) {
		dev_err(ipu_dev->miscdev.this_device, "niu disconnect "
						      "failed.\n");
	} else {
		dev_info(ipu_dev->miscdev.this_device, "niu disconnect ok.\n");
	}

	/* notify firmware to reset ipu */
	xh2a_ipu_writel(ipu_dev, AOSS_SYSCTRL_IPUSS_RESET_STATUS, 0);
	xh2a_ipu_readl(ipu_dev, AOSS_SYSCTRL_IPUSS_RESET_STATUS, &reg_val);
	ret = xh2a_pcie_write_bar_msgbit(ipu_dev->private_data,
					 IPUSS_RESET_REQUEST_BIT,
					 XH2A_PCIE_MSG_TO_E2);

	if (ret != 0) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: write msgbit fail, keep ipu niu disconnected.\n",
			__func__);
		return;
	}

	/* wait ipu reset */
	timeout = jiffies + msecs_to_jiffies(1000);
	while (time_before(jiffies, timeout)) {
		xh2a_ipu_readl(ipu_dev, AOSS_SYSCTRL_IPUSS_RESET_STATUS,
			       &reg_val);
		if ((reg_val & 0x1))
			break;
		usleep_range(10, 100);
	}

	xh2a_ipu_readl(ipu_dev, AOSS_SYSCTRL_IPUSS_RESET_STATUS, &reg_val);
	if (!(reg_val & 0x1)) {
		dev_err(ipu_dev->miscdev.this_device,
			"%s: get ipureset fail, check firmware version...\n",
			__func__);
		/*  compatible with different firmware. DO not return here. */
	}

	trycnt = 0;
	do {
		xh2a_ipu_writel(ipu_dev, niu_ctrl_addr, IPU_NIU_CONNECT);
		xh2a_ipu_readl(ipu_dev, niu_status_addr, &niu_val);
		trycnt++;
	} while (((niu_val & IPU_NIU_MASK) != 0) && (trycnt < 1000));

	if (trycnt >= 1000) {
		dev_err(ipu_dev->miscdev.this_device, "niu connect failed.\n");
	} else {
		dev_info(ipu_dev->miscdev.this_device, "niu connect ok.\n");
	}
}

void xh2a_ipu_booter_pre_startup(struct xh2a_ipu_device *ipu_device)
{
	uint32_t core_id, tile_id;
	struct xh2a_ipu_booter_queue *queue;

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		for (tile_id = 0; tile_id < XH2A_TILE_NUM_PER_CORE; tile_id++) {
			queue = &ipu_device->tile_queues[core_id][tile_id];

			mutex_init(&queue->tile_mutex);
			INIT_LIST_HEAD(&queue->group_list);
		}
	}
}

static void xh2a_ipu_prefetch_init(struct xh2a_ipu_device *ipu_device)
{
	uint32_t core_id, tile_id, core_reg_addr, cache_cfg_addr, mask;

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		mask = (0xF << (core_id * 4));
		if ((ipu_device->available_tiles_mask & mask) != mask)
			continue;

		core_reg_addr =
			IPUSS_CFG_BASE + IPUSS_CORE_OFFSET * (core_id + 1);

		for (tile_id = 0; tile_id < XH2A_TILE_NUM_PER_CORE; tile_id++) {
			cache_cfg_addr = core_reg_addr + 0x10000 * tile_id +
					 IPUSS_CACHE_CFG_OFFSET;
			xh2a_ipu_writel(ipu_device, cache_cfg_addr, 0x12900);
		}
	}
}

static void xh2a_ipu_sw_interleave_init(struct xh2a_ipu_device *ipu_device)
{
	uint32_t core_id, tile_id, core_reg_addr, sw_interleave_addr, mask;

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		mask = (0xF << (core_id * 4));
		if ((ipu_device->available_tiles_mask & mask) != mask)
			continue;

		core_reg_addr =
			IPUSS_CFG_BASE + IPUSS_CORE_OFFSET * (core_id + 1);

		for (tile_id = 0; tile_id < XH2A_TILE_NUM_PER_CORE; tile_id++) {
			sw_interleave_addr = core_reg_addr + 0x10000 * tile_id +
					     IPUSS_SW_INTERLEAVE_OFFSET;
			xh2a_ipu_writel(ipu_device, sw_interleave_addr, 0x1);
		}
	}
}

void xh2a_ipu_hw_startup(struct xh2a_ipu_device *ipu_device)
{
	uint32_t core_id, tile_id, booter_reg_addr;
	struct booter_device_reg_s *booter_reg = NULL;

	ipuss_lcrg_enable(ipu_device);

	for (core_id = 0; core_id < XH2A_IPU_CORE_NUM; core_id++) {
		booter_reg_addr = IPUSS_CFG_BASE +
				  IPUSS_CORE_OFFSET * (core_id + 1) +
				  IPUSS_GNODE_OFFSET + IPUSS_BOOTER_OFFSET;

		booter_reg = (struct booter_device_reg_s
				      *)(uintptr_t)(booter_reg_addr);

		for (tile_id = 0; tile_id < XH2A_TILE_NUM_PER_CORE; tile_id++)
			xh2a_ipu_booter_queue_init(ipu_device, booter_reg,
						   core_id, tile_id);
	}

	xh2a_ipu_prefetch_init(ipu_device);

	xh2a_ipu_sw_interleave_init(ipu_device);
	dev_dbg(ipu_device->miscdev.this_device, "ipuss hw startup done\n");
}

void xh2a_ipu_hw_shutdown(struct xh2a_ipu_device *ipu_dev)
{
	uint32_t niu_val, trycnt;
	uint32_t niu_ctrl_addr = AOSS_NIU_BASE + IPU_NIU_CTRL_OFFSET;
	uint32_t niu_status_addr = AOSS_NIU_BASE + IPU_NIU_STATUS_OFFSET;

	/* ONLY disconnect NIU when shutdown ipu. DO NOT reset it! */
	trycnt = 0;
	do {
		xh2a_ipu_writel(ipu_dev, niu_ctrl_addr, IPU_NIU_DISCONNECT);
		xh2a_ipu_readl(ipu_dev, niu_status_addr, &niu_val);
		trycnt++;
	} while (((niu_val & IPU_NIU_MASK) == 0) && (trycnt < 1000));

	if (trycnt >= 1000) {
		dev_err(ipu_dev->miscdev.this_device, "niu disconnect "
						      "failed.\n");
	} else {
		dev_info(ipu_dev->miscdev.this_device, "niu disconnect ok.\n");
	}
}

static void xh2a_dump_curr_kd(struct xh2a_ipu_device *ipu_dev,
			      struct xh2a_ipu_booter_queue *queue,
			      uint32_t que_ptr, uint32_t end_ptr,
			      uint32_t kernel_num)
{
	struct kernel_desc_s *queue_addr =
		(struct kernel_desc_s *)queue->queue_addr;
	struct kernel_desc_s kd;
	uint32_t rptr = (que_ptr >> 16) & 0xFFF;
	uint32_t start_ptr;

	start_ptr = (queue->entry_capacity + end_ptr - kernel_num);
	start_ptr %= queue->entry_capacity;

	/*  if (xh2a_ipu_calc_queue_delta(start_ptr, que_ptr) >= kernel_num) */
	/*  	return; */

	dev_info(ipu_dev->miscdev.this_device,
		 "C%uT%u queue addr: %08lx, rptr: %u(0x%x), kd: %08lx\n",
		 queue->core_id, queue->tile_id, (uintptr_t)queue->queue_addr,
		 rptr, rptr, (uintptr_t)(queue_addr + rptr));

	if (!xh2a_pcie_pio_read_mem(ipu_dev->private_data,
				    (uintptr_t)(queue_addr + rptr), &kd,
				    sizeof(kd)))
		print_hex_dump(KERN_INFO, "KD: ", DUMP_PREFIX_OFFSET, 16, 4,
			       &kd, sizeof(kd), false);
}

static void xh2a_dump_core_reg(struct xh2a_ipu_device *ipu_dev,
			       uint32_t core_id)
{
	uint32_t idx, addr;
	uint32_t core_top_err, gnode_mbus_err, gnode_top_err, load_err[2],
		resizer_err, store_err;

	xh2a_ipu_readl(ipu_dev, IPUSS_CORE_TOP_ERR(core_id), &core_top_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%u]%-10s = 0x%08x\n",
		 core_id, "CORE-TOP-ERR", core_top_err);

	xh2a_ipu_readl(ipu_dev, IPUSS_GNODE_MBUS_ERR(core_id), &gnode_mbus_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%u]%-10s = 0x%08x\n",
		 core_id, "GNODE-MBUS-ERR", gnode_mbus_err);

	xh2a_ipu_readl(ipu_dev, IPUSS_GNODE_TOP_ERR(core_id), &gnode_top_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%u]%-10s = 0x%08x\n",
		 core_id, "GNODE-TOP-ERR", gnode_top_err);

	for (idx = 0; idx < 2; idx++) {
		addr = IPUSS_LOAD_ERR(core_id, idx);
		xh2a_ipu_readl(ipu_dev, addr, &load_err[idx]);
	}

	dev_info(ipu_dev->miscdev.this_device,
		 "  [C%u]%-10s = load1:%08x load0:%08x\n", core_id, "LOAD-ERR",
		 load_err[0], load_err[1]);

	xh2a_ipu_readl(ipu_dev, IPUSS_RESIZER_ERR(core_id), &resizer_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%u]%-10s = 0x%08x\n",
		 core_id, "RESIZER-ERR", resizer_err);

	xh2a_ipu_readl(ipu_dev, IPUSS_STORE_ERR(core_id), &store_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%u]%-10s = 0x%08x\n",
		 core_id, "STORE-ERR", store_err);
}

static void xh2a_dump_tile_reg(struct xh2a_ipu_device *ipu_dev,
			       uint32_t core_id, uint32_t tile_id)
{
	uint32_t idx, addr;
	uint32_t pcs[4], rv_info[4], rv_err[4], vp_err0[4], vp_err1[4], te_err,
		nl_err, icache_err, tile_top_err, tnode_booter_err,
		tnode_mbus_err;

	for (idx = 0; idx < 4; idx++) {
		addr = IPUSS_TNODE_RV_PC(core_id, tile_id, idx);
		xh2a_ipu_readl(ipu_dev, addr, &pcs[idx]);
		addr = IPUSS_TNODE_RV_INFO(core_id, tile_id, idx);
		xh2a_ipu_readl(ipu_dev, addr, &rv_info[idx]);
		addr = IPUSS_TNODE_RV_ERR_INFO(core_id, tile_id, idx);
		xh2a_ipu_readl(ipu_dev, addr, &rv_err[idx]);
		addr = IPUSS_TNODE_VP_ERR_INFO0(core_id, tile_id, idx);
		xh2a_ipu_readl(ipu_dev, addr, &vp_err0[idx]);
		addr = IPUSS_TNODE_VP_ERR_INFO1(core_id, tile_id, idx);
		xh2a_ipu_readl(ipu_dev, addr, &vp_err1[idx]);
		dev_dbg(ipu_dev->miscdev.this_device,
			"  [C%uT%u] %u pc addr: 0x%08x\n", core_id, tile_id,
			idx, addr);
	}

	dev_info(ipu_dev->miscdev.this_device,
		 "  [C%uT%u]%-10s = %08x %08x %08x %08x", core_id, tile_id,
		 "RV-PC", pcs[0], pcs[1], pcs[2], pcs[3]);
	dev_info(ipu_dev->miscdev.this_device,
		 "  [C%uT%u]%-10s = %08x %08x %08x %08x", core_id, tile_id,
		 "RV-INFO", rv_info[0], rv_info[1], rv_info[2], rv_info[3]);
	dev_info(ipu_dev->miscdev.this_device,
		 "  [C%uT%u]%-10s = %08x %08x %08x %08x", core_id, tile_id,
		 "RV-ERR", rv_err[0], rv_err[1], rv_err[2], rv_err[3]);
	dev_info(ipu_dev->miscdev.this_device,
		 "  [C%uT%u]%-10s = %08x %08x %08x %08x", core_id, tile_id,
		 "VP-ERR0", vp_err0[0], vp_err0[1], vp_err0[2], vp_err0[3]);
	dev_info(ipu_dev->miscdev.this_device,
		 "  [C%uT%u]%-10s = %08x %08x %08x %08x", core_id, tile_id,
		 "VP-ERR1", vp_err1[0], vp_err1[1], vp_err1[2], vp_err1[3]);

	addr = IPUSS_TNODE_TE_ERR(core_id, tile_id);
	xh2a_ipu_readl(ipu_dev, addr, &te_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%uT%u]%-10s = 0x%08x\n",
		 core_id, tile_id, "TE-ERR", te_err);

	addr = IPUSS_TNODE_NL_BASE(core_id, tile_id);
	xh2a_ipu_readl(ipu_dev, addr, &nl_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%uT%u]%-10s = 0x%08x\n",
		 core_id, tile_id, "NL-ERR", nl_err);

	addr = IPUSS_L2ICACHE_ERR(core_id, tile_id);
	xh2a_ipu_readl(ipu_dev, addr, &icache_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%uT%u]%-10s = 0x%08x\n",
		 core_id, tile_id, "ICACHE-ERR", icache_err);

	addr = IPUSS_TILE_TOP_ERR(core_id, tile_id);
	xh2a_ipu_readl(ipu_dev, addr, &tile_top_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%uT%u]%-10s = 0x%08x\n",
		 core_id, tile_id, "TILE-TOP-ERR", tile_top_err);

	addr = IPUSS_TNODE_BOOTER_ERR(core_id, tile_id);
	xh2a_ipu_readl(ipu_dev, addr, &tnode_booter_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%uT%u]%-10s = 0x%08x\n",
		 core_id, tile_id, "TNODE-BOOTER-ERR", tnode_booter_err);

	addr = IPUSS_TNODE_MBUS_ERR(core_id, tile_id);
	xh2a_ipu_readl(ipu_dev, addr, &tnode_mbus_err);
	dev_info(ipu_dev->miscdev.this_device, "  [C%uT%u]%-10s = 0x%08x\n",
		 core_id, tile_id, "TNODE-MBUS-ERR", tnode_mbus_err);
}

void xh2a_dump_debug_regs(struct xh2a_ipu_device *ipu_dev,
			  struct xh2a_ipu_group *group)
{
	uint32_t idx, core_id, tile_id, reg_val;
	uint32_t booter_reg_addr, core_reg_addr;
	uint32_t kernel_num;
	struct booter_tile_reg_s *tile_reg = NULL;
	struct xh2a_ipu_booter_queue *queue;
	struct miscdevice *miscdev = &ipu_dev->miscdev;

	mutex_lock(&ipu_dev->dump_mutex);
	/* TODO: lock group? */
	dev_info(miscdev->this_device,
		 "==== DUMP BOOTER REGS for group %u %p ====\n", group->id,
		 group);

	kernel_num = group->kernel_num;
	for (idx = 0; idx < group->core_num; idx++) {
		core_id = group->target[idx].core_id;
		core_reg_addr =
			IPUSS_CFG_BASE + IPUSS_CORE_OFFSET * (core_id + 1);
		booter_reg_addr = core_reg_addr + IPUSS_GNODE_OFFSET +
				  IPUSS_BOOTER_OFFSET;

		xh2a_ipu_readl(ipu_dev, booter_reg_addr + 0x80, &reg_val);
		dev_info(miscdev->this_device, "[Core%u]%-10s = 0x%08x\n",
			 core_id, "err_info", reg_val);

		xh2a_dump_core_reg(ipu_dev, core_id);

		for (tile_id = 0; tile_id < XH2A_TILE_NUM_PER_CORE; tile_id++) {
			queue = &ipu_dev->tile_queues[core_id][tile_id];
			tile_reg = (struct booter_tile_reg_s *)queue->reg_base;

			xh2a_ipu_readl(ipu_dev, (uintptr_t)&tile_reg->que_cfg,
				       &reg_val);
			dev_info(miscdev->this_device,
				 "  [C%uT%u]%-10s = 0x%08x\n", core_id, tile_id,
				 "que_cfg", reg_val);

			xh2a_ipu_readl(ipu_dev, (uintptr_t)&tile_reg->que_ptr,
				       &reg_val);
			dev_info(miscdev->this_device,
				 "  [C%uT%u]%-10s = 0x%08x\n", core_id, tile_id,
				 "que_ptr", reg_val);
			dev_info(miscdev->this_device, "  [C%uT%u]%-10s = %u\n",
				 core_id, tile_id, "sw_group_wptr",
				 group->target[idx].end_wptr[tile_id]);
			dev_info(miscdev->this_device, "  [C%uT%u]%-10s = %u\n",
				 core_id, tile_id, "sw_rptr", queue->rptr);
			dev_info(miscdev->this_device, "  [C%uT%u]%-10s = %u\n",
				 core_id, tile_id, "sw_last_rptr",
				 queue->last_rptr);
			dev_info(miscdev->this_device, "  [C%uT%u]%-10s = %u\n",
				 core_id, tile_id, "sw_wptr", queue->wptr);
			dev_info(miscdev->this_device, "  [C%uT%u]%-10s = %u\n",
				 core_id, tile_id, "group_kernel_num",
				 group->kernel_num);
			xh2a_dump_tile_reg(ipu_dev, core_id, tile_id);
			xh2a_dump_curr_kd(ipu_dev, queue, reg_val,
					  group->target[idx].end_wptr[tile_id],
					  kernel_num);
		}
	}
	mutex_unlock(&ipu_dev->dump_mutex);
}
