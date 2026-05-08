// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: baoping.fan<baoping.fan@houmo.ai>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <linux/kref.h>
#include <linux/pm_runtime.h>
#include <xh2a_host_drv.h>
#include <xh2a_pcie_api.h>

#include "xh2a_i2c.h"
#include "xh2a_i2c_dev.h"

static int xh2a_i2c_writel(struct xh2a_i2c_dev *i2c_dev, uint32_t reg,
			   uint32_t val)
{
	uint64_t addr = i2c_dev->xh2a_i2c_base + reg;

	return xh2a_pcie_pio_writel(i2c_dev->private_data, addr, val);
}

static int xh2a_i2c_readl(struct xh2a_i2c_dev *i2c_dev, uint32_t reg,
			  uint32_t *val)
{
	uint64_t addr = i2c_dev->xh2a_i2c_base + reg;

	return xh2a_pcie_pio_readl(i2c_dev->private_data, addr, val);
}

/*
 * xh2a_i2c_wait_for_bus_idle - Wait for the I2C bus to become idle
 * @i2c_dev: Pointer to the I2C device structure
 *
 * This function polls the I2C controller's status register to ensure the
 * bus and the transmit FIFO are idle before proceeding. If the conditions
 * are not met within the specified timeout, it returns a timeout error.
 *
 * Returns:
 *   0 - Success, the bus is idle
 *   -ETIMEDOUT - Timeout error, the bus did not become idle
 */
static int xh2a_i2c_wait_for_bus_idle(struct xh2a_i2c_dev *i2c_dev)
{
	int ret = 0;
	uint32_t status = 0;
	unsigned long timeout = jiffies + msecs_to_jiffies(POLL_TIMEOUT_MS);

	while (time_before(jiffies, timeout)) {
		ret = xh2a_i2c_readl(i2c_dev, DW_IC_STATUS, &status);

		if (ret != 0) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: pcie read failed\n", __func__);
			return ret;
		}

		if ((status & IC_STATUS_MA) || !(status & IC_STATUS_TFE))
			usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
		else
			return 0;
	}

	dev_err(i2c_dev->miscdev.this_device,
		"%s: Timeout waiting for TX FIFO empty\n", __func__);

	return -ETIMEDOUT;
}

/**
 * xh2a_i2c_wait_for_stop - Wait for the I2C STOP condition to occur.
 * @i2c_dev: Pointer to the I2C device structure.
 *
 * This function polls the I2C status register to detect the occurrence of the
 * STOP condition. If the STOP condition is detected, the corresponding
 * interrupt is cleared, and the function returns successfully. If the timeout
 * period expires before the STOP condition occurs, the function logs an error
 * and returns a timeout error code.
 *
 * Returns:
 * 0 on success, -ETIMEDOUT if the STOP condition is not detected within the
 * timeout period.
 */
static int xh2a_i2c_wait_for_stop(struct xh2a_i2c_dev *i2c_dev)
{
	int ret = 0;
	uint32_t status = 0x0;
	uint32_t clr_stop = 0x0;
	unsigned long timeout = jiffies + msecs_to_jiffies(POLL_TIMEOUT_MS);

	while (time_before(jiffies, timeout)) {
		ret = xh2a_i2c_readl(i2c_dev, DW_IC_RAW_INTR_STAT, &status);

		if (ret != 0) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: pcie read failed\n", __func__);
			return ret;
		}

		if (status & IC_STOP_DET) {
			xh2a_i2c_readl(i2c_dev, DW_IC_CLR_STOP_DET, &clr_stop);
			return 0;
		}

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	}

	dev_err(i2c_dev->miscdev.this_device,
		"%s: Timeout waiting for STOP condition\n", __func__);

	return -ETIMEDOUT;
}

static int xh2a_i2c_flush_rxfifo(struct xh2a_i2c_dev *i2c_dev)
{
	int ret;
	uint32_t dummy;
	uint32_t status;
	unsigned long timeout = jiffies + msecs_to_jiffies(POLL_TIMEOUT_MS);

	while (1) {
		ret = xh2a_i2c_readl(i2c_dev, DW_IC_STATUS, &status);
		if (ret) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: Failed to read IC_STATUS\n", __func__);
			return ret;
		}

		if (!(status & IC_STATUS_RFNE))
			break;

		ret = xh2a_i2c_readl(i2c_dev, DW_IC_DATA_CMD, &dummy);
		if (ret) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: Failed to read IC_DATA_CMD\n", __func__);
			return ret;
		}

		if (time_after(jiffies, timeout)) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: Timeout while flushing RX FIFO\n",
				__func__);
			return -ETIMEDOUT;
		}
	}

	return 0;
}

