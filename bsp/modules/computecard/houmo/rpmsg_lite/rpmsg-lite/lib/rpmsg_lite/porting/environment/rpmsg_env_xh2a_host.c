// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#include "rpmsg_compiler.h"
#include "rpmsg_env.h"
#include "rpmsg_platform.h"
#include "virtqueue.h"

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <asm/barrier.h>

#if !defined(RL_USE_ENVIRONMENT_CONTEXT) || (RL_USE_ENVIRONMENT_CONTEXT != 1)
#error "This RPMsg-Lite port requires RL_USE_ENVIRONMENT_CONTEXT set to 1"
#endif

#if defined(RL_USE_STATIC_API) && (RL_USE_STATIC_API == 1)
#error "This RPMsg-Lite port requires RL_USE_STATIC_APT set to 0"
#endif

#if defined(RL_CLEAR_USED_BUFFERS) && (RL_CLEAR_USED_BUFFERS == 1)
#error "This RPMsg-Lite port requires RL_CLEAR_USED_BUFFERS set to 0"
#endif

#if (!defined(RL_ENV_XH2A_HOST)) || (RL_ENV_XH2A_HOST != 1)
#error "This RPMsg-Lite port to xh2a host requires RL_ENV_XH2A_HOST set to 1"
#endif

/* Max supported ISR counts, only one link supported now, which means only two
 * interrupts supported (tvq & rvq)
 */
#define ISR_COUNT (2U)

/**
 * Structure to keep track of registered ISR's.
 * @data: data passed to virtqueue_notification()
 * @enabled: the isr is enabled or not
 */
struct isr_info {
	void *data;
	volatile uint32_t enabled;
};

/**
 * Env. context structure
 * @init_counter: count of initialize
 * @handle: specific pointer used to distinguish env_context
 * @platform_context: pointer to platform context
 * @isr_table: table with registered Virt. queue data
 */
typedef struct env_context {
	struct list_head node;
	int init_counter;
	void *handle;
	void *platform_context;
	struct isr_info isr_table[ISR_COUNT];
} env_context_t;

/* static DEFINE_MUTEX(env_list_lock); */
static struct list_head env_list = LIST_HEAD_INIT(env_list);

/*!
 * env_wait_for_link_up
 *
 * Wait until the link_state parameter of the rpmsg_lite_instance is set.
 *
 */
uint32_t env_wait_for_link_up(volatile uint32_t *link_state, uint32_t link_id,
			      uint32_t timeout_ms)
{
	(void)link_id;
	(void)timeout_ms;

	while (*link_state != 1U)
		;

	return 1U;
}

/**
 * Returns pointer to platform context.
 *
 * @param env_context Pointer to env. context
 *
 * @return Pointer to platform context
 */
void *env_get_platform_context(void *env_context)
{
	env_context_t *env = env_context;

	return env->platform_context;
}

/*!
 * env_tx_callback
 *
 * Set event to notify task waiting in env_wait_for_link_up().
 * Empty implementation for xh2a-host(Windows).
 *
 */
void env_tx_callback(void *env, uint32_t link_id)
{
	env_context_t *ctx = env;

	platform_tx_callback(ctx->platform_context, link_id);
}

/*!
 * env_init
 *
 * Initializes xh2a-host(Windows) environment.
 *
 */
int32_t env_init(void **env_context, void *env_init_data)
{
	int found = 0;
	env_context_t *ctx;
	rpmsg_env_init_t *init = env_init_data;

	if (init == NULL) {
		env_print("NULL pointer of env_init_data\n");
		return -1;
	}

	list_for_each_entry(ctx, &env_list, node) {
		if (ctx->handle == init->handle) {
			found = 1;
			break;
		}
	}

	if (!found) {
		ctx = env_allocate_memory(sizeof(env_context_t));
		if (ctx == NULL) {
			env_print("Allocate memory for env_context_t failed\n");
			return -1;
		}

		if (platform_init(&ctx->platform_context, ctx,
				  init->platform_cfg)) {
			env_print("Initialize platform failed\n");
			env_free_memory(ctx);
			return -1;
		}

		ctx->init_counter = 0;
		ctx->handle = init->handle;
		INIT_LIST_HEAD(&ctx->node);
		list_add_tail(&ctx->node, &env_list);
	}

	ctx->init_counter += 1;
	*env_context = ctx;

	return 0;
}

/*!
 * env_deinit
 *
 * Uninitializes xh2a-host(Windows) environment.
 *
 * @returns Execution status
 */
int32_t env_deinit(void *env_context)
{
	env_context_t *ctx;
	int found = 0;

	list_for_each_entry(ctx, &env_list, node) {
		if ((void *)ctx == env_context) {
			found = 1;
			break;
		}
	}

	if (!found) {
		env_print("Invalid env_context\n");
		return -1;
	}

	ctx->init_counter -= 1;
	if (ctx->init_counter)
		return 0;

	list_del(&ctx->node);
	platform_deinit(ctx->platform_context);
	env_free_memory(ctx);

	return 0;
}

