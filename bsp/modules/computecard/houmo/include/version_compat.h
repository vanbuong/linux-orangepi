// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2022-2025 Houmo AI Inc.
 * Author: shuangyou.wang<shuangyou.wang@houmo.ai>
 */

#ifndef __VERSION_COMPAT_H__
#define __VERSION_COMPAT_H__

#include <linux/version.h>
#include <linux/uaccess.h>

#define __VERSION_CMP(a, b, c, op) \
	(LINUX_VERSION_CODE op KERNEL_VERSION(a, b, c))
#define KERNEL_VERSION_EQ(a, b, c) __VERSION_CMP(a, b, c, ==)
#define KERNEL_VERSION_NE(a, b, c) __VERSION_CMP(a, b, c, !=)
#define KERNEL_VERSION_GT(a, b, c) __VERSION_CMP(a, b, c, >)
#define KERNEL_VERSION_GE(a, b, c) __VERSION_CMP(a, b, c, >=)
#define KERNEL_VERSION_LT(a, b, c) __VERSION_CMP(a, b, c, <)
#define KERNEL_VERSION_LE(a, b, c) __VERSION_CMP(a, b, c, <=)

#if KERNEL_VERSION_GE(5, 0, 0)
#define access_read_ok(addr, size)	access_ok(addr, size)
#define access_write_ok(addr, size)	access_ok(addr, size)
#else
#define access_read_ok(addr, size)	access_ok(VERIFY_READ, addr, size)
#define access_write_ok(addr, size)	access_ok(VERIFY_WRITE, addr, size)
#endif

#endif