/*
 * xh2a_i2c_xfer_finish - Wait for the I2C transfer to finish and
 * clear STOP condition
 * @i2c_dev: Pointer to the I2C device structure
 *
 * This function waits for the I2C transfer to complete by checking
 * for the STOP condition detection. Once detected, it clears
 * the STOP condition and then waits
 * for the I2C bus to become idle. If the STOP condition is not
 * detected within
 * the specified timeout period, it returns a timeout error.
 *
 * Returns:
 *   0 - Success, the transfer finished and bus is idle
 *   -ETIMEDOUT - Timeout error, the STOP condition was not detected
 *   Other error codes from xh2a_i2c_wait_for_bus_idle - if waiting for the
 *   bus to be idle fails
 */
static int xh2a_i2c_xfer_finish(struct xh2a_i2c_dev *i2c_dev)
{
	int ret = 0;

	ret = xh2a_i2c_wait_for_stop(i2c_dev);

	if (ret) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: Failed to wait for STOP condition\n", __func__);
		return ret;
	}

	ret = xh2a_i2c_wait_for_bus_idle(i2c_dev);

	if (ret) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: Failed to wait for bus to become idle\n",
			__func__);
		return ret;
	}

	xh2a_i2c_flush_rxfifo(i2c_dev);

	return 0;
}

static int xh2a_i2c_set_ic_enable(struct xh2a_i2c_dev *i2c_dev, uint32_t ena)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(POLL_TIMEOUT_MS);
	uint32_t v_enable = 0x0;

	xh2a_i2c_writel(i2c_dev, DW_IC_ENABLE, ena);

	while (time_before(jiffies, timeout)) {
		xh2a_i2c_readl(i2c_dev, DW_IC_ENABLE, &v_enable);

		if (v_enable == ena)
			return 0;

		usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
	}

	dev_err(i2c_dev->miscdev.this_device,
		"%s: Timeout while setting I2C enable state\n", __func__);

	return -ETIMEDOUT;
}

/**
 * xh2a_i2c_set_target - Set the target I2C slave address.
 * @i2c_dev: Pointer to the I2C device structure.
 * @address: Target I2C slave address to be set.
 *
 * This function configures the I2C controller to communicate with a specific
 * I2C slave device by setting the target address in the I2C Target Address
 * Register (DW_IC_TAR). The function first disables the I2C controller to
 * allow configuration of the target address, writes the new address to the
 * register, and then re-enables the I2C controller.
 *
 * Returns:
 * 0 on success, or a negative error code if disabling or enabling the I2C
 * controller fails.
 */
static int xh2a_i2c_set_target(struct xh2a_i2c_dev *i2c_dev, uint8_t address)
{
	int ret = 0;

	ret = xh2a_i2c_set_ic_enable(i2c_dev, IC_DISABLE);

	if (ret < 0) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: disable i2c failed\n", __func__);
		return ret;
	}

	xh2a_i2c_writel(i2c_dev, DW_IC_TAR, address);

	ret = xh2a_i2c_set_ic_enable(i2c_dev, IC_ENABLE);

	if (ret < 0) {
		dev_err(i2c_dev->miscdev.this_device, "%s: enable i2c failed\n",
			__func__);
		return ret;
	}

	return 0;
}

/**
 * xh2a_dw_i2c_read - Perform an I2C read operation
 * @i2c_dev: Pointer to the I2C device structure
 * @buf: Buffer to store the received data
 * @len: Number of bytes to read
 *
 * This function reads data from the I2C bus and stores it in the
 * provided buffer.
 * It reads data from the RX FIFO and writes read commands to the TX FIFO.
 * It checks for any transmission errors, including TX aborts, and handles
 * timeouts if the operation takes too long.
 *
 * Returns:
 *   0 - Success, data read successfully
 *   -ETIMEDOUT - Timeout error during the read process
 *   -EIO - I2C transmission aborted due to an error
 */
