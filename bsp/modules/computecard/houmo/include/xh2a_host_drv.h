// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_HOST_DRV_H_
#define _XH2A_HOST_DRV_H_

/*
 * xh2a_host_register_notifier_chain() - register notifier chain
 * @nb: notifier block to register
 * Return: 0 on success, negative error code on failure
 */
int xh2a_host_register_notifier_chain(struct notifier_block *nb);

/*
 * xh2a_host_unregister_notifier_chain() - unregister notifier chain
 * @nb: notifier block to unregister
 */
void xh2a_host_unregister_notifier_chain(struct notifier_block *nb);

/*
 * xh2a_host_call_notifier_chain() - call notifier chain
 * @val: event value
 * @v: notifier chain
 * Return: 0 on success, negative error code on failure
 */
int xh2a_host_call_notifier_chain(int val, void *v);

/*
 * XH2A_HOST_NOTIFY_PCIE_PROBE - notify when PCIe device is probed
 * XH2A_HOST_NOTIFY_PCIE_REMOVE - notify when PCIe device is removed
 * XH2A_HOST_NOTIFY_PCIE_SHUTDOWN - notify when PCIe device is shutdown
 */
#define XH2A_HOST_NOTIFY_PCIE_PROBE    100
#define XH2A_HOST_NOTIFY_PCIE_REMOVE   200
#define XH2A_HOST_NOTIFY_PCIE_SHUTDOWN 300

int xh2a_pcie_register_driver(void);
void xh2a_pcie_unregister_driver(void);

int xh2a_sys_register_driver(void);
void xh2a_sys_unregister_driver(void);

int xh2a_memory_allocator_register_driver(void);
void xh2a_memory_allocator_unregister_driver(void);

int xh2a_memory_transfer_register_driver(void);
void xh2a_memory_transfer_unregister_driver(void);

int xh2a_transport_register_driver(void);
void xh2a_transport_unregister_driver(void);

int xh2a_qspi_register_driver(void);
void xh2a_qspi_unregister_driver(void);

int xh2a_i2c_register_driver(void);
void xh2a_i2c_unregister_driver(void);

int xh2a_ipu_register_driver(void);
void xh2a_ipu_unregister_driver(void);

int xh2a_rpmsg_register_driver(void);
void xh2a_rpmsg_unregister_driver(void);

int xh2a_fast_memory_register_driver(void);
void xh2a_fast_memory_unregister_driver(void);

#endif
