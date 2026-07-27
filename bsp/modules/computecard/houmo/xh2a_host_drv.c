// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */
#include <linux/module.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>

static struct xh2a_ctl_ctx *ctl_ctx;
static BLOCKING_NOTIFIER_HEAD(xh2a_host_notifier_list);

int xh2a_host_register_notifier_chain(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&xh2a_host_notifier_list, nb);
}
EXPORT_SYMBOL(xh2a_host_register_notifier_chain);

void xh2a_host_unregister_notifier_chain(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&xh2a_host_notifier_list, nb);
}
EXPORT_SYMBOL(xh2a_host_unregister_notifier_chain);

int xh2a_host_call_notifier_chain(int val, void *v)
{
	return blocking_notifier_call_chain(&xh2a_host_notifier_list, val, v);
}
EXPORT_SYMBOL(xh2a_host_call_notifier_chain);

int xh2a_ctl_get_ctx(struct xh2a_ctl_ctx **ctl_out)
{
	if (!ctl_ctx)
		return -ENODEV;

	*ctl_out = ctl_ctx;

	return 0;
}
EXPORT_SYMBOL(xh2a_ctl_get_ctx);

static int __init xh2a_host_init(void)
{
	int rc;

	pr_info("%s: installing xh2a driver version %s: %s\n", __func__,
		XH2A_HOST_DRIVER_RELEASE, XH2A_HOST_DRIVER_BUILDTIME);

	rc = xh2a_ctl_init(&ctl_ctx);
	if (rc < 0) {
		pr_err("%s: xh2a_ctl_init failed\n", __func__);
		goto module_exit;
	}

	xh2a_sys_register_driver();

	xh2a_memory_allocator_register_driver();

	xh2a_memory_transfer_register_driver();

	xh2a_transport_register_driver();

	xh2a_qspi_register_driver();

	xh2a_i2c_register_driver();

	xh2a_ipu_register_driver();

	xh2a_rpmsg_register_driver();

	xh2a_fast_memory_register_driver();

	rc = xh2a_pcie_register_driver();

	if (rc < 0) {
		pr_info("%s: NO xh2a PCI driver found.\n", __func__);
		rc = -ENODEV;
		xh2a_ctl_exit(ctl_ctx);
		ctl_ctx = NULL;
		goto module_exit;
	}

	return 0;

module_exit:

	return rc;
}

static void __exit xh2a_host_exit(void)
{
	xh2a_pcie_unregister_driver();

	xh2a_fast_memory_unregister_driver();

	xh2a_rpmsg_unregister_driver();

	xh2a_ipu_unregister_driver();

	xh2a_sys_unregister_driver();

	xh2a_memory_allocator_unregister_driver();

	xh2a_memory_transfer_unregister_driver();

	xh2a_transport_unregister_driver();

	xh2a_qspi_unregister_driver();

	xh2a_i2c_unregister_driver();

	xh2a_ctl_exit(ctl_ctx);
	ctl_ctx = NULL;

	pr_info("%s: removed xh2a driver version %s: %s\n", __func__,
		XH2A_HOST_DRIVER_RELEASE, XH2A_HOST_DRIVER_BUILDTIME);
}

module_init(xh2a_host_init);
module_exit(xh2a_host_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hongxing.ma@houmo.ai");
MODULE_DESCRIPTION("xh2a host driver");
MODULE_VERSION(XH2A_HOST_DRIVER_RELEASE);