static int xh2a_dw_i2c_read(struct xh2a_i2c_dev *i2c_dev, uint8_t *buffer,
			    int len)
{
	int ret = 0;
	uint32_t active = 0;
	uint32_t status;
	uint32_t value = 0;
	unsigned long timeout = jiffies + msecs_to_jiffies(POLL_TIMEOUT_MS);

	while (len) {
		if (time_after(jiffies, timeout)) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: Timeout while processing I2C read\n",
				__func__);
			return -ETIMEDOUT;
		}

		if (!active) {
			if (len == 1)
				xh2a_i2c_writel(i2c_dev, DW_IC_DATA_CMD,
						IC_CMD | IC_STOP);
			else
				xh2a_i2c_writel(i2c_dev, DW_IC_DATA_CMD,
						IC_CMD);

			active = 1;
		}

		ret = xh2a_i2c_readl(i2c_dev, DW_IC_STATUS, &status);
		if (ret != 0) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: pcie read failed\n", __func__);
			return ret;
		}

		if (status & IC_STATUS_RFNE) {
			xh2a_i2c_readl(i2c_dev, DW_IC_DATA_CMD, &value);
			*buffer++ = (uint8_t)value;
			len--;
			active = 0;
		} else {
			usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
		}
	}

	return xh2a_i2c_xfer_finish(i2c_dev);
}

/**
 * xh2a_dw_i2c_write - Perform an I2C write operation
 * @i2c_dev: Pointer to the I2C device structure
 * @buf: Buffer containing the data to write
 * @len: Number of bytes to write
 *
 * This function writes data to the I2C bus. It writes data to the TX FIFO,
 * checking the available space, and handles the TX abort condition.
 * It also checks for a timeout during the write operation and
 * ensures that the write command is correctly finished.
 *
 * Returns:
 *   0 - Success, data written successfully
 *   -ETIMEDOUT - Timeout error during the write process
 *   -EIO - I2C transmission aborted due to an error
 */
static int xh2a_dw_i2c_write(struct xh2a_i2c_dev *i2c_dev, uint8_t *buffer,
			     int len)
{
	int ret = 0;
	uint32_t status = 0x0;
	unsigned long timeout = jiffies + msecs_to_jiffies(POLL_TIMEOUT_MS);

	while (len) {
		if (time_after(jiffies, timeout)) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: Timeout while writing data\n", __func__);
			return -ETIMEDOUT;
		}

		ret = xh2a_i2c_readl(i2c_dev, DW_IC_STATUS, &status);
		if (ret != 0) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: pcie read failed\n", __func__);
			return ret;
		}

		if (status & IC_STATUS_TFNF) {
			if (--len == 0) {
				ret = xh2a_i2c_writel(i2c_dev, DW_IC_DATA_CMD,
						      *buffer | IC_STOP);
			} else {
				ret = xh2a_i2c_writel(i2c_dev, DW_IC_DATA_CMD,
						      *buffer);
			}

			if (ret != 0) {
				dev_err(i2c_dev->miscdev.this_device,
					"%s: Failed to write DATA_CMD, "
					"ret=%d\n",
					__func__, ret);
				return ret;
			}

			buffer++;

		} else {
			usleep_range(POLL_DELAY_MIN, POLL_DELAY_MAX);
		}
	}

	ret = xh2a_i2c_xfer_finish(i2c_dev);

	if (ret < 0)
		return ret;

	return 0;
}

/**
 * xh2a_i2c_read_xfer - Perform an I2C read transfer with address and data
 * buffers
 * @i2c_dev: Pointer to the I2C device structure
 * @addr_buf: Buffer containing the address to read from
 * @alen: Length of the address buffer
 * @data_buf: Buffer to store the read data
 * @data_len: Number of bytes to read from the I2C device
 *
 * This function performs an I2C read operation by first sending the address
 * and then reading the specified number of bytes into the data buffer.
 * It manages the transmission of the address and data through the TX FIFO,
 * checking for space and ensuring no transmission errors.
 * The function also handles timeout and read errors.
 *
 * Returns:
 *   0 - Success, data read successfully
 *   -ETIMEDOUT - Timeout error during the transfer process
 *   -EIO - I2C transmission aborted due to an error
 */
static int xh2a_i2c_read_xfer(struct xh2a_i2c_dev *i2c_dev, uint32_t addr,
			      uint32_t alen, uint8_t *data_buf,
			      uint32_t data_len)
{
	while (alen) {
		alen--;
		/* high byte address going out first */
		xh2a_i2c_writel(i2c_dev, DW_IC_DATA_CMD,
				(addr >> (alen * 8)) & 0xff);
	}

	return xh2a_dw_i2c_read(i2c_dev, data_buf, data_len);
}

