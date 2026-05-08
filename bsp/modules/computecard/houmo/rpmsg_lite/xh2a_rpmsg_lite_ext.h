// SPDX-License-Identifer: GPL-2.0
/*
 * Copyright(c) 2025 Houmo AI Inc.
 * Author: liang.huang<liang.huang@houmo.ai>
 */

#ifndef _XH2A_RPMSG_LITE_EXT_H_
#define _XH2A_RPMSG_LITE_EXT_H_

#include "xh2a_rpmsg_lite.h"

struct xh2a_rpmsg_lite_ext {
	struct xh2a_rpmsg_ept *ept;
};

int xh2a_rpmsg_lite_ext_init(struct xh2a_rpmsg_lite_dev *rldev);

void xh2a_rpmsg_lite_ext_deinit(struct xh2a_rpmsg_lite_dev *rldev);

#endif /* _XH2A_RPMSG_LITE_EXT_H_ */
