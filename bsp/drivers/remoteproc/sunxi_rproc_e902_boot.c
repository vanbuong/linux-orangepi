/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Allwinner sunxi remoteproc E902 (XuanTie openE902) boot driver
 *
 * Used by A733 / sun60iw2 CPUS RISC-V subsystem (E902_CFG @ 0x07032000).
 *
 * Copyright (c) 2020-2025, Allwinnertech
 * Copyright (c) 2026
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <asm/io.h>

#include "sunxi_rproc_boot.h"
#include <sunxi-log.h>

#ifdef dev_fmt
#undef dev_fmt
#define dev_fmt(fmt) fmt
#endif

/*
 * E902_CFG registers (Allwinner A733 User Manual V0.92, section 5.2)
 * Base address: 0x07032000
 */
#define E902_AUTO_GATING_REG		(0x0004)
#define E902_DDR_REMAP_REG		(0x0020)
#define E902_TS_TMODE_SEL_REG		(0x0040)
#define E902_WAKEUP_IRQ_NUM_REG		(0x0060)
#define E902_WAKEUP_MASK0_REG		(0x0064)
#define E902_WAKEUP_MASK1_REG		(0x0068)
#define E902_PAD_LPMD_REG		(0x0080)
#define E902_RST_START_ADDR_REG		(0x0204)

#define E902_AUTOGATE_EN		BIT(0)
#define E902_DDR_REMAP_MASK		GENMASK(3, 0)
#define E902_RST_START_ADDR_ALIGN	BIT(0)

#define RPROC_NAME			"e902"

extern int simulator_debug;

static int sunxi_rproc_e902_assert(struct sunxi_rproc_priv *rproc_priv);
static int sunxi_rproc_e902_deassert(struct sunxi_rproc_priv *rproc_priv);

static int sunxi_rproc_e902_resource_get(struct sunxi_rproc_priv *rproc_priv,
					 struct platform_device *pdev)
{
	struct sunxi_rproc_e902_cfg *cfg;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource *res;
	u32 *map_array;
	u32 remap = 0;
	int ret, i;

	rproc_priv->dev = dev;

	cfg = devm_kzalloc(dev, sizeof(*cfg), GFP_KERNEL);
	if (!cfg) {
		dev_err(dev, "alloc e902 cfg error\n");
		return -ENOMEM;
	}

	cfg->mod_clk = devm_clk_get(dev, "mod");
	if (IS_ERR_OR_NULL(cfg->mod_clk)) {
		dev_err(dev, "no find mod in dts\n");
		return -ENXIO;
	}

	cfg->cfg_clk = devm_clk_get(dev, "cfg");
	if (IS_ERR_OR_NULL(cfg->cfg_clk)) {
		dev_err(dev, "no find cfg in dts\n");
		return -ENXIO;
	}

	/* Optional 24M timestamp clock (ts_clk in the E902 block diagram). */
	cfg->ts_clk = devm_clk_get_optional(dev, "ts");
	if (IS_ERR(cfg->ts_clk)) {
		dev_err(dev, "failed to get optional ts clock\n");
		return PTR_ERR(cfg->ts_clk);
	}

	cfg->cfg_rst = devm_reset_control_get(dev, "cfg-rst");
	if (IS_ERR_OR_NULL(cfg->cfg_rst)) {
		dev_err(dev, "no find cfg-rst in dts\n");
		return -ENXIO;
	}

	/*
	 * Optional: some platforms expose a separate core/mod reset. A733
	 * sun60iw2-r-ccu currently only documents RST_BUS_RISCV_CFG.
	 */
	cfg->mod_rst = devm_reset_control_get_optional(dev, "mod-rst");
	if (IS_ERR(cfg->mod_rst)) {
		dev_err(dev, "failed to get optional mod-rst\n");
		return PTR_ERR(cfg->mod_rst);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "e902-cfg");
	if (IS_ERR_OR_NULL(res)) {
		dev_err(dev, "no find e902-cfg in dts\n");
		return -ENXIO;
	}

	cfg->e902_cfg = devm_ioremap_resource(dev, res);
	if (IS_ERR_OR_NULL(cfg->e902_cfg)) {
		dev_err(dev, "fail to ioremap e902-cfg\n");
		return -ENXIO;
	}

	/*
	 * Optional DDR remap for E902's 1GB window at 0x8000_0000..0xBFFF_FFFF.
	 * See E902_DDR_REMAP_REG encoding in the A733 user manual.
	 */
	if (!of_property_read_u32(np, "ddr-remap", &remap)) {
		if (remap > 0xf) {
			dev_err(dev, "invalid ddr-remap value %#x\n", remap);
			return -EINVAL;
		}
		cfg->ddr_remap = remap & E902_DDR_REMAP_MASK;
		cfg->ddr_remap_valid = true;
	}

	ret = of_property_count_elems_of_size(np, "memory-mappings", sizeof(u32) * 3);
	if (ret <= 0) {
		dev_err(dev, "fail to get memory-mappings\n");
		return -ENXIO;
	}