static int xh2a_i2c_write_xfer(struct xh2a_i2c_dev *i2c_dev, uint32_t addr,
			       uint32_t alen, uint8_t *data_buf,
			       uint32_t data_len)
{
	while (alen) {
		alen--;
		/* high byte address going out first */
		xh2a_i2c_writel(i2c_dev, DW_IC_DATA_CMD,
				(addr >> (alen * 8)) & 0xff);
	}

	return xh2a_dw_i2c_write(i2c_dev, data_buf, data_len);
}

/**
 * xh2a_i2c_xfer - Perform I2C transfer (read or write) for
 * the specified device.
 * @i2c_dev: Pointer to the I2C device structure.
 * @i2c_ioctl: Pointer to the I2C ioctl structure containing the transfer data.
 * @is_read: Boolean flag indicating whether this is a read (true) or
 * write (false) operation.
 *
 * This function performs an I2C transfer for either reading or
 * writing data to a device.
 * It prepares the transfer buffer, sets the target device address,
 * and then either reads data from or writes data to the device.
 * The function handles both address and data buffers.
 *
 * Returns:
 *   0 - Success, transfer completed successfully
 *   -EINVAL - Invalid argument, the transfer buffer is too large
 *   -ETIMEDOUT - Timeout error during the transfer process (if any)
 *   -EIO - I2C transmission aborted due to an error (if any)
 */
static int xh2a_i2c_xfer(struct xh2a_i2c_dev *i2c_dev,
			 struct xh2a_i2c_params *i2c_ioctl, bool is_read)
{
	int ret = 0;
	uint8_t xfer_buf[XFER_BUFFER_LEN] = { 0 };
	uint32_t alen = i2c_ioctl->data_address_len;
	uint32_t addr = i2c_ioctl->data_address;
	uint8_t *buf = i2c_ioctl->buf;
	uint32_t buf_len = i2c_ioctl->buf_len;

	if (alen + buf_len > sizeof(xfer_buf)) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: Transfer buffer too large (alen=%u, buf_len=%u)\n",
			__func__, alen, buf_len);
		return -EINVAL;
	}

	ret = xh2a_i2c_wait_for_bus_idle(i2c_dev);
	if (ret < 0) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: I2C bus busy (chip_address=0x%X), error: %d\n",
			__func__, i2c_ioctl->chip_address, ret);
		return ret;
	}

	ret = xh2a_i2c_set_target(i2c_dev, i2c_ioctl->chip_address);

	if (ret < 0) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: Failed to set target address 0x%X, error: %d\n",
			__func__, i2c_ioctl->chip_address, ret);
		return ret;
	}

	if (is_read) {
		ret = xh2a_i2c_read_xfer(i2c_dev, addr, alen, buf, buf_len);
	} else {
		ret = xh2a_i2c_write_xfer(i2c_dev, addr, alen, buf, buf_len);
	}

	return ret;
}

static void xh2a_i2c_safe_release(struct kref *kref)
{
	struct xh2a_i2c_dev *i2c_dev =
		container_of(kref, struct xh2a_i2c_dev, refcount);
	kfree(i2c_dev);
}

