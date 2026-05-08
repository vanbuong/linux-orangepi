// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2024 Houmo AI Inc.
 * Author: hongxing.ma<hongxing.ma@houmo.ai>
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/idr.h>
#include <linux/pm_runtime.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <xh2a_host_drv.h>
#include <xh2a_rpmsg_lite_api.h>
#include <xh2a_pcie_api.h>
#include "xh2a_pcie.h"

#define XH2A_MAX_DEVICES (1U << MINORBITS)
static DEFINE_IDR(xh2a_minor_idr);
static DEFINE_MUTEX(xh2a_minor_idr_mutex);

static pci_ers_result_t xh2a_pcie_err_error_detected(struct pci_dev *pdev,
						     pci_channel_state_t state)
{
	(void)state;
	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t xh2a_pcie_err_slot_reset(struct pci_dev *pdev)
{
	return PCI_ERS_RESULT_NEED_RESET;
}

static void xh2a_pcie_err_resume(struct pci_dev *pdev)
{
}

static int xh2a_pcie_pm_notifier(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	int ret = 0;
	struct xh2a_pcie_client *pos, *n;
	struct xh2a_pcie_dev *p_xh2a =
		container_of(nb, struct xh2a_pcie_dev, pm_notifier);
	struct device *dev = &(p_xh2a->pdev->dev);

	switch (action) {
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		dev_info(dev, "before suspend/hibernation prepare\n");

		pm_runtime_get_sync(dev);

		list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
			if (pos->notifier_prepare_cb)
				ret |= pos->notifier_prepare_cb(p_xh2a, false);
		}

		/* if notifier return non-zero value, rollback */
		if (ret != 0) {
			list_for_each_entry_safe(pos, n, &p_xh2a->client_list,
						 node) {
				if (pos->notifier_prepare_cb)
					pos->notifier_prepare_cb(p_xh2a, true);
			}

			pm_runtime_mark_last_busy(dev);
			pm_runtime_put_sync(dev);
		}
		break;
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
		dev_info(dev, "after suspend/hibernation complete\n");
		list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
			if (pos->notifier_complete_cb)
				ret |= pos->notifier_complete_cb(p_xh2a);
		}

		pm_runtime_mark_last_busy(dev);
		pm_runtime_put_sync(dev);

		break;
	case PM_POST_RESTORE:
	case PM_RESTORE_PREPARE:
	default:
		dev_info(dev, "ignore pm action %ld\n", action);
		break;
	}
	if (ret)
		return NOTIFY_BAD;
	return NOTIFY_DONE;
}

static int xh2a_pcie_pm_prepare(struct device *dev)
{
	struct xh2a_pcie_client *pos, *n;
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(to_pci_dev(dev));
	unsigned long timeout;
	int ret;
	uint32_t pwrsts;
	bool is_compatible;

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return -ENODEV;
	}

	p_xh2a->pdev->d3cold_allowed = p_xh2a->d3cold_allowed_saved;

	if (pm_suspend_target_state == PM_SUSPEND_TO_IDLE)
		xh2a_rpmsg_lite_ext_send(p_xh2a, XH2A_LPCTRL_START_SLEEP_ID,
					 XH2A_LPCTRL_START_SLEEP_STR,
					 XH2A_LPCTRL_START_SLEEP_LEN);
	else
		xh2a_rpmsg_lite_ext_send(p_xh2a, XH2A_LPCTRL_START_HIBERNATE_ID,
					 XH2A_LPCTRL_START_HIBERNATE_STR,
					 XH2A_LPCTRL_START_HIBERNATE_LEN);

	dev_info(dev,
		 "%s: step1, sent rpmsg to notify device start hibernate, "
		 "jiffies %ld, HZ %d\n",
		 __func__, jiffies, HZ);

	/* wait max 5s for u7 linux to freeze */
	/* is_compatible is used for non-compatible firmware */
	is_compatible = false;
	timeout = jiffies + msecs_to_jiffies(5000);
	while (time_before(jiffies, timeout)) {
		ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

		if (ret != 0) {
			dev_err(dev, "%s: pcie read failed\n", __func__);
			return ret;
		}

		if ((pwrsts == XH2A_LPSTS_U7_ENTER_LP) ||
		    (pwrsts == XH2A_LPSTS_E2_ENTER_LP)) {
			is_compatible = true;
			break;
		}

		usleep_range(10, 100);
	}

	dev_info(dev, "%s: step2, polled pwrsts get %d, jiffies %ld\n",
		 __func__, pwrsts, jiffies);

	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (pos->prepare_cb)
			pos->prepare_cb(p_xh2a, is_compatible);
	}

	dev_info(dev, "%s: step3, each submodule prepared, jiffies %ld\n",
		 __func__, jiffies);

	/* wait max 1s for saving device image */
	timeout = jiffies + msecs_to_jiffies(1000);
	while (time_before(jiffies, timeout)) {
		ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

		if (ret != 0) {
			dev_err(dev, "%s: pcie read failed\n", __func__);
			return ret;
		}

		if (pwrsts == XH2A_LPSTS_E2_ENTER_LP)
			break;

		usleep_range(10, 100);
	}

	dev_info(dev, "%s: step4, polled pwrsts get %d, jiffies %ld, HZ %d\n",
		 __func__, pwrsts, jiffies, HZ);

	return 0;
}

