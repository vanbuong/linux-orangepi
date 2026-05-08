// SPDX-License-Identifier: GPL-2.0
/*++

Copyright(c) 2023 Houmo AI Inc. All rights reserved.

Module Name:

	xh2a_i2c_internal.h

Abstract:

	This module contains the declarations shared by driver
	and HAL for I2C module internal used.

	Since this file is shared with the HAL layer,
	any modifications to its content must be very carefully
	synchronized with corresponding changes in the HAL layer.

Environment:

	user and kernel

--*/

#ifndef _XH2A_I2C_INTERNAL_H_
#define _XH2A_I2C_INTERNAL_H_

#define XFER_BUFFER_LEN     64
/*
 * struct xh2a_i2c_params - xh2a i2c ioctl
 * @i2c_bus: i2c bus number
 * @chip_address: slave adderss
 * @chip_address_len: slave address width [7bit/10bit]
 * @data_address: Memory address or register address
 * @data_address_len: Memory address or register address width
 * @buf_len: Data length
 * @buf: Data Buffer
 */
struct xh2a_i2c_params {
	int32_t i2c_bus;
	int32_t chip_address;
	int32_t chip_address_len; /* Not used yet. Reserved for easy expansion. */
	int32_t data_address;
	int32_t data_address_len;
	int32_t buf_len;
	uint8_t buf[XFER_BUFFER_LEN];
};

 /*
  * XH2A_I2C_DEVICE_NAME - unique name of xh2a i2c device
  */
#define XH2A_I2C_DEVICE_NAME     "xh2a_i2c_device"

#define XH2A_I2C_IOC_MAGIC 'D'

#define IOCTL_XH2A_I2C_WRITE_CMD \
    _IOW(XH2A_I2C_IOC_MAGIC, 0, struct xh2a_i2c_params)
#define IOCTL_XH2A_I2C_READ_CMD \
    _IOWR(XH2A_I2C_IOC_MAGIC, 1, struct xh2a_i2c_params)

#endif // ! _XH2A_I2C_INTERNAL_H_