static int xh2a_i2c_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct xh2a_i2c_dev *i2c_dev = NULL;
	int major = MAJOR(inode->i_rdev);
	int minor = MINOR(inode->i_rdev);

	i2c_dev = container_of(miscdev, struct xh2a_i2c_dev, miscdev);

	if (!i2c_dev) {
		dev_err(miscdev->this_device, "%s: invalid device\n", __func__);
		return -ENODEV;
	}

	if (atomic_read(&i2c_dev->removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (kref_get_unless_zero(&i2c_dev->refcount) == 0) {
		pr_err("%s: device is removing...\n", __func__);
		return -ENODEV;
	}

	mutex_lock(&i2c_dev->i2c_mutex);
	if (atomic_read(&i2c_dev->opened)) {
		mutex_unlock(&i2c_dev->i2c_mutex);
		dev_err(miscdev->this_device, "%s: device is already opened\n",
			__func__);
		kref_put(&i2c_dev->refcount, xh2a_i2c_safe_release);
		return -EBUSY;
	}

	atomic_set(&i2c_dev->opened, 1);
	mutex_unlock(&i2c_dev->i2c_mutex);

	dev_dbg(miscdev->this_device, "%s: major=%d, minor=%d\n", __func__,
		major, minor);
	dev_dbg(miscdev->this_device, "%s: client: %s (%d)\n", __func__,
		current->comm, current->pid);

	return 0;
}

static int xh2a_i2c_release(struct inode *inode, struct file *filp)
{
	struct miscdevice *miscdev = filp->private_data;
	struct xh2a_i2c_dev *i2c_dev = NULL;
	int major = MAJOR(inode->i_rdev);
	int minor = MINOR(inode->i_rdev);

	filp->private_data = NULL;

	if (!miscdev) {
		pr_err("%s: miscdev is NULL\n", __func__);
		return -EINVAL;
	}

	i2c_dev = container_of(miscdev, struct xh2a_i2c_dev, miscdev);

	if (!i2c_dev) {
		dev_err(miscdev->this_device, "%s: invalid device\n", __func__);
		return -ENODEV;
	}

	dev_dbg(miscdev->this_device, "%s: major=%d, minor=%d\n", __func__,
		major, minor);

	mutex_lock(&i2c_dev->i2c_mutex);
	atomic_set(&i2c_dev->opened, 0);
	mutex_unlock(&i2c_dev->i2c_mutex);

	kref_put(&i2c_dev->refcount, xh2a_i2c_safe_release);

	return 0;
}

static int xh2a_i2c_handle_xfer(struct xh2a_i2c_dev *i2c_dev,
				struct xh2a_i2c_params __user *arg,
				bool is_read)
{
	int ret = 0;
	struct xh2a_i2c_params ioc = { 0 };
	const uint32_t i2c_base_addresses[] = { I2C0_BASE_ADDR, I2C1_BASE_ADDR,
						I2C2_BASE_ADDR };

	if (copy_from_user(&ioc, arg, sizeof(ioc))) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: copy_from_user failed\n", __func__);
		return -EFAULT;
	}

	if (ioc.i2c_bus < ARRAY_SIZE(i2c_base_addresses)) {
		i2c_dev->xh2a_i2c_base = i2c_base_addresses[ioc.i2c_bus];
	} else {
		dev_err(i2c_dev->miscdev.this_device, "%s: i2c bus error\n",
			__func__);
		return -EINVAL;
	}

	ret = xh2a_i2c_xfer(i2c_dev, &ioc, is_read);

	if (ret < 0) {
		dev_err(i2c_dev->miscdev.this_device,
			"%s: i2c xfer error: %d\n", __func__, ret);
		return ret;
	}

	if (is_read) {
		if (copy_to_user(arg, &ioc, sizeof(ioc))) {
			dev_err(i2c_dev->miscdev.this_device,
				"%s: copy_to_user failed\n", __func__);
			return -EFAULT;
		}
	}

	return ret;
}

