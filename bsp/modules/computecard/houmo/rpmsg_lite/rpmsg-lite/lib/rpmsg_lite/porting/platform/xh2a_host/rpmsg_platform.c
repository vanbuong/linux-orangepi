// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

/* clang-format off */

#include "rpmsg_platform.h"
#include "rpmsg_env.h"

#include <linux/delay.h>

#if !defined(RL_USE_ENVIRONMENT_CONTEXT) || (RL_USE_ENVIRONMENT_CONTEXT == 0)
#error "This RPMsg-Lite port requires RL_USE_ENVIRONMENT_CONTEXT set to 1"
#endif

#if (!defined(RL_ENV_XH2A_HOST)) || (RL_ENV_XH2A_HOST != 1)
#error "This RPMsg-Lite port to xh2a host requires RL_ENV_XH2A_HOST set to 1"
#endif

/**
 * struct platform_context - platform context
 * @env: pointer to environment context
 * @handle: the parameter passed to functions in ops
 * @ops: functions need to be provided by driver
 */
typedef struct platform_context {
	void *env;
	void *handle;
	struct rpmsg_platform_ops *ops;
} platform_context_t;

void platform_isr(void *platform_context, uint32_t link_id, uint32_t queue_id)
{
	uint32_t vector_id;
	platform_context_t *ctx = platform_context;

	vector_id = RL_GET_VQ_ID(link_id, queue_id);
	env_isr(ctx->env, vector_id);
}

int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data)
{
	(void)vector_id;
	(void)isr_data;

	/* nothing to do */

	return 0;
}

int32_t platform_deinit_interrupt(uint32_t vector_id)
{
	(void)vector_id;

	/* nothing to do */

	return 0;
}

void platform_tx_callback(void *platform_context, uint32_t link_id)
{
	platform_context_t *ctx = platform_context;

	ctx->ops->link_up(ctx->handle, link_id);
}

/* FIXME: concurrency access? */
void rl_platform_notify(void *platform_context, uint32_t vector_id)
{
	uint32_t link_id, queue_id;
	platform_context_t *ctx = platform_context;

	link_id = RL_GET_LINK_ID(vector_id);
	queue_id = RL_GET_Q_ID(vector_id);

	ctx->ops->notify_device(ctx->handle, link_id, queue_id);
}

/**
 * platform_time_delay
 *
 * @param num_msec Delay time in ms.
 *
 * This is not an accurate delay, it ensures at least num_msec passed when
 * return.
 */
void platform_time_delay(uint32_t num_msec)
{
	/* FIXME */
	msleep(num_msec);
}

/**
 * platform_interrupt_enable
 *
 * Enable peripheral-related interrupt
 *
 * @param vector_id Virtual vector ID that needs to be converted to IRQ number
 *
 * @return vector_id Return value is never checked.
 *
 */
int32_t platform_interrupt_enable(uint32_t vector_id)
{
	/* nothing to do */
	return ((int32_t)vector_id);
}

/**
 * platform_interrupt_disable
 *
 * Disable peripheral-related interrupt.
 *
 * @param vector_id Virtual vector ID that needs to be converted to IRQ number
 *
 * @return vector_id Return value is never checked.
 *
 */
int32_t platform_interrupt_disable(uint32_t vector_id)
{
	/* nothing to do */
	return ((int32_t)vector_id);
}

/**
 * platform_map_mem_region
 *
 * Dummy implementation
 *
 */
void platform_map_mem_region(uint64_t vrt_addr, uint64_t phy_addr,
			     uint32_t size, uint32_t flags)
{
	(void)vrt_addr;
	(void)phy_addr;
	(void)size;
	(void)flags;
}

/**
 * platform_cache_all_flush_invalidate
 *
 * Dummy implementation
 *
 */
void platform_cache_all_flush_invalidate(void)
{
}

/**
 * platform_cache_disable
 *
 * Dummy implementation
 *
 */
