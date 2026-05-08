// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#ifndef RPMSG_ENV_SPECIFIC_H_
#define RPMSG_ENV_SPECIFIC_H_

#include <linux/types.h>
#include "rpmsg_default_config.h"

/*
 * struct rpmsg_env_init - initial data for environment layer
 * @handle: WDFDEVICE Object, maybe removed in future
 * @platform_cfg: initial data pass to platform layer
 */
typedef struct rpmsg_env_init {
	void *handle;
	void *platform_cfg;
} rpmsg_env_init_t;

/*
 * struct rpmsg_queue_rx_cb_data_t - queue rx callback data
 * @src: source address
 * @data: data pointer
 * @len: data length
 */
typedef struct {
	uint32_t src;
	void *data;
	uint32_t len;
} rpmsg_queue_rx_cb_data_t;

#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
#error "This RPMsg-Lite port requires RL_USE_STATIC_API set to 0"
#endif

#endif /* RPMSG_ENV_SPECIFIC_H_ */
