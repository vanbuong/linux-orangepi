// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#ifndef _XH2A_PCIE_API_H_
#define _XH2A_PCIE_API_H_

#include <xh2a_pcie_msi.h>

typedef int (*xh2a_pm_prepare_callback)(void *arg, bool is_compatible);
typedef int (*xh2a_pm_complete_callback)(void *arg, bool is_compatible);
typedef int (*xh2a_pm_notifier_prepare_callback)(void *arg, bool rollback);
typedef int (*xh2a_pm_notifier_complete_callback)(void *arg);
typedef int (*xh2a_pm_runtime_suspend_callback)(void *arg);
typedef int (*xh2a_pm_runtime_resume_callback)(void *arg);

/*
 * XH2A_PCIE_CLIENT_NAME_LEN - the max length of client name
 */
#define XH2A_PCIE_CLIENT_NAME_LEN 64

/*
 * XH2A_PCIE_CLIENT_MAX_WORK_NUM - the max number of work for client
 */
#define XH2A_PCIE_CLIENT_MAX_WORK_NUM 32

/*
 * struct xh2a_pcie_client - the client of xh2a pcie driver
 * @node: the node of client list
 * @name: the name of client
 * @msi_work: the work struct of msi
 * @msi_id: the msi id of client
 * @work_num: register work numbers
 * @prepare_cb: callback for device enter hibernate
 * @complete_cb: callback for device exit hibernate
 * @notifier_prepare_cb: callback before prepare_cb
 * @notifier_complete_cb: callback after complete_cb
 * @private_data: the private data of client, pcie handle
 * @client_data: the client data of client, client device struct.
 */
struct xh2a_pcie_client {
	struct list_head node;

	char name[XH2A_PCIE_CLIENT_NAME_LEN];

	struct {
		struct work_struct *msi_work;
		int msi_id;
	} work[XH2A_PCIE_CLIENT_MAX_WORK_NUM];

	int work_num;

	xh2a_pm_prepare_callback prepare_cb;
	xh2a_pm_complete_callback complete_cb;
	xh2a_pm_notifier_prepare_callback notifier_prepare_cb;
	xh2a_pm_notifier_complete_callback notifier_complete_cb;
	xh2a_pm_runtime_suspend_callback runtime_suspend_cb;
	xh2a_pm_runtime_resume_callback runtime_resume_cb;

	void *private_data;
	void *client_data;
};

/*
 * xh2a_pcie_membar_getinfo - get membar's addr and size
 * @handle: the pcie handle
 * @bar_addr: the membar's addr
 * @bar_size: the membar's size
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_membar_getinfo(void *handle, uint64_t *bar_addr,
			     uint64_t *bar_size);
/*
 * xh2a_pcie_membar_map - set membar's mapping
 * @handle: the pcie handle
 * @paddr: the membar's mapping addr
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_membar_map(void *handle, uint64_t paddr);

/*
 * xh2a_pcie_membar_unmap - unset membar's mapping
 * @handle: the pcie handle
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_membar_unmap(void *handle);

/*
 * xh2a_pcie_membar_map_no_lock - set membar's mapping, no lock
 * @handle: the pcie handle
 * @paddr: the membar's mapping addr
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_membar_map_no_lock(void *handle, uint64_t paddr);

/*
 * xh2a_pcie_membar_unmap_no_lock - unset membar's mapping, no lock
 * @handle: the pcie handle
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_membar_unmap_no_lock(void *handle);

/*
 * xh2a_pcie_pio_readb() - read a byte from pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value read from pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_readb(void *handle, uint64_t paddr, uint8_t *value);

/*
 * xh2a_pcie_pio_readw() - read a word from pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value read from pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_readw(void *handle, uint64_t paddr, uint16_t *value);

/*
 * xh2a_pcie_pio_readl() - read a long from pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value read from pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_readl(void *handle, uint64_t paddr, uint32_t *value);

/*
 * xh2a_pcie_pio_readq() - read a quad-word from pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value read from pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_readq(void *handle, uint64_t paddr, uint64_t *value);

/*
 * xh2a_pcie_pio_writeb() - write a byte to pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value to write to pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_writeb(void *handle, uint64_t paddr, uint8_t value);

/*
 * xh2a_pcie_pio_writew() - write a word to pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value to write to pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_writew(void *handle, uint64_t paddr, uint16_t value);

/*
 * xh2a_pcie_pio_writel() - write a long to pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value to write to pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_writel(void *handle, uint64_t paddr, uint32_t value);

/*
 * xh2a_pcie_pio_writeq() - write a quad-word to pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @value: the value to write to pcie device
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_writeq(void *handle, uint64_t paddr, uint64_t value);

/*
 * xh2a_pcie_pio_read_mem() - read a memory region from pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @host_dst_addr: the host address to store the read data
 * @size: the size of the memory region to read
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_read_mem(void *handle, uint64_t paddr, void *host_dst_addr,
			   uint32_t size);

/*
 * xh2a_pcie_pio_write_mem() - write a memory region to pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @host_src_addr: the host address to read the data from
 * @size: the size of the memory region to write
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_write_mem(void *handle, uint64_t paddr, void *host_src_addr,
			    uint32_t size);

/*
 * xh2a_pcie_pio_write_mem_sync() - sync write a memory region to pcie device
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @host_src_addr: the host address to read the data from
 * @size: the size of the memory region to write
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_pio_write_mem_sync(void *handle, uint64_t paddr,
				 void *host_src_addr, uint32_t size);

/*
 * xh2a_pcie_dma_read_mem() - read a memory region from pcie device in dma mode
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @host_dst_addr: the host address to store the read data
 * @size: the size of the memory region to read
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_dma_read_mem(void *handle, uint64_t paddr, void *host_dst_addr,
			   uint32_t size);

/*
 * xh2a_pcie_dma_write_mem() - write a memory region to pcie device in dma mode
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @host_src_addr: the host address to read the data from
 * @size: the size of the memory region to write
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_dma_write_mem(void *handle, uint64_t paddr, void *host_src_addr,
			    uint32_t size);

/*
 * xh2a_pcie_dma_mrd_direct()
	- copy data from src to dst with MRd tlp, aka hdma's rd channel
 * @handle: the pcie handle
 * @src_paddr: host's bar address
 * @dst_paddr: xh2a address
 * @size: transfer size
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_dma_mrd_direct(void *handle, uint64_t src_paddr,
			     uint64_t dst_paddr, uint32_t size);

/*
 * xh2a_pcie_dma_mwr_direct()
	- copy data from src to dst with MWr tlp, aka hdma's wr channel
 * @handle: the pcie handle
 * @src_paddr: xh2a's address
 * @dst_paddr: host's bar address
 * @size: transfer size
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_dma_mwr_direct(void *handle, uint64_t src_paddr,
			     uint64_t dst_paddr, uint32_t size);

/*
 * xh2a_pcie_dma_read_mem_userspace()
 *     - read a memory region from pcie device in dma mode to user space
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @__user host_dst_addr: the user space host address to store the read data
 * @size: the size of the memory region to read
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_dma_read_mem_userspace(void *handle, uint64_t paddr,
				     void __user *host_dst_addr, uint32_t size);

/*
 * xh2a_pcie_dma_write_mem_userspace()
 *     - write a memory region to pcie device in dma mode from user space
 * @handle: the pcie handle
 * @paddr: the pcie device address (xh2a global address)
 * @__user host_src_addr: the user space host address to read the data from
 * @size: the size of the memory region to write
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_dma_write_mem_userspace(void *handle, uint64_t paddr,
				      void __user *host_src_addr,
				      uint32_t size);

/*
 * XH2A_PCIE_MSG_TO_U7 - message to cpu U7
 * XH2A_PCIE_MSG_TO_E2 - message to cpu E2
 */