static void xh2a_pcie_pm_complete(struct device *dev)
{
	struct xh2a_pcie_client *pos, *n;
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(to_pci_dev(dev));
	unsigned long timeout;
	int ret;
	uint32_t pwrsts;
	bool is_compatible;

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return;
	}

	xh2a_pcie_probe_post(p_xh2a);

	xh2a_pcie_write_bar_msgbit(p_xh2a, XH2A_LPCTRL_EXIT_IDLE_OR_L1_CODE,
				   XH2A_PCIE_MSG_TO_E2);

	dev_info(dev, "%s: step1, pcie_probe_post, jiffies %ld, HZ %d\n",
		 __func__, jiffies, HZ);

	/* wait max 5s for device ready */
	timeout = jiffies + msecs_to_jiffies(5000);
	while (time_before(jiffies, timeout)) {
		ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

		if (ret != 0) {
			dev_err(dev, "%s: pcie read failed\n", __func__);
			break;
		}

		if ((pwrsts == XH2A_LPSTS_E2_WAIT_HOST) ||
		    (pwrsts == XH2A_LPSTS_U7_EXIT_LP))
			break;

		usleep_range(10, 100);
	}

	dev_info(dev, "%s: step2, polled pwrsts get %d, jiffies %ld\n",
		 __func__, pwrsts, jiffies);

	/* mark that device can start image restoring */
	if (pwrsts == XH2A_LPSTS_E2_WAIT_HOST)
		xh2a_pcie_update_pwrsts(p_xh2a, XH2A_LPSTS_HOST_READY);

	dev_info(dev, "%s: step3, update pwrsts, jiffies %ld\n", __func__,
		 jiffies);

	/* wait max 5s for image restoring */
	is_compatible = false;
	timeout = jiffies + msecs_to_jiffies(5000);
	while (time_before(jiffies, timeout)) {
		ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

		if (ret != 0) {
			dev_err(dev, "%s: pcie read failed\n", __func__);
			break;
		}

		if (pwrsts == XH2A_LPSTS_U7_EXIT_LP) {
			is_compatible = true;
			break;
		}

		usleep_range(10, 100);
	}

	dev_info(dev, "%s: step4, polled pwrsts get %d, jiffies %ld\n",
		 __func__, pwrsts, jiffies);

	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (pos->complete_cb)
			pos->complete_cb(p_xh2a, is_compatible);
	}

	dev_info(dev, "%s: step5, each submodule complete. jiffies %ld\n",
		 __func__, jiffies);

	p_xh2a->d3cold_allowed_saved = p_xh2a->pdev->d3cold_allowed;
	p_xh2a->pdev->d3cold_allowed = 0;
}