/*!
 * env_allocate_memory - implementation
 *
 * @param size
 */
void *env_allocate_memory(uint32_t size)
{
	/* Always running in non-atomic context, use GFP_KERNEL flag */
	return kmalloc(size, GFP_KERNEL);
}

/*!
 * env_free_memory - implementation
 *
 * @param ptr
 */
void env_free_memory(void *ptr)
{
	kfree(ptr);
}

/*!
 *
 * env_memset - implementation
 *
 * @param ptr
 * @param value
 * @param size
 */
void env_memset(void *ptr, int32_t value, uint32_t size)
{
	(void)memset(ptr, value, size);
}

/*!
 *
 * env_memset_remote - implementation
 *
 * @param ptr - remote memory addr(physical)
 * @param value
 * @param size
 */
void env_memset_remote(void *env, uint64_t addr, int32_t value, uint32_t size)
{
	platform_memset_remote(env_get_platform_context(env), addr, value,
			       size);
}

/*!
 *
 * env_memcpy - implementation
 *
 * @param dst
 * @param src
 * @param len
 */
void env_memcpy(void *dst, void const *src, uint32_t len)
{
	(void)memcpy(dst, src, len);
}

/*!
 *
 * env_strcmp - implementation
 *
 * @param dst
 * @param src
 */

int32_t env_strcmp(const char *dst, const char *src)
{
	return strcmp(dst, src);
}

/*!
 *
 * env_strncpy - implementation
 *
 * @param dest
 * @param src
 * @param len
 */
void env_strncpy(char *dest, const char *src, uint32_t len)
{
	(void)strncpy(dest, src, len);
}

/*!
 *
 * env_strncmp - implementation
 *
 * @param dest
 * @param src
 * @param len
 */
int32_t env_strncmp(char *dest, const char *src, uint32_t len)
{
	return strncmp(dest, src, len);
}

/*!
 *
 * env_mb - implementation
 *
 */
void env_mb(void)
{
	mb();
}

/*!
 * env_rmb - implementation
 */
void env_rmb(void)
{
	rmb();
}

/*!
 * env_wmb - implementation
 */
void env_wmb(void)
{
	wmb();
}

/*!
 * env_map_vatopa - implementation
 *
 * @param address
 */
uint64_t env_map_vatopa(void *env, void *address)
{
	(void)env;

	return platform_vatopa(address);
}

/*!
 * env_map_patova - implementation
 *
 * @param address
 */
void *env_map_patova(void *env, uint64_t address)
{
	(void)env;

	return platform_patova(address);
}

/*!
 * env_create_mutex
 *
 * Creates a mutex with the given initial count.
 *
 */
int32_t env_create_mutex(void **lock, int32_t count)
{
	struct mutex *mutex;

	(void)count;

	mutex = env_allocate_memory(sizeof(struct mutex));
	if (!lock) {
		env_print("Allocate memory for mutex failed\n");
		return -1;
	}

	mutex_init(mutex);
	*lock = mutex;

	return 0;
}

/*!
 * env_delete_mutex
 *
 * Deletes the given lock
 *
 */
void env_delete_mutex(void *lock)
{
	env_free_memory(lock);
}

/*!
 * env_lock_mutex
 *
 * Tries to acquire the lock, if lock is not available then call to
 * this function will suspend.
 */
void env_lock_mutex(void *lock)
{
	struct mutex *mutex = lock;

	mutex_lock(mutex);
}

/*!
 * env_unlock_mutex
 *
 * Releases the given lock.
 */
void env_unlock_mutex(void *lock)
{
	struct mutex *mutex = lock;

	mutex_unlock(mutex);
}

/*!
 * env_sleep_msec
 *
 * Suspends the calling thread for given time , in msecs.
 */
void env_sleep_msec(uint32_t num_msec)
{
	platform_time_delay(num_msec);
}

/*!
 * env_register_isr
 *
 * Registers interrupt handler data for the given interrupt vector.
 *
 * @param vector_id - virtual interrupt vector number
 * @param data      - interrupt handler data (virtqueue)
 */
void env_register_isr(void *env, uint32_t vector_id, void *data)
{
	env_context_t *ctx = env;

	RL_ASSERT(vector_id < ISR_COUNT);
	if (vector_id < ISR_COUNT)
		ctx->isr_table[vector_id].data = data;
}

/*!
 * env_unregister_isr
 *
 * Unregisters interrupt handler data for the given interrupt vector.
 *
 * @param vector_id - virtual interrupt vector number
 */
void env_unregister_isr(void *env, uint32_t vector_id)
{
	env_context_t *ctx = env;

	RL_ASSERT(vector_id < ISR_COUNT);
	if (vector_id < ISR_COUNT) {
		ctx->isr_table[vector_id].data = NULL;
		ctx->isr_table[vector_id].enabled = 0;
	}
}