static long xh2a_i2c_params(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	int retval = 0;
	struct miscdevice *miscdev;
	struct xh2a_i2c_dev *i2c_dev = NULL;
	uint32_t tmp;
	bool is_read;

	miscdev = filp->private_data;
	i2c_dev = container_of(miscdev, struct xh2a_i2c_dev, miscdev);

	if (!i2c_dev) {
		dev_err(miscdev->this_device, "%s: invalid device\n", __func__);
		return -ENODEV;
	}

	if (atomic_read(&i2c_dev->removed)) {
		pr_err("%s: device mark removed\n", __func__);
		return -ENODEV;
	}

	if (_IOC_TYPE(cmd) != XH2A_I2C_IOC_MAGIC) {
		dev_err(miscdev->this_device,
			"%s: Invalid IOCTL type, expected 0x%X, got 0x%X\n",
			__func__, XH2A_I2C_IOC_MAGIC, _IOC_TYPE(cmd));
		return -ENOTTY;
	}

	tmp = _IOC_SIZE(cmd);

	if (tmp != sizeof(struct xh2a_i2c_params)) {
		dev_err(miscdev->this_device,
			"%s: Invalid IOCTL size, expected %zu, got %u\n",
			__func__, sizeof(struct xh2a_i2c_params), tmp);
		return -EINVAL;
	}

	if (atomic_read(&i2c_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: device try enter sleep, wait...\n", __func__);

		retval = wait_event_interruptible(
			i2c_dev->block_ioctl_wq,
			atomic_read(&i2c_dev->block_ioctl_flag) == 0);

		if (retval < 0) {
			dev_err(miscdev->this_device,
				"%s: wait_event_interruptible failed %d\n",
				__func__, retval);
			return -EINTR;
		}
	}

	pm_runtime_get_sync(miscdev->parent);

	mutex_lock(&i2c_dev->i2c_mutex);

	switch (cmd) {
	case IOCTL_XH2A_I2C_WRITE_CMD:
	case IOCTL_XH2A_I2C_READ_CMD:
		is_read = (cmd == IOCTL_XH2A_I2C_READ_CMD);

		retval = xh2a_i2c_handle_xfer(
			i2c_dev, (struct xh2a_i2c_params __user *)arg, is_read);

		break;

	default:
		retval = -EINVAL;
		break;
	}

	mutex_unlock(&i2c_dev->i2c_mutex);

	if ((retval != 0) && atomic_read(&i2c_dev->block_ioctl_flag)) {
		dev_err(miscdev->this_device,
			"%s: ioctl failed in sleep process.\n", __func__);
		retval = -EINTR;
	}

	pm_runtime_mark_last_busy(miscdev->parent);
	pm_runtime_put_sync(miscdev->parent);

	return retval;
}

static const struct file_operations xh2a_i2c_fops = {
	.owner = THIS_MODULE,
	.open = xh2a_i2c_open,
	.release = xh2a_i2c_release,
	.unlocked_ioctl = xh2a_i2c_params,
};

/*
 * xh2a_i2c_pm_notifier_prepare()
 *     - before pm prepare the i2c device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_i2c_pm_notifier_prepare(void *handle, bool rollback)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_i2c_dev *i2c_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_I2C_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	i2c_dev = client->client_data;

	if (rollback) {
		if (atomic_dec_and_test(&i2c_dev->block_ioctl_flag))
			wake_up_interruptible(&i2c_dev->block_ioctl_wq);
	} else {
		atomic_inc(&i2c_dev->block_ioctl_flag);
	}

	return 0;
}

/*
 * xh2a_i2c_pm_notifier_complete()
 *     - after pm complete the i2c device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_i2c_pm_notifier_complete(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_i2c_dev *i2c_dev;

	xh2a_pcie_get_client(handle, &client, XH2A_I2C_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	i2c_dev = client->client_data;

	(void)i2c_dev;

	return 0;
}

/*
 * xh2a_i2c_pm_prepare()
 *     - pm prepare the i2c device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_i2c_pm_prepare(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_i2c_dev *i2c_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_I2C_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	i2c_dev = client->client_data;

	(void)i2c_dev;
	return 0;
}

/*
 * xh2a_i2c_pm_complete()
 *     - pm complete the i2c device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_i2c_pm_complete(void *handle, bool is_compatible)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_i2c_dev *i2c_dev;

	(void)is_compatible;
	xh2a_pcie_get_client(handle, &client, XH2A_I2C_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: client not found\n", __func__);
		return 0;
	}

	i2c_dev = client->client_data;

	if (atomic_dec_and_test(&i2c_dev->block_ioctl_flag))
		wake_up_interruptible(&i2c_dev->block_ioctl_wq);

	return 0;
}

/*
 * xh2a_i2c_init() - probe function for i2c device
 * @handle: handle of xh2a_pcie_dev
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_i2c_init(void *handle)
{
	int ret;
	struct xh2a_i2c_dev *i2c_dev = NULL;

	pr_debug("%s xh2a i2c driver init\n", __func__);

	i2c_dev = kzalloc(sizeof(struct xh2a_i2c_dev), GFP_KERNEL);

	if (i2c_dev == NULL) {
		pr_err("%s: alloc i2c_dev failed\n", __func__);
		return -ENOMEM;
	}

	i2c_dev->private_data = handle;
	i2c_dev->xh2a_i2c_base = I2C0_BASE_ADDR;

	atomic_set(&i2c_dev->opened, 0);
	atomic_set(&i2c_dev->removed, 0);
	kref_init(&i2c_dev->refcount);

	atomic_set(&i2c_dev->block_ioctl_flag, 0);
	init_waitqueue_head(&i2c_dev->block_ioctl_wq);

	mutex_init(&i2c_dev->i2c_mutex);

	memset(i2c_dev->name_buf, 0, XH2A_I2C_DEVICE_NAME_LEN);
	snprintf(i2c_dev->name_buf, XH2A_I2C_DEVICE_NAME_LEN,
		 XH2A_I2C_DEVICE_NAME "%d", xh2a_pcie_device_index(handle));

	i2c_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	i2c_dev->miscdev.name = i2c_dev->name_buf;
	i2c_dev->miscdev.fops = &xh2a_i2c_fops;
	i2c_dev->miscdev.mode = 0666;
	i2c_dev->miscdev.parent = xh2a_pcie_device_ptr(handle);

	ret = misc_register(&i2c_dev->miscdev);

	if (ret) {
		pr_err("%s: misc_register failed\n", __func__);
		goto err_misc_register;
	}

	snprintf(i2c_dev->client.name, XH2A_PCIE_CLIENT_NAME_LEN,
		 XH2A_I2C_DEVICE_NAME);

	i2c_dev->client.work_num = 0;

	i2c_dev->client.client_data = i2c_dev;
	i2c_dev->client.private_data = handle;
	i2c_dev->client.prepare_cb = xh2a_i2c_pm_prepare;
	i2c_dev->client.complete_cb = xh2a_i2c_pm_complete;
	i2c_dev->client.notifier_prepare_cb = xh2a_i2c_pm_notifier_prepare;
	i2c_dev->client.notifier_complete_cb = xh2a_i2c_pm_notifier_complete;

	ret = xh2a_pcie_register_client(handle, &i2c_dev->client);

	if (ret != 0) {
		pr_err("%s: register client failed %d\n", __func__, ret);
		goto err_client_register;
	}

	return 0;

err_client_register:
	misc_deregister(&i2c_dev->miscdev);
err_misc_register:
	kfree(i2c_dev);

	return ret;
}

/*
 * xh2a_i2c_deinit() - remove the i2c device
 * @handle: pcie handle
 * Return: 0 on success, negative error code on failure
 */
static int xh2a_i2c_deinit(void *handle)
{
	struct xh2a_pcie_client *client = NULL;
	struct xh2a_i2c_dev *i2c_dev = NULL;

	pr_debug("%s xh2a i2c driver deinit\n", __func__);

	xh2a_pcie_get_client(handle, &client, XH2A_I2C_DEVICE_NAME);

	if (client == NULL) {
		pr_debug("%s: Failed to get client for name '%s'\n", __func__,
			 XH2A_I2C_DEVICE_NAME);
		return 0;
	}

	i2c_dev = client->client_data;

	atomic_set(&i2c_dev->removed, 1);

	misc_deregister(&i2c_dev->miscdev);

	xh2a_pcie_unregister_client(handle, client);

	i2c_dev->private_data = NULL;
	kref_put(&i2c_dev->refcount, xh2a_i2c_safe_release);

	return 0;
}

/*
 * xh2a_i2c_notifier_call() - called when the pcie notifies this driver
 * @nb: notifier block
 * @event: event type
 * @data: data from notifier
 * Return:
 *  = NOTIFY_OK - Success
 */
static int xh2a_i2c_notifier_call(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	int ret = 0;

	pr_debug("%s: event = %ld, data = %p\n", __func__, event, data);

	if (event == XH2A_HOST_NOTIFY_PCIE_PROBE)
		ret = xh2a_i2c_init(data);
	else if (event == XH2A_HOST_NOTIFY_PCIE_REMOVE)
		ret = xh2a_i2c_deinit(data);

	if (ret)
		return NOTIFY_STOP;

	return NOTIFY_OK;
}

/*
 * @notifier_call: called when the pcie notifies this driver
 */
static struct notifier_block xh2a_i2c_notifier_block = {
	.notifier_call = xh2a_i2c_notifier_call,
};

/*
 * xh2a_i2c_register_driver() - register dummy driver
 */
int __init xh2a_i2c_register_driver(void)
{
	pr_debug("%s: Register XH2A I2C driver\n", __func__);
	xh2a_host_register_notifier_chain(&xh2a_i2c_notifier_block);
	return 0;
}

/*
 * xh2a_i2c_unregister_driver() - unregister dummy driver
 */
void __exit xh2a_i2c_unregister_driver(void)
{
	pr_debug("%s: Unregister XH2A I2C driver\n", __func__);
	xh2a_host_unregister_notifier_chain(&xh2a_i2c_notifier_block);
}

MODULE_LICENSE("GPL");