static int xh2a_pcie_pm_suspend(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_resume(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_freeze(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_thaw(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_poweroff(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_restore(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_suspend_noirq(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_resume_noirq(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_freeze_noirq(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_thaw_noirq(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_poweroff_noirq(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_restore_noirq(struct device *dev)
{
	return 0;
}

static int xh2a_pcie_pm_runtime_suspend(struct device *dev)
{
	int ret;
	uint32_t pwrsts;
	unsigned long timeout;
	struct xh2a_pcie_client *pos, *n;
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(to_pci_dev(dev));

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return -ENODEV;
	}

	dev_info(&p_xh2a->pdev->dev, "%s: runtime suspend\n", __func__);

	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (pos->runtime_suspend_cb)
			pos->runtime_suspend_cb(p_xh2a);
	}

	xh2a_rpmsg_lite_ext_send(p_xh2a, XH2A_LPCTRL_START_SLEEP_ID,
				 XH2A_LPCTRL_START_SLEEP_STR,
				 XH2A_LPCTRL_START_SLEEP_LEN);

	/* wait max 1000ms for xh2a enter lp mode */
	timeout = jiffies + msecs_to_jiffies(1000);
	while (time_before(jiffies, timeout)) {
		ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

		if (ret != 0) {
			dev_err(dev, "%s: pcie read failed\n", __func__);
			return ret;
		}

		if (pwrsts == XH2A_LPSTS_E2_ENTER_LP)
			break;

		usleep_range(10, 100);
	}

	return 0;
}

static int xh2a_pcie_pm_runtime_resume(struct device *dev)
{
	int ret;
	uint32_t pwrsts;
	unsigned long timeout;
	struct xh2a_pcie_client *pos, *n;
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(to_pci_dev(dev));

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return -ENODEV;
	}

	dev_info(&p_xh2a->pdev->dev, "%s: runtime resume\n", __func__);

	xh2a_pcie_probe_post(p_xh2a);

	ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

	if (ret != 0) {
		dev_err(dev, "%s: pcie read failed\n", __func__);
		return -ERESTARTSYS;
	}

	if (pwrsts == XH2A_LPSTS_E2_ENTER_LP)
		xh2a_pcie_write_bar_msgbit(p_xh2a,
					   XH2A_LPCTRL_EXIT_IDLE_OR_L1_CODE,
					   XH2A_PCIE_MSG_TO_E2);

	/* wait max 2000ms for xh2a resume from idle or sleep */
	dev_info(&p_xh2a->pdev->dev,
		 "%s: wait for xh2a runtime resume. jiffies %ld, HZ %d\n",
		 __func__, jiffies, HZ);
	timeout = jiffies + msecs_to_jiffies(2000);
	while (time_before(jiffies, timeout)) {
		ret = xh2a_pcie_check_pwrsts(p_xh2a, &pwrsts);

		if (ret != 0) {
			dev_err(dev, "%s: pcie read failed\n", __func__);
			break;
		}

		if (pwrsts == XH2A_LPSTS_U7_EXIT_LP) {
			break;
		}

		usleep_range(10, 100);
	}
	dev_info(&p_xh2a->pdev->dev,
		 "%s: xh2a runtime resumed. jiffies %ld, HZ %d\n", __func__,
		 jiffies, HZ);

	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		if (pos->runtime_resume_cb)
			pos->runtime_resume_cb(p_xh2a);
	}

	return 0;
}

static int xh2a_pcie_pm_runtime_idle(struct device *dev)
{
	return 0;
}

static irqreturn_t xh2a_pcie_msi_handler_hdma(int irq, void *arg)
{
	struct xh2a_pcie_dev *p_xh2a = (struct xh2a_pcie_dev *)arg;

	schedule_work(&p_xh2a->hdma.hdma_work);
	return IRQ_HANDLED;
}

static irqreturn_t xh2a_pcie_msi_handler(int irq, void *arg)
{
	struct xh2a_pcie_client *pos, *n;
	struct xh2a_pcie_dev *p_xh2a = (struct xh2a_pcie_dev *)arg;
	int i, msi_id = -1;

	for (i = 0; i < p_xh2a->irqs; i++) {
		if (irq == p_xh2a->msi_irq[i]) {
			msi_id = i;
			break;
		}
	}

	dev_dbg(&p_xh2a->pdev->dev, "%s: got IRQ%d MSI vector%d", __func__, irq,
		msi_id);

	list_for_each_entry_safe(pos, n, &p_xh2a->client_list, node) {
		for (i = 0; i < pos->work_num; i++)
			if (pos->work[i].msi_id == msi_id)
				schedule_work(pos->work[i].msi_work);
	}

	return IRQ_HANDLED;
}

static void msi_free(struct xh2a_pcie_dev *p_xh2a, int nirqs)
{
	struct pci_dev *pdev = p_xh2a->pdev;
	struct device *dev = &pdev->dev;
	int i, minirqs;

	minirqs = (nirqs <= p_xh2a->irqs) ? nirqs : p_xh2a->irqs;

	for (i = 0; i < minirqs; i++)
		devm_free_irq(dev, (uint32_t)p_xh2a->msi_irq[i],
			      (void *)p_xh2a);

	pci_free_irq_vectors(pdev);
}

static int parse_interrupt_resource(struct xh2a_pcie_dev *p_xh2a)
{
	struct pci_dev *pdev = p_xh2a->pdev;
	struct device *dev = &pdev->dev;
	int ret, i;

	p_xh2a->irqs = pci_alloc_irq_vectors(pdev, XH2A_PCIE_MSI_NVEC,
					     XH2A_PCIE_MSI_NVEC, PCI_IRQ_MSI);

	if (p_xh2a->irqs < 0) {
		dev_err(dev, "pci_alloc_irq_vectors failed %d\n", p_xh2a->irqs);
		return -1;
	}

	for (i = 0; i < p_xh2a->irqs; i++) {
		p_xh2a->msi_irq[i] = pci_irq_vector(pdev, (uint32_t)i);

		if (p_xh2a->msi_irq[i] < 0) {
			dev_err(dev, "pci_irq_vector failed: %d, %d\n", i,
				p_xh2a->msi_irq[i]);
			pci_free_irq_vectors(pdev);
			return -1;
		}
	}

	for (i = 0; i < p_xh2a->irqs - 1; i++) {
		ret = devm_request_irq(dev, (uint32_t)p_xh2a->msi_irq[i],
				       xh2a_pcie_msi_handler, 0x0, "xh2a_pcie",
				       (void *)p_xh2a);

		if (ret != 0) {
			dev_err(dev, "devm_request_irq failed: %d, %d\n",
				p_xh2a->msi_irq[i], ret);
			msi_free(p_xh2a, i);
			return -1;
		}
	}

	ret = devm_request_irq(dev,
			       (uint32_t)p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA],
			       xh2a_pcie_msi_handler_hdma, 0x0, "xh2a_pcie",
			       (void *)p_xh2a);

	if (ret != 0) {
		dev_err(dev, "devm_request_irq failed: %d, %d\n",
			p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA], ret);
		msi_free(p_xh2a, p_xh2a->irqs - 1);
		return -1;
	}

	if (irq_get_msi_desc(p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA])) {
		get_cached_msi_msg(p_xh2a->msi_irq[XH2A_PCIE_MSI_ID_HDMA],
				   &p_xh2a->hdma_msi);
		p_xh2a->hdma_msi.data |= XH2A_PCIE_MSI_ID_HDMA;
	}

	return 0;
}

static void mem_unmap(struct xh2a_pcie_dev *p_xh2a)
{
	struct pci_dev *pdev = p_xh2a->pdev;

	pcim_iounmap(pdev, p_xh2a->bar0_mem);
	pcim_iounmap(pdev, p_xh2a->bar2_mem);
	pcim_iounmap(pdev, p_xh2a->trgt0_mem);
}

static int parse_mem_resource(struct xh2a_pcie_dev *p_xh2a)
{
	struct pci_dev *pdev = p_xh2a->pdev;
	struct device *dev = &pdev->dev;

	p_xh2a->bar0_base = pci_resource_start(pdev, XH2A_PCIE_BAR0);
	p_xh2a->bar0_len = pci_resource_len(pdev, XH2A_PCIE_BAR0);
	p_xh2a->bar0_mem = pcim_iomap(pdev, XH2A_PCIE_BAR0, p_xh2a->bar0_len);

	if (!p_xh2a->bar0_mem) {
		dev_err(dev, "map BAR%d failed\n", XH2A_PCIE_BAR0);
		return -1;
	}

	p_xh2a->bar2_base = pci_resource_start(pdev, XH2A_PCIE_BAR2);
	p_xh2a->bar2_len = pci_resource_len(pdev, XH2A_PCIE_BAR2);
	p_xh2a->bar2_mem = pcim_iomap(pdev, XH2A_PCIE_BAR2, p_xh2a->bar2_len);

	if (!p_xh2a->bar2_mem) {
		dev_err(dev, "map BAR%d failed\n", XH2A_PCIE_BAR2);
		return -1;
	}

	p_xh2a->trgt0_base = pci_resource_start(pdev, XH2A_PCIE_TRGT0);
	p_xh2a->trgt0_len = pci_resource_len(pdev, XH2A_PCIE_TRGT0);
	p_xh2a->trgt0_mem =
		pcim_iomap(pdev, XH2A_PCIE_TRGT0, p_xh2a->trgt0_len);

	if (!p_xh2a->trgt0_mem) {
		dev_err(dev, "map BAR%d failed\n", XH2A_PCIE_TRGT0);
		return -1;
	}

	p_xh2a->membar_lockmap = 0;

	return 0;
}

static inline int xh2a_pcie_get_minor(struct xh2a_pcie_dev *p_xh2a)
{
	int ret;

	mutex_lock(&xh2a_minor_idr_mutex);
	ret = idr_alloc(&xh2a_minor_idr, p_xh2a, 0, XH2A_MAX_DEVICES,
			GFP_KERNEL);

	if (ret >= 0) {
		p_xh2a->minor = ret;
		ret = 0;
	} else if (ret == -ENOSPC) {
		dev_err(&p_xh2a->pdev->dev, "too many xh2a devices\n");
		ret = -EINVAL;
	}

	mutex_unlock(&xh2a_minor_idr_mutex);
	return ret;
}

static inline void xh2a_pcie_put_minor(struct xh2a_pcie_dev *p_xh2a)
{
	mutex_lock(&xh2a_minor_idr_mutex);
	idr_remove(&xh2a_minor_idr, p_xh2a->minor);
	mutex_unlock(&xh2a_minor_idr_mutex);
}

static int xh2a_pcie_probe(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct xh2a_pcie_dev *p_xh2a = NULL;
	struct device *dev = &pdev->dev;
	int ret, i;

	p_xh2a = devm_kzalloc(&pdev->dev, sizeof(struct xh2a_pcie_dev),
			      GFP_KERNEL);

	if (!p_xh2a) {
		dev_err(dev, "devm_kzalloc xha2 device failed\n");
		return -ENOMEM;
	}

	pci_set_drvdata(pdev, (void *)p_xh2a);

	ret = xh2a_pcie_get_minor(p_xh2a);

	if (ret) {
		dev_err(dev, "xh2a_pcie_get_minor failed %d\n", ret);
		goto get_minor_err;
	}

	ret = pcim_enable_device(pdev);

	if (ret) {
		dev_err(dev, "pcim_enable_device failed %d\n", ret);
		goto enable_device_err;
	}

	ret = pci_request_regions(pdev, "xh2a_pci_region");

	if (ret) {
		dev_err(dev, "pci_request_regions failed %d\n", ret);
		goto request_regions_err;
	}

	pci_set_master(pdev);
	p_xh2a->pdev = pdev;
	ret = parse_mem_resource(p_xh2a);

	if (ret) {
		dev_err(dev, "parse_mem_resource failed %d\n", ret);
		goto mem_parse_err;
	}

	ret = parse_interrupt_resource(p_xh2a);

	if (ret) {
		dev_err(dev, "parse_interrupt_resource failed %d\n", ret);
		goto interrupt_parse_err;
	}

	ret = hdma_init(&p_xh2a->hdma, p_xh2a->trgt0_mem, &p_xh2a->hdma_msi,
			&p_xh2a->pdev->dev);

	if (ret) {
		dev_err(dev, "hdma_init failed %d\n", ret);
		goto hdma_init_err;
	}

	INIT_LIST_HEAD(&p_xh2a->client_list);
	mutex_init(&p_xh2a->client_list_mutex);

	for (i = 0; i < XH2A_PCIE_ATU_NCHAN; i++)
		spin_lock_init(&p_xh2a->iatu_lock[i]);

	xh2a_pcie_probe_post(p_xh2a);

	dev_info(dev, "%s pcie init done. notify each sub-module\n", __func__);

	ret = xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_PROBE,
					    p_xh2a);

	if (ret == NOTIFY_STOP) {
		dev_err(dev, "%s notify stop\n", __func__);
		xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_REMOVE,
					      p_xh2a);
		goto notifier_err;
	}

	p_xh2a->pm_notifier.notifier_call = xh2a_pcie_pm_notifier;
	register_pm_notifier(&p_xh2a->pm_notifier);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 5000);

	/* force disable d3cold */
	p_xh2a->d3cold_allowed_saved = pdev->d3cold_allowed;
	pdev->d3cold_allowed = 0;
#ifdef HM_RUNTIME_PM_ENABLE
	pm_runtime_allow(dev);
#else
	pm_runtime_forbid(dev);
#endif
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_sync(dev);

	return 0;

notifier_err:
	hdma_deinit(&p_xh2a->hdma);
hdma_init_err:
	msi_free(p_xh2a, XH2A_PCIE_MSI_NVEC);
interrupt_parse_err:
	mem_unmap(p_xh2a);
mem_parse_err:
	pci_clear_master(pdev);
	pci_release_regions(pdev);
request_regions_err:
	pci_disable_device(pdev);
enable_device_err:
	xh2a_pcie_put_minor(p_xh2a);
get_minor_err:
	pci_set_drvdata(pdev, NULL);
	devm_kfree(dev, p_xh2a);
	return ret;
}

static void xh2a_pcie_remove(struct pci_dev *pdev)
{
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return;
	}

	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_get_sync(dev);

	pdev->d3cold_allowed = p_xh2a->d3cold_allowed_saved;

	unregister_pm_notifier(&p_xh2a->pm_notifier);

	dev_dbg(dev, "%s notify\n", __func__);
	xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_REMOVE, p_xh2a);

	hdma_deinit(&p_xh2a->hdma);
	msi_free(p_xh2a, XH2A_PCIE_MSI_NVEC);
	mem_unmap(p_xh2a);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	xh2a_pcie_put_minor(p_xh2a);

	pci_set_drvdata(pdev, NULL);
	devm_kfree(dev, p_xh2a);
	dev_info(dev, "%s module remove done.\n", __func__);
}

