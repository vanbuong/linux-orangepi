// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_TRANSPORT_H_
#define _XH2A_TRANSPORT_H_

#include <xh2a_pcie_api.h>
#include "transport_protocol.h"

/*
 * XH2A_TRANSPORT_DEVICE_NAME - unique name of xh2a transport device
 */
#define XH2A_TRANSPORT_DEVICE_NAME	"xh2a_transport_device"
#define XH2A_TRANSPORT_DEVICE_NAME_LEN 64

/*
 * struct xh2a_transport_dev - xh2a transport device
 * @protocol: structure for transport protocol
 * @private_data: handle for pcie
 */
struct xh2a_transport_dev {
	struct xh2a_transport_protocol_handle protocol;
	void *private_data;
	struct xh2a_pcie_client client;
};

#endif
