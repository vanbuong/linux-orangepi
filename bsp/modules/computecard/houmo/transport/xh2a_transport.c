// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

/*
 * xh2a_transport is a simple protocol to save/restore xh2a's image.
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>
#include "xh2a_transport.h"

/*
 * xh2a_transport_probe() - probe function for transport device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_transport_probe(void *handle)
{
	int ret;
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_transport_dev *transport_dev = NULL;

	pr_debug("%s\n", __func__);

	transport_dev = kzalloc(sizeof(struct xh2a_transport_dev), GFP_KERNEL);

	if (transport_dev == NULL) {
		pr_err("%s: alloc transport_dev failed\n", __func__);
		return -ENOMEM;
	}

	client = &transport_dev->client;

	transport_dev->private_data = handle;

	ret = xh2a_transport_protocol_init(&transport_dev->protocol,
					   transport_dev->private_data);

	if (ret != 0) {
		pr_err("%s: transport protocol init failed\n", __func__);
		goto err_protocol_init;
	}

	snprintf(client->name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_TRANSPORT_DEVICE_NAME);

	client->work[0].msi_id = XH2A_PCIE_MSI_ID_AOSS;
	client->work[0].msi_work = &transport_dev->protocol.transport_work;
	client->work_num = 1;

	client->client_data = transport_dev;
	client->private_data = handle;

	ret = xh2a_pcie_register_client(handle, client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_protocol_init;
	}

	return 0;

err_protocol_init:
	kfree(transport_dev);
	return ret;
}

/*
 * xh2a_transport_remove() - remove the transport device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_transport_remove(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_transport_dev *transport_dev;

	pr_debug("%s\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_TRANSPORT_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	transport_dev = client->client_data;

	xh2a_transport_protocol_deinit(&transport_dev->protocol);

	xh2a_pcie_unregister_client(handle, client);

	kfree(transport_dev);

	return 0;
}

/*
 * xh2a_transport_notifier_call() - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_transport_notifier_call(struct notifier_block *nb,
					unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_transport_probe(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_transport_remove(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_transport_notifier_block = {
	.notifier_call = xh2a_transport_notifier_call,
};

/*
 * xh2a_transport_register_driver() - register transport driver
 */
int __init xh2a_transport_register_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_transport_notifier_block);
	return 0;
}

/*
 * xh2a_transport_unregister_driver() - unregister transport driver
 */
void __exit xh2a_transport_unregister_driver(void)
{
	pr_debug("%s\n", __func__);
	xh2a_host_unregister_notifier_chain(&xh2a_transport_notifier_block);
}

MODULE_LICENSE("GPL");