static void xh2a_pcie_shutdown(struct pci_dev *pdev)
{
	struct xh2a_pcie_dev *p_xh2a = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	if (!p_xh2a) {
		dev_err(dev, "%s cannot find xh2a device\n", __func__);
		return;
	}

	/* runtime resume is called by device_shutdown */

	dev_dbg(dev, "%s notify\n", __func__);
	xh2a_host_call_notifier_chain(XH2A_HOST_NOTIFY_PCIE_SHUTDOWN, p_xh2a);
}

static const struct pci_error_handlers xh2a_pcie_err_handler = {
	.error_detected = xh2a_pcie_err_error_detected,
	.slot_reset = xh2a_pcie_err_slot_reset,
	.resume = xh2a_pcie_err_resume,
};

static const struct dev_pm_ops xh2a_pcie_pm_ops = {
	.prepare = xh2a_pcie_pm_prepare,
	.complete = xh2a_pcie_pm_complete,
	.suspend = xh2a_pcie_pm_suspend,
	.resume = xh2a_pcie_pm_resume,
	.freeze = xh2a_pcie_pm_freeze,
	.thaw = xh2a_pcie_pm_thaw,
	.poweroff = xh2a_pcie_pm_poweroff,
	.restore = xh2a_pcie_pm_restore,
	.suspend_noirq = xh2a_pcie_pm_suspend_noirq,
	.resume_noirq = xh2a_pcie_pm_resume_noirq,
	.freeze_noirq = xh2a_pcie_pm_freeze_noirq,
	.thaw_noirq = xh2a_pcie_pm_thaw_noirq,
	.poweroff_noirq = xh2a_pcie_pm_poweroff_noirq,
	.restore_noirq = xh2a_pcie_pm_restore_noirq,
	.runtime_suspend = xh2a_pcie_pm_runtime_suspend,
	.runtime_resume = xh2a_pcie_pm_runtime_resume,
	.runtime_idle = xh2a_pcie_pm_runtime_idle,
};