	rproc_priv->mem_maps_cnt = ret;
	rproc_priv->mem_maps = devm_kcalloc(dev, rproc_priv->mem_maps_cnt,
					    sizeof(*(rproc_priv->mem_maps)),
					    GFP_KERNEL);
	if (!rproc_priv->mem_maps)
		return -ENOMEM;

	map_array = devm_kcalloc(dev, rproc_priv->mem_maps_cnt * 3, sizeof(u32), GFP_KERNEL);
	if (!map_array)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, "memory-mappings", map_array,
					 rproc_priv->mem_maps_cnt * 3);
	if (ret) {
		dev_err(dev, "fail to read memory-mappings\n");
		return -ENXIO;
	}

	for (i = 0; i < rproc_priv->mem_maps_cnt; i++) {
		rproc_priv->mem_maps[i].da = map_array[i * 3];
		rproc_priv->mem_maps[i].len = map_array[i * 3 + 1];
		rproc_priv->mem_maps[i].pa = map_array[i * 3 + 2];
		dev_dbg(dev, "memory-mappings[%d]: da: 0x%llx, len: 0x%llx, pa: 0x%llx\n",
			i, rproc_priv->mem_maps[i].da, rproc_priv->mem_maps[i].len,
			rproc_priv->mem_maps[i].pa);
	}

	devm_kfree(dev, map_array);
	rproc_priv->rproc_cfg = cfg;

	return 0;
}

static void sunxi_rproc_e902_resource_put(struct sunxi_rproc_priv *rproc_priv,
					  struct platform_device *pdev)
{
}

static int sunxi_rproc_e902_start(struct sunxi_rproc_priv *rproc_priv)
{
	struct sunxi_rproc_e902_cfg *cfg = rproc_priv->rproc_cfg;
	struct device *dev = rproc_priv->dev;
	u32 start_addr;
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	if (simulator_debug) {
		dev_dbg(dev, "e902 simulator mode, skip clk/reset programming\n");
		return 0;
	}

	ret = sunxi_rproc_e902_assert(rproc_priv);
	if (ret) {
		dev_err(dev, "rproc assert err\n");
		return ret;
	}

	ret = clk_prepare_enable(cfg->cfg_clk);
	if (ret) {
		dev_err(dev, "cfg clk enable err\n");
		return ret;
	}

	if (cfg->ts_clk) {
		ret = clk_prepare_enable(cfg->ts_clk);
		if (ret) {
			dev_err(dev, "ts clk enable err\n");
			goto err_disable_cfg_clk;
		}
	}

	/*
	 * Deassert CFG reset so E902_CFG is accessible, then program the
	 * start vector before enabling the core clock. The manual requires
	 * RST_START_ADDR to be configured before the E902 reset is released;
	 * with only CFG reset exposed on sun60iw2, hold the core ungated
	 * (mod clk off) until the vector is written.
	 */
	ret = sunxi_rproc_e902_deassert(rproc_priv);
	if (ret) {
		dev_err(dev, "rproc deassert err\n");
		goto err_disable_ts_clk;
	}

	if (cfg->ddr_remap_valid) {
		writel(cfg->ddr_remap & E902_DDR_REMAP_MASK,
		       cfg->e902_cfg + E902_DDR_REMAP_REG);
		dev_info(dev, "e902 ddr-remap set to %#x\n", cfg->ddr_remap);
	}

	/* Bit0 of RST_START_ADDR is fixed to 0. */
	start_addr = rproc_priv->pc_entry & ~E902_RST_START_ADDR_ALIGN;
	writel(start_addr, cfg->e902_cfg + E902_RST_START_ADDR_REG);
	dev_info(dev, "e902 boot address: 0x%08x\n", start_addr);

	ret = clk_prepare_enable(cfg->mod_clk);
	if (ret) {
		dev_err(dev, "mod clk enable err\n");
		goto err_disable_ts_clk;
	}

	return 0;

err_disable_ts_clk:
	if (cfg->ts_clk)
		clk_disable_unprepare(cfg->ts_clk);
err_disable_cfg_clk:
	clk_disable_unprepare(cfg->cfg_clk);
	return ret;
}

static int sunxi_rproc_e902_stop(struct sunxi_rproc_priv *rproc_priv)
{
	struct sunxi_rproc_e902_cfg *cfg = rproc_priv->rproc_cfg;
	int ret;

	dev_dbg(rproc_priv->dev, "%s\n", __func__);

	if (simulator_debug) {
		dev_dbg(rproc_priv->dev, "e902 simulator mode, skip clk/reset teardown\n");
		return 0;
	}

	clk_disable_unprepare(cfg->mod_clk);

	ret = sunxi_rproc_e902_assert(rproc_priv);
	if (ret) {
		dev_err(rproc_priv->dev, "rproc assert err\n");
		return ret;
	}

	if (cfg->ts_clk)
		clk_disable_unprepare(cfg->ts_clk);

	clk_disable_unprepare(cfg->cfg_clk);

	return 0;
}