/*!
 * env_enable_interrupt
 *
 * Enables the given interrupt
 *
 * @param vector_id   - virtual interrupt vector number
 */

void env_enable_interrupt(void *env, uint32_t vector_id)
{
	env_context_t *ctx = env;

	RL_ASSERT(vector_id < ISR_COUNT);
	if (vector_id < ISR_COUNT)
		ctx->isr_table[vector_id].enabled = 1;
}

/*!
 * env_disable_interrupt
 *
 * Disables the given interrupt
 *
 * @param vector_id   - virtual interrupt vector number
 */

void env_disable_interrupt(void *env, uint32_t vector_id)
{
	env_context_t *ctx = env;

	RL_ASSERT(vector_id < ISR_COUNT);
	if (vector_id < ISR_COUNT)
		ctx->isr_table[vector_id].enabled = 0;
}

/*!
 * env_map_memory
 *
 * Enables memory mapping for given memory region.
 *
 * @param pa   - physical address of memory
 * @param va   - logical address of memory
 * @param size - memory size
 * param flags - flags for cache/uncached  and access type
 */

void env_map_memory(uint32_t pa, uint32_t va, uint32_t size, uint32_t flags)
{
	platform_map_mem_region(va, pa, size, flags);
}

/*!
 * env_disable_cache
 *
 * Disables system caches.
 *
 */

void env_disable_cache(void)
{
	platform_cache_all_flush_invalidate();
	platform_cache_disable();
}

void env_cache_flush(void *data, uint32_t len)
{
	(void)data;
	(void)len;
#if defined(RL_USE_DCACHE) && (RL_USE_DCACHE == 1)
	platform_cache_flush(data, len);
#endif
}

void env_cache_invalidate(void *data, uint32_t len)
{
	(void)data;
	(void)len;
#if defined(RL_USE_DCACHE) && (RL_USE_DCACHE == 1)
	platform_cache_invalidate(data, len);
#endif
}

/*========================================================= */
/* Util data / functions for xh2a-host(Windows) */

void env_isr(void *env, uint32_t vector)
{
	struct isr_info *info;
	env_context_t *ctx = env;

	RL_ASSERT(vector < ISR_COUNT);
	if (vector < ISR_COUNT) {
		info = &ctx->isr_table[vector];
		if (info->enabled)
			virtqueue_notification((struct virtqueue *)info->data);
	}
}

/**
 * Called by rpmsg to init an interrupt
 *
 * @param env      Pointer to env context.
 * @param vq_id    Virt. queue ID.
 * @param isr_data Pointer to interrupt data.
 *
 * @return         Execution status.
 */
int32_t env_init_interrupt(void *env, int32_t vq_id, void *isr_data)
{
	env_register_isr(env, vq_id, isr_data);

	return 0;
}

/**
 * Called by rpmsg to deinit an interrupt.
 *
 * @param env   Pointer to env context.
 * @param vq_id Virt. queue ID.
 *
 * @return      Execution status.
 */
int32_t env_deinit_interrupt(void *env, int32_t vq_id)
{
	env_unregister_isr(env, vq_id);

	return 0;
}

/*!
 * env_create_queue
 *
 * Creates a message queue.
 *
 * @param queue -  pointer to created queue
 * @param length -  maximum number of elements in the queue
 * @param element_size - queue element size in bytes
 * @param queue_static_storage - pointer to queue static storage buffer
 * @param queue_static_context - pointer to queue static context
 *
 * @return - status of function execution
 */

int32_t env_create_queue(void **queue, int32_t length, int32_t element_size)
{
	(void)queue;
	(void)length;
	(void)element_size;

	return 0;
}

/*!
 * env_delete_queue
 *
 * Deletes the message queue.
 *
 * @param queue - queue to delete
 */

void env_delete_queue(void *queue)
{
	(void)queue;
}

/*!
 * env_put_queue
 *
 * Put an element in a queue.
 *
 * @param queue - queue to put element in
 * @param msg - pointer to the message to be put into the queue
 * @param timeout_ms - timeout in ms
 *
 * @return - status of function execution
 */

int32_t env_put_queue(void *queue, void *msg, uintptr_t timeout_ms)
{
	(void)queue;
	(void)msg;
	(void)timeout_ms;

	return 0;
}

/*!
 * env_get_queue
 *
 * Get an element out of a queue.
 *
 * @param queue - queue to get element from
 * @param msg - pointer to a memory to save the message
 * @param timeout_ms - timeout in ms
 *
 * @return - status of function execution
 */

int32_t env_get_queue(void *queue, void *msg, uintptr_t timeout_ms)
{
	(void)queue;
	(void)msg;
	(void)timeout_ms;

	return 0;
}

/*!
 * env_get_current_queue_size
 *
 * Get current queue size.
 *
 * @param queue - queue pointer
 *
 * @return - Number of queued items in the queue
 */

int32_t env_get_current_queue_size(void *queue)
{
	(void)queue;

	return 0;
}