void platform_cache_disable(void)
{
}

/**
 * platform_cache_flush
 *
 * Empty implementation
 *
 */
void platform_cache_flush(void *data, uint32_t len)
{
	(void)data;
	(void)len;
}

/**
 * platform_cache_invalidate
 *
 * Empty implementation
 *
 */
void platform_cache_invalidate(void *data, uint32_t len)
{
	(void)data;
	(void)len;
}

/**
 * platform_vatopa
 *
 * Dummy implementation
 *
 */
uintptr_t platform_vatopa(void *addr)
{
	return ((uintptr_t)(char *)addr);
}

/**
 * platform_patova
 *
 * Dummy implementation
 *
 */
void *platform_patova(uintptr_t addr)
{
	return ((void *)(char *)addr);
}

/**
 * platform_init
 *
 * platform/environment init
 */
int32_t platform_init(void **platform_context, void *env_context,
		      void *platform_init)
{
	platform_context_t *ctx;
	rpmsg_platform_init_data_t *init = platform_init;

	if (!init || !init->handle || !init->ops) {
		env_print("%s", "Invalid platform_init\n");
		return -1;
	}

	if (!init->ops->writew_remote ||
			!init->ops->readw_remote ||
			!init->ops->readq_remote ||
			!init->ops->memcpy_to_remote ||
			!init->ops->memcpy_from_remote ||
			!init->ops->bind_platform_context ||
			!init->ops->notify_device ||
			!init->ops->link_up) {
		env_print("%s", "Invalid ops for rpmsg platform\n");
		return -1;
	}

	ctx = env_allocate_memory(sizeof(platform_context_t));

	if (!ctx) {
		env_print("%s",
			  "Allocate memory for platform_context_t failed!\n");
		return -1;
	}

	ctx->env = env_context;
	ctx->handle = init->handle;
	ctx->ops = init->ops;

	ctx->ops->bind_platform_context(ctx->handle, ctx);

	*platform_context = ctx;

	return 0;
}

/**
 * platform_deinit
 *
 * platform/environment deinit process
 */
int32_t platform_deinit(void *platform_context)
{
	env_free_memory(platform_context);

	return 0;
}

void platform_w16_remote(void *platform_context, uint64_t addr, uint16_t data)
{
	platform_context_t *ctx = platform_context;

	ctx->ops->writew_remote(ctx->handle, addr, data);
}

uint64_t platform_r64_remote(void *platform_context, uint64_t addr)
{
	uint64_t data;
	platform_context_t *ctx = platform_context;

	data = ctx->ops->readq_remote(ctx->handle, addr);

	return data;
}

uint16_t platform_r16_remote(void *platform_context, uint64_t addr)
{
	uint16_t data;
	platform_context_t *ctx = platform_context;

	data = ctx->ops->readw_remote(ctx->handle, addr);

	return data;
}

void platform_memcpy_to_remote(void *platform_context, uint64_t dst, void *src,
			       uint32_t len)
{
	platform_context_t *ctx = platform_context;

	ctx->ops->memcpy_to_remote(ctx->handle, dst, src, len);
}

void platform_memcpy_from_remote(void *platform_context, void *dst,
				 uint64_t src, uint32_t len)
{
	platform_context_t *ctx = platform_context;

	ctx->ops->memcpy_from_remote(ctx->handle, dst, src, len);
}

void platform_memset_remote(void *platform_context, uint64_t addr,
			    int32_t value, uint32_t len)
{
	void *tmpbuf;

	tmpbuf = env_allocate_memory(len);
	if (tmpbuf == NULL) {
		env_print("%s", "Allocate memory for tmpbuf failed!\n");
		return;
	}

	env_memset(tmpbuf, value, len);

	platform_memcpy_to_remote(platform_context, addr, tmpbuf, len);

	env_free_memory(tmpbuf);
}

/* clang-format on */