static int sunxi_rproc_e902_attach(struct sunxi_rproc_priv *rproc_priv)
{
	struct sunxi_rproc_e902_cfg *cfg = rproc_priv->rproc_cfg;
	struct device *dev = rproc_priv->dev;
	int ret;

	dev_dbg(dev, "%s\n", __func__);

	ret = sunxi_rproc_e902_deassert(rproc_priv);
	if (ret) {
		dev_err(dev, "rproc deassert err\n");
		return ret;
	}

	ret = clk_prepare_enable(cfg->cfg_clk);
	if (ret) {
		dev_err(dev, "cfg clk enable err\n");
		return ret;
	}

	if (cfg->ts_clk) {
		ret = clk_prepare_enable(cfg->ts_clk);
		if (ret) {
			dev_err(dev, "ts clk enable err\n");
			clk_disable_unprepare(cfg->cfg_clk);
			return ret;
		}
	}

	ret = clk_prepare_enable(cfg->mod_clk);
	if (ret) {
		dev_err(dev, "mod clk enable err\n");
		if (cfg->ts_clk)
			clk_disable_unprepare(cfg->ts_clk);
		clk_disable_unprepare(cfg->cfg_clk);
		return ret;
	}

	return 0;
}

static int sunxi_rproc_e902_assert(struct sunxi_rproc_priv *rproc_priv)
{
	struct sunxi_rproc_e902_cfg *cfg = rproc_priv->rproc_cfg;
	int ret;

	if (cfg->mod_rst) {
		ret = reset_control_assert(cfg->mod_rst);
		if (ret) {
			dev_err(rproc_priv->dev, "mod rst assert err\n");
			return -ENXIO;
		}
	}

	ret = reset_control_assert(cfg->cfg_rst);
	if (ret) {
		dev_err(rproc_priv->dev, "cfg rst assert err\n");
		return -ENXIO;
	}

	return 0;
}

static int sunxi_rproc_e902_deassert(struct sunxi_rproc_priv *rproc_priv)
{
	struct sunxi_rproc_e902_cfg *cfg = rproc_priv->rproc_cfg;
	int ret;

	ret = reset_control_deassert(cfg->cfg_rst);
	if (ret) {
		dev_err(rproc_priv->dev, "cfg rst de-assert err\n");
		return -ENXIO;
	}

	if (cfg->mod_rst) {
		ret = reset_control_deassert(cfg->mod_rst);
		if (ret) {
			dev_err(rproc_priv->dev, "mod rst de-assert err\n");
			return -ENXIO;
		}
	}

	return 0;
}

static int sunxi_rproc_e902_reset(struct sunxi_rproc_priv *rproc_priv)
{
	int ret;

	ret = sunxi_rproc_e902_assert(rproc_priv);
	if (ret)
		return ret;

	return sunxi_rproc_e902_deassert(rproc_priv);
}

static int sunxi_rproc_e902_enable_sram(struct sunxi_rproc_priv *rproc_priv, u32 value)
{
	/* A733 CPUS SRAM A2 has no dedicated pubsram gate in sun60iw2-r-ccu. */
	return 0;
}

static int sunxi_rproc_e902_set_runstall(struct sunxi_rproc_priv *rproc_priv, u32 value)
{
	/* E902 has no runstall bit like some DSP cores. */
	return 0;
}

static bool sunxi_rproc_e902_is_booted(struct sunxi_rproc_priv *rproc_priv)
{
	struct sunxi_rproc_e902_cfg *cfg = rproc_priv->rproc_cfg;

	return __clk_is_enabled(cfg->mod_clk);
}

static struct sunxi_rproc_ops sunxi_rproc_e902_ops = {
	.resource_get = sunxi_rproc_e902_resource_get,
	.resource_put = sunxi_rproc_e902_resource_put,
	.start = sunxi_rproc_e902_start,
	.stop = sunxi_rproc_e902_stop,
	.attach = sunxi_rproc_e902_attach,
	.reset = sunxi_rproc_e902_reset,
	.set_localram = sunxi_rproc_e902_enable_sram,
	.set_runstall = sunxi_rproc_e902_set_runstall,
	.is_booted = sunxi_rproc_e902_is_booted,
};

static int __init sunxi_rproc_e902_boot_init(void)
{
	int ret;

	ret = sunxi_rproc_priv_ops_register(RPROC_NAME, &sunxi_rproc_e902_ops, NULL);
	if (ret) {
		sunxi_err(NULL, "rproc(" RPROC_NAME ") register ops failed, ret: %d\n", ret);
		return ret;
	}

	return 0;
}
subsys_initcall(sunxi_rproc_e902_boot_init);

static void __exit sunxi_rproc_e902_boot_exit(void)
{
	int ret;

	ret = sunxi_rproc_priv_ops_unregister(RPROC_NAME);
	if (ret)
		sunxi_err(NULL, "rproc(" RPROC_NAME ") unregister ops failed, ret: %d\n", ret);
}
module_exit(sunxi_rproc_e902_boot_exit)

MODULE_DESCRIPTION("Allwinner sunxi rproc e902 boot driver");
MODULE_AUTHOR("Cursor Agent");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0.0");