#define XH2A_PCIE_MSG_TO_U7 1
#define XH2A_PCIE_MSG_TO_E2 2

/*
 * xh2a_pcie_write_bar_msgbit() - send a message bit to xh2a cpu
 * @handle: the pcie handle
 * @msgbit: the message bit to send, [0..15]
 * @tocpu: the cpu to send the message to (XH2A_PCIE_MSG_TOCPU_U7 or
 * XH2A_PCIE_MSG_TOCPU_E2)
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_write_bar_msgbit(void *handle, uint32_t msgbit, uint32_t tocpu);

/*
 * xh2a_pcie_get_pwrsts() - get the pcie power status
 * @handle: the pcie handle
 * @pwrsts: the pcie power status
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_check_pwrsts(void *handle, uint32_t *pwrsts);

/*
 * xh2a_pcie_update_pwrsts() - update the pcie power status
 * @handle: the pcie handle
 * @pwrsts: the pcie power status
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_update_pwrsts(void *handle, uint32_t pwrsts);

/*
 * xh2a_pcie_register_client() - register a client
 * @handle: the pcie handle
 * @client: the client to register
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_register_client(void *handle, struct xh2a_pcie_client *client);

/*
 * xh2a_pcie_unregister_client() - unregister a client
 * @handle: the pcie handle
 * @client: the client to unregister
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_unregister_client(void *handle, struct xh2a_pcie_client *client);

/*
 * xh2a_pcie_get_client() - get a client with name
 * @handle: the pcie handle
 * @client: the client to get
 * @name: the name of the client
 * Return: 0 on success, negative error code on failure
 */
int xh2a_pcie_get_client(void *handle, struct xh2a_pcie_client **client,
			 const char *name);

/*
 * xh2a_pcie_device_index() - get the device index
 * @handle: the pcie handle
 * Return: the device index
 */
int xh2a_pcie_device_index(void *handle);

/*
 * xh2a_pcie_device_dbdf() - get the device's domain:BDF
 * @handle: the pcie handle
 * @dbdf: device's domain:BDF. Domain << 16 | Bus << 8 | DEV << 3 | FUNC
 * Return: the device index
 */
int xh2a_pcie_device_dbdf(void *handle, uint32_t *dbdf);

/*
 * xh2a_pcie_device_ptr() - get the device pointer
 * @handle: the pcie handle
 * Return: the device pointer
 */
struct device *xh2a_pcie_device_ptr(void *handle);

/*
 * xh2a_pcie_stop_runtime_pm() - stop runtime pm
 * @handle: the pcie handle
 */
void xh2a_pcie_stop_runtime_pm(void *handle);

#endif
