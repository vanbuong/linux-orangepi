// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */

#ifndef _XH2A_SAT_H_
#define _XH2A_SAT_H_

/* sat */
#define AOSS_EXT_SYS_BUS_ADDR_WIDTH (40)
#define AOSS_SAT_BASE_Q				(0x70006000U)
#define AOSS_SAT_REG_OFFSET_XSPI	(4)
#define AOSS_SAT_UPPER_ADDR_BITS	(11)
#define AOSS_SAT_LOWER_ADDR_BITS	(AOSS_EXT_SYS_BUS_ADDR_WIDTH - \
	AOSS_SAT_UPPER_ADDR_BITS)
#define AOSS_SAT_MAP_AREA_SIZE		(1UL << AOSS_SAT_LOWER_ADDR_BITS)
#define AOSS_SAT_MAP_AREA_MASK		((1UL << AOSS_SAT_LOWER_ADDR_BITS) - 1UL)

#define E21_EXT_SYS_BASE			(0x80000000UL)

#endif