static ssize_t driver_version_show(struct device_driver *drv, char *buf)
{
	return sprintf(buf, "xh2a driver version %s: %s\n",
		       XH2A_HOST_DRIVER_RELEASE, XH2A_HOST_DRIVER_BUILDTIME);
}
static DRIVER_ATTR_RO(driver_version);

static struct attribute *xh2a_pcie_drv_attrs[] = {
	&driver_attr_driver_version.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xh2a_pcie_drv);

static const struct pci_device_id xh2a_pcie_pci_tbl[] = {
	{ PCI_DEVICE(XH2A_PCIE_EP_VID, XH2A_PCIE_EP_DID) },
	{ PCI_DEVICE(XH2A_PCIE_EP_ALT_VID, XH2A_PCIE_EP_ALT_DID) },
	{ 0 },
};
MODULE_DEVICE_TABLE(pci, xh2a_pcie_pci_tbl);

static struct pci_driver xh2a_pcie_driver = {
	.name = "houmo,xh2a",
	.probe = xh2a_pcie_probe,
	.remove = xh2a_pcie_remove,
	.id_table = xh2a_pcie_pci_tbl,
	.driver = {
		.pm = &xh2a_pcie_pm_ops,
	},
	.shutdown = xh2a_pcie_shutdown,
	.err_handler = &xh2a_pcie_err_handler,
	.groups = xh2a_pcie_drv_groups,
};

int __init xh2a_pcie_register_driver(void)
{
	pr_debug("%s\n", __func__);

	return pci_register_driver(&xh2a_pcie_driver);
}

void __exit xh2a_pcie_unregister_driver(void)
{
	pr_debug("%s\n", __func__);

	idr_destroy(&xh2a_minor_idr);

	return pci_unregister_driver(&xh2a_pcie_driver);
}

MODULE_LICENSE("GPL");
