// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#ifndef _XH2A_IPU_CONFIG_H_
#define _XH2A_IPU_CONFIG_H_

#define XH2A_IPU_NAME_LEN	     64
#define XH2A_IPU_WQ_NAME_LEN	 64
#define XH2A_IPU_POLICY_NAME_LEN 64
#define XH2A_IPU_MSI_WORK_NUM	 24
#define XH2A_IPU_CORE_NUM	 2
#define XH2A_TILE_NUM_PER_CORE	 4
#define XH2A_IPU_TILE_QUEUE_NUM	 (XH2A_IPU_CORE_NUM * XH2A_TILE_NUM_PER_CORE)
#define ENTRIES_PER_QUEUE	 1024
#define XH2A_IPU_EVENT_NUM	 4096

#define XH2A_IPU_TILE_QUEUE_FLAG_INTERRUPT BIT(0)
#define XH2A_IPU_TILE_QUEUE_FLAG_BAD	   BIT(1)

#define IPU_BASE_ADDR		  0x41000000
#define BOOTER_VIEW_SPM_BASE_ADDR 0x2000000

/* temp config, remove after build tool ready */
#define XH2A_SPM_TOTAL_SIZE (1ULL * 1024 * 1024)
#define IPU_IOMAP(addr)	    (addr - 0x1000000000)

#endif /* _XH2A_IPU_CONFIG_H_ */