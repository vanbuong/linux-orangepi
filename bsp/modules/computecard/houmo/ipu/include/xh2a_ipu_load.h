// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_LOAD_H_
#define _XH2A_IPU_LOAD_H_

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include "xh2a_ipu_config.h"

/* ipu filter param(0.5) */
#define IPU_UTILI_FILTER 10000
/* ipu sample period(in ms) */
#define IPU_SAMPLE_PERIOD 100

/**
 * struct xh2a_ipu_load_core - load state for a single IPU core.
 * @group_count: Number of groups currently enqueued/bound to this core.
 * @is_working: Whether the core has in-flight work since the last sample.
 * @start_ts: Timestamp (µs) when the current measurement window started.
 * @last_ts: Timestamp (µs) of the most recent accounting update.
 * @curr_busy_time: Busy time accumulated in the current window (µs).
 * @last_busy_time: Busy time recorded in the previous window (µs), used for
 *  smoothing and percentage calculations.
 */
struct xh2a_ipu_load_core {
	uint32_t group_count;
	bool is_working;

	uint64_t start_ts;
	uint64_t last_ts;
	uint64_t curr_busy_time;
	uint64_t last_busy_time;
};

/* load api */
void xh2a_ipu_load_on_enqueue(struct xh2a_ipu_device *ipu_dev, int core_id,
			      uint64_t start_time_us);
void xh2a_ipu_load_on_done(struct xh2a_ipu_device *ipu_dev, int core_id,
			   uint64_t end_time_us);
void xh2a_ipu_get_core_load(struct xh2a_ipu_device *ipu_dev, uint32_t core_id,
			    uint32_t *ipu_load);
void xh2a_ipu_load_init(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_load_exit(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_load_start(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_load_stop(struct xh2a_ipu_device *ipu_dev);
void xh2a_ipu_load_work(struct work_struct *work);

#endif /* _XH2A_IPU_LOAD_H_ */