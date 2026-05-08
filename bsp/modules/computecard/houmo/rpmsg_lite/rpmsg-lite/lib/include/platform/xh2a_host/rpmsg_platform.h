// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#ifndef RPMSG_PLATFORM_H_
#define RPMSG_PLATFORM_H_

#include <linux/types.h>
#include <linux/delay.h>

/**
 * struct rpmsg_platform_ops - operations for platform layer
 * @writew_remote: write a 16-bit value to a remote address
 * @readw_remote: read a 16-bit value from a remote address
 * @readq_remote: read a 64-bit value from a remote address
 * @memcpy_to_remote: copy a block of data from the local memory to the remote
 *		      memory
 * @memcpy_from_remote: copy a block of data from the remote memory to the local
 *			memory
 * @bind_platform_context: report platform context
 * @notify_device: notify the remote device (trigger irq)
 * @link_up: report link up
 */
struct rpmsg_platform_ops {
	void (*writew_remote)(void *handle, uint64_t addr, uint16_t data);
	uint16_t (*readw_remote)(void *handle, uint64_t addr);
	uint64_t (*readq_remote)(void *handle, uint64_t addr);
	void (*memcpy_to_remote)(void *handle, uint64_t dst, void *src,
				 uint32_t len);
	void (*memcpy_from_remote)(void *handle, void *dst, uint64_t src,
				   uint32_t len);
	void (*bind_platform_context)(void *handle, void *platform_context);
	void (*notify_device)(void *handle, uint32_t link_id,
			      uint32_t queue_id);
	void (*link_up)(void *handle, uint32_t link_id);
};

/*
 * struct rpmsg_platform_init_data - initial data for platform layer
 * @handle: WDFDEVICE Object
 * @ops: functions need to be provided by driver
 */
typedef struct rpmsg_platform_init_data {
	void *handle;
	struct rpmsg_platform_ops *ops;
} rpmsg_platform_init_data_t;

/*
 * Linux requires the ALIGN to 0x1000(4KB) instead of 0x80
 * Just follow in Windows
 */
#ifndef VRING_ALIGN
#define VRING_ALIGN (0x1000U)
#endif

/* contains pool of descriptors and two circular buffers */
#ifndef VRING_SIZE
#define VRING_SIZE (0x8000UL)
#endif

/* define shared memory space for VRINGS per one channel */
#define RL_VRING_OVERHEAD (2UL * VRING_SIZE)

#define RL_GET_VQ_ID(link_id, queue_id) \
	(((queue_id) & 0x1U) | (((link_id) << 1U) & 0xFFFFFFFEU))
#define RL_GET_LINK_ID(id) (((id) & 0xFFFFFFFEU) >> 1U)
#define RL_GET_Q_ID(id)	   ((id) & 0x1U)

#define RL_PLATFORM_HIGHEST_LINK_ID (0U)

/* platform interrupt related functions */
int32_t platform_init_interrupt(uint32_t vector_id, void *isr_data);
int32_t platform_deinit_interrupt(uint32_t vector_id);
int32_t platform_interrupt_enable(uint32_t vector_id);
int32_t platform_interrupt_disable(uint32_t vector_id);
void rl_platform_notify(void *platform_context, uint32_t vector_id);
void platform_isr(void *platform_context, uint32_t link_id, uint32_t queue_id);

/* platform low-level time-delay */
void platform_time_delay(uint32_t num_msec);

/* platform memory functions */
void platform_map_mem_region(uint64_t vrt_addr, uint64_t phy_addr,
			     uint32_t size, uint32_t flags);
void platform_cache_all_flush_invalidate(void);
void platform_cache_disable(void);
uintptr_t platform_vatopa(void *addr);
void *platform_patova(uintptr_t addr);

/* platform init/deinit */
int32_t platform_init(void **platform_context, void *env_context,
		      void *platform_init);
int32_t platform_deinit(void *platform_context);

/* for xh2a host remote read/write u7 via pcie */
void platform_w16_remote(void *platform_context, uint64_t addr, uint16_t data);
uint64_t platform_r64_remote(void *platform_context, uint64_t addr);
uint16_t platform_r16_remote(void *platform_context, uint64_t addr);
void platform_memset_remote(void *platform_context, uint64_t addr,
			    int32_t value, uint32_t len);
void platform_memcpy_to_remote(void *platform_context, uint64_t dst, void *src,
			       uint32_t len);
void platform_memcpy_from_remote(void *platform_context, void *dst,
				 uint64_t src, uint32_t len);

/* platform tx related functions */
void platform_tx_callback(void *platform_context, uint32_t link_id);
#endif /* RPMSG_PLATFORM_H_ */
