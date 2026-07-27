// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright(c) 2026 Houmo AI Inc.
 * Author: mingdong.wang<mingdong.wang@houmo.ai>
 */

#include <linux/fs.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <xh2a_ctl_internal.h>
#include "xh2a_ctl.h"
#include "xh2a_ctl_dev.h"

static bool xh2a_ctl_lock_owner_alive(pid_t tgid)
{
	struct pid *pid;
	struct task_struct *task;
	bool alive = false;

	if (tgid <= 0)
		return false;

	pid = find_get_pid(tgid);
	if (!pid)
		return false;

	task = get_pid_task(pid, PIDTYPE_TGID);
	if (task) {
		alive = true;
		put_task_struct(task);
	}

	put_pid(pid);

	return alive;
}

static struct xh2a_ctl_dev_ctx *xh2a_ctl_get_lock_ctx(struct xh2a_ctl_ctx *ctl,
						      uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id == dev_id) {
			xh2a_ctl_ctx_get(ctl_ctx);
			mutex_unlock(&ctl->lock);
			return ctl_ctx;
		}
	}
	mutex_unlock(&ctl->lock);

	return NULL;
}

/**
 * xh2a_ctl_find_remove_lock_ctx - Find and remove lock context from list.
 * @ctl: Control context containing the lock list.
 * @dev_id: Device ID to find.
 *
 * Finds the lock context for the given device ID and removes it from the list.
 * Returns the lock context or NULL if not found.
 */
static struct xh2a_ctl_dev_ctx *
xh2a_ctl_find_remove_lock_ctx(struct xh2a_ctl_ctx *ctl, uint32_t dev_id)
{
	bool found = false;
	struct xh2a_ctl_dev_ctx *ctl_ctx = NULL, *tmp;

	mutex_lock(&ctl->lock);
	list_for_each_entry_safe(ctl_ctx, tmp, &ctl->ctl_list, node) {
		if (ctl_ctx->dev_id == dev_id) {
			list_del_init(&ctl_ctx->node);
			found = true;
			break;
		}
	}
	mutex_unlock(&ctl->lock);

	return found ? ctl_ctx : NULL;
}

/**
 * xh2a_ctl_update_lock_ctx_on_unregister - Update lock context when
 * unregistering.
 * @ctl_ctx: Lock context to update.
 * @ctldev: Device pointer for logging.
 */
void xh2a_ctl_update_lock_ctx_on_unregister(struct xh2a_ctl_dev_ctx *ctl_ctx,
					    struct device *ctldev)
{
	mutex_lock(&ctl_ctx->lock);

	/* Mark offline first to prevent new lock acquisitions */
	ctl_ctx->online = false;

	dev_dbg(ctldev, "%s: dev_id=%u offline, owner_tgid=%d refcnt=%u\n",
		__func__, ctl_ctx->dev_id, ctl_ctx->owner_tgid,
		ctl_ctx->refcnt);

	/* Clear lock context state */
	ctl_ctx->owner_tgid = 0;
	ctl_ctx->refcnt = 0;
	ctl_ctx->pcie_dev = NULL;

	mutex_unlock(&ctl_ctx->lock);
}

struct xh2a_ctl_dev_ctx *xh2a_ctl_create_lock(struct xh2a_ctl_ctx *ctl,
					      uint32_t dev_id, void *handle)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;

	ctl_ctx = kzalloc(sizeof(*ctl_ctx), GFP_KERNEL);
	if (!ctl_ctx)
		return NULL;

	kref_init(&ctl_ctx->kref);
	mutex_init(&ctl_ctx->lock);
	init_waitqueue_head(&ctl_ctx->wq);

	ctl_ctx->owner_tgid = 0;
	ctl_ctx->refcnt = 0;
	ctl_ctx->online = true;
	ctl_ctx->dev_id = dev_id;
	ctl_ctx->pcie_dev = handle;

	INIT_LIST_HEAD(&ctl_ctx->node);

	return ctl_ctx;
}

void xh2a_ctl_destroy_lock(struct xh2a_ctl_ctx *ctl, uint32_t dev_id)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	struct device *ctldev;

	if (!ctl)
		return;

	ctldev = ctl->miscdev.this_device;

	dev_dbg(ctldev, "%s: unregister dev_id=%u\n", __func__, dev_id);

	/* Find and remove lock context from list */
	ctl_ctx = xh2a_ctl_find_remove_lock_ctx(ctl, dev_id);
	if (!ctl_ctx)
		return;

	/* Update lock context status */
	xh2a_ctl_update_lock_ctx_on_unregister(ctl_ctx, ctldev);

	/* Wake up any waiters */
	wake_up_all(&ctl_ctx->wq);

	/* Release lock context (kref_put) */
	xh2a_ctl_ctx_put(ctl_ctx);
}

static int xh2a_ctl_try_lock_dev_locked(struct xh2a_ctl_dev_ctx *ctl_ctx,
					pid_t tgid, bool *owner_dead)
{
	if (owner_dead)
		*owner_dead = false;

	/* device offline: fail immediately */
	if (!ctl_ctx->online)
		return -ENODEV;

	/* owner dead: reclaim lock to avoid permanent deadlock */
	if (ctl_ctx->owner_tgid && ctl_ctx->owner_tgid != tgid &&
	    !xh2a_ctl_lock_owner_alive(ctl_ctx->owner_tgid)) {
		if (ctl_ctx->pcie_dev) {
			dev_dbg(&ctl_ctx->pcie_dev->pdev->dev,
				"%s: dev_id=%u reclaim from dead owner "
				"tgid=%d\n",
				__func__, ctl_ctx->dev_id, ctl_ctx->owner_tgid);
		}

		ctl_ctx->owner_tgid = 0;
		ctl_ctx->refcnt = 0;
		if (owner_dead)
			*owner_dead = true;
	}

	/* first acquire (0 -> 1): establish new owner */
	if (ctl_ctx->owner_tgid == 0) {
		ctl_ctx->owner_tgid = tgid;
		ctl_ctx->refcnt = 1;

		if (ctl_ctx->pcie_dev) {
			dev_dbg(&ctl_ctx->pcie_dev->pdev->dev,
				"%s: dev_id=%u new owner tgid=%d refcnt=1\n",
				__func__, ctl_ctx->dev_id, tgid);
		}

		return 0;
	}

	return -EBUSY;
}

static int xh2a_ctl_try_lock_once(struct xh2a_ctl_dev_ctx *ctl_ctx, pid_t tgid)
{
	int ret;
	bool owner_dead = false;

	mutex_lock(&ctl_ctx->lock);
	ret = xh2a_ctl_try_lock_dev_locked(ctl_ctx, tgid, &owner_dead);
	mutex_unlock(&ctl_ctx->lock);

	if (owner_dead)
		wake_up_all(&ctl_ctx->wq);

	return ret;
}

static bool xh2a_ctl_lock_is_ready(struct xh2a_ctl_dev_ctx *ctl_ctx, pid_t tgid)
{
	pid_t owner = READ_ONCE(ctl_ctx->owner_tgid);

	if (!READ_ONCE(ctl_ctx->online))
		return true;

	return (owner == 0);
}

/**
 * xh2a_ctl_unlock_single_dev_core - Core unlock logic for a single device.
 * @ctl_ctx: Lock context for the device.
 * @tgid: Thread group ID of the caller.
 * @ctldev: Device pointer for logging.
 * @dev_id: Device ID being unlocked.
 * @need_put: Output flag, true if this unlock released the last reference
 * (used to wake up waiters).
 *
 * Returns 0 on success, error code otherwise.
 */
static int xh2a_ctl_unlock_single_dev_core(struct xh2a_ctl_dev_ctx *ctl_ctx,
					   pid_t tgid, struct device *ctldev,
					   uint32_t dev_id, bool *need_put)
{
	int ret = 0;

	mutex_lock(&ctl_ctx->lock);

	if (!ctl_ctx->online) {
		ret = -ENODEV;
		dev_err(ctldev, "%s: UNLOCK dev_id=%u tgid=%d offline\n",
			__func__, dev_id, tgid);
		goto unlock;
	}

	/* Only the owner is allowed to unlock the device. */
	if (ctl_ctx->owner_tgid != tgid) {
		ret = -EPERM;
		dev_err(ctldev,
			"%s: UNLOCK dev_id=%u tgid=%d not owner (owner=%d)\n",
			__func__, dev_id, tgid, ctl_ctx->owner_tgid);
		goto unlock;
	}

	/* Check if refcnt is already 0 */
	if (ctl_ctx->refcnt == 0) {
		ret = -EINVAL;
		dev_err(ctldev,
			"%s: UNLOCK dev_id=%u tgid=%d refcnt already 0\n",
			__func__, dev_id, tgid);
		goto unlock;
	}

	ctl_ctx->refcnt--;

	dev_dbg(ctldev, "%s: UNLOCK dev_id=%u tgid=%d new refcnt=%u\n",
		__func__, dev_id, tgid, ctl_ctx->refcnt);

	if (ctl_ctx->refcnt == 0) {
		ctl_ctx->owner_tgid = 0;
		*need_put = true;
	}

unlock:
	mutex_unlock(&ctl_ctx->lock);
	return ret;
}

static long xh2a_ctl_unlock_single_dev(struct file *filp, uint32_t dev_id,
				       const struct xh2a_ctl_lock_req *req)
{
	int ret = 0;
	pid_t tgid;
	struct xh2a_ctl_ctx *ctl = filp->private_data;
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	bool need_put = false;
	struct device *ctldev;

	if (!ctl)
		return -EINVAL;
	if (dev_id >= XH2A_MAX_PCIE_CARDS)
		return -EINVAL;

	ctldev = ctl->miscdev.this_device;
	tgid = task_tgid_vnr(current);

	dev_dbg(ctldev, "%s: UNLOCK dev_id=%u tgid=%d\n", __func__, dev_id,
		tgid);

	ctl_ctx = xh2a_ctl_get_lock_ctx(ctl, dev_id);
	if (!ctl_ctx) {
		dev_err(ctldev,
			"%s: UNLOCK dev_id=%u tgid=%d no ctx (ENODEV)\n",
			__func__, dev_id, tgid);
		return -ENODEV;
	}

	ret = xh2a_ctl_unlock_single_dev_core(ctl_ctx, tgid, ctldev, dev_id,
					      &need_put);

	if (need_put)
		wake_up_all(&ctl_ctx->wq);

	xh2a_ctl_ctx_put(ctl_ctx);

	dev_dbg(ctldev, "%s: UNLOCK dev_id=%u tgid=%d ret=%d\n", __func__,
		dev_id, tgid, ret);

	return ret;
}

long xh2a_ctl_unlock_dev(struct file *filp, const struct xh2a_ctl_lock_req *req)
{
	pid_t tgid;
	long ret = 0;
	struct xh2a_ctl_ctx *ctl = filp->private_data;
	struct device *ctldev;
	uint64_t devmask = req->devmask;
	int i;

	if (!ctl)
		return -EINVAL;

	ctldev = ctl->miscdev.this_device;
	tgid = task_tgid_vnr(current);

	/* Handle bitmask case */
	dev_dbg(ctldev, "%s: UNLOCK devmask=0x%llx tgid=%d\n", __func__,
		(unsigned long long)devmask, tgid);

	/* Unlock all devices in the bitmask */
	for (i = 0; i < XH2A_MAX_PCIE_CARDS; i++) {
		if (devmask & (1ULL << i)) {
			long unlock_ret =
				xh2a_ctl_unlock_single_dev(filp, i, req);
			if (unlock_ret != 0) {
				dev_err(ctldev,
					"%s: UNLOCK dev_id=%u failed ret=%ld\n",
					__func__, i, unlock_ret);
				ret = unlock_ret;
			}
		}
	}

	return ret;
}

/**
 * xh2a_ctl_release_locks - Auto-unlock all devices owned by tgid.
 * @ctl:   ctl context.
 * @tgid:  Thread group ID whose locks should be released.
 *
 * Called from xh2a_ctl_file_release() when a process closes the ctl device.
 */
void xh2a_ctl_release_locks(struct xh2a_ctl_ctx *ctl, pid_t tgid)
{
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	struct xh2a_ctl_dev_ctx *locks[XH2A_MAX_PCIE_CARDS];
	unsigned int i, n = 0;
	struct device *ctldev;

	if (!ctl)
		return;

	ctldev = ctl->miscdev.this_device;

	mutex_lock(&ctl->lock);
	list_for_each_entry(ctl_ctx, &ctl->ctl_list, node) {
		if (ctl_ctx->owner_tgid == tgid && n < XH2A_MAX_PCIE_CARDS) {
			xh2a_ctl_ctx_get(ctl_ctx);
			locks[n++] = ctl_ctx;
		}
	}
	mutex_unlock(&ctl->lock);

	for (i = 0; i < n; i++) {
		bool need_put = false;
		int ret;

		ctl_ctx = locks[i];

		ret = xh2a_ctl_unlock_single_dev_core(
			ctl_ctx, tgid, ctldev, ctl_ctx->dev_id, &need_put);
		if (ret) {
			dev_dbg(ctldev,
				"%s: release auto-unlock dev_id=%u tgid=%d "
				"ret=%d\n",
				__func__, ctl_ctx->dev_id, tgid, ret);
		}

		/* Always wake up waiters, even if unlock failed */
		if (need_put)
			wake_up_all(&ctl_ctx->wq);

		xh2a_ctl_ctx_put(ctl_ctx);
	}
}

/**
 * xh2a_ctl_lock_single_dev_nonblock - Try to lock a device in non-blocking
 * mode.
 * @ctl_ctx: Lock context for the device.
 * @tgid: Thread group ID of the caller.
 * @ctldev: Device pointer for logging.
 * @dev_id: Device ID being locked.
 *
 * Returns 0 on success, error code otherwise.
 */
static long xh2a_ctl_lock_single_dev_nonblock(struct xh2a_ctl_dev_ctx *ctl_ctx,
					      pid_t tgid, struct device *ctldev,
					      uint32_t dev_id)
{
	long ret = xh2a_ctl_try_lock_once(ctl_ctx, tgid);
	dev_dbg(ctldev, "%s: LOCK(NONBLOCK) dev_id=%u tgid=%d ret=%ld\n",
		__func__, dev_id, tgid, ret);
	return ret;
}

/**
 * xh2a_ctl_lock_single_dev_block - Try to lock a device in blocking mode.
 * @ctl_ctx: Lock context for the device.
 * @tgid: Thread group ID of the caller.
 * @ctldev: Device pointer for logging.
 * @dev_id: Device ID being locked.
 * @timeout_j: Timeout in jiffies.
 *
 * Returns 0 on success, error code otherwise.
 */
static long xh2a_ctl_lock_single_dev_block(struct xh2a_ctl_dev_ctx *ctl_ctx,
					   pid_t tgid, struct device *ctldev,
					   uint32_t dev_id,
					   unsigned long timeout_j)
{
	long ret;

	for (;;) {
		ret = xh2a_ctl_try_lock_once(ctl_ctx, tgid);

		/*
		 * If the lock is acquired successfully or any non-busy error
		 * occurs (e.g. ENODEV, EINTR), exit the loop.
		 */
		if (ret != -EBUSY)
			return ret;

		/* The device is currently owned; wait until available. */
		if (timeout_j) {
			long left = wait_event_interruptible_timeout(
				ctl_ctx->wq,
				xh2a_ctl_lock_is_ready(ctl_ctx, tgid),
				timeout_j);

			if (left == 0) {
				ret = -ETIMEDOUT;
				dev_err(ctldev,
					"%s: LOCK dev_id=%u tgid=%d timeout\n",
					__func__, dev_id, tgid);
				return ret;
			}
			if (left < 0) {
				ret = left;
				dev_err(ctldev,
					"%s: LOCK dev_id=%u tgid=%d wake "
					"err=%ld\n",
					__func__, dev_id, tgid, ret);
				return ret;
			}

			/* Update remaining timeout */
			timeout_j = left;
		} else {
			/*
			 * Infinite wait: sleep until the lock available
			 * or a signal interrupts.
			 */
			ret = wait_event_interruptible(
				ctl_ctx->wq,
				xh2a_ctl_lock_is_ready(ctl_ctx, tgid));
			if (ret) {
				dev_dbg(ctldev,
					"%s: LOCK dev_id=%u tgid=%d "
					"interrupted ret=%ld\n",
					__func__, dev_id, tgid, ret);
				return ret;
			}
		}
	}
}

static long xh2a_ctl_lock_single_dev(struct file *filp, uint32_t dev_id,
				     const struct xh2a_ctl_lock_req *req)
{
	pid_t tgid;
	long ret;
	struct xh2a_ctl_ctx *ctl = filp->private_data;
	struct xh2a_ctl_dev_ctx *ctl_ctx;
	unsigned long timeout_j = 0;
	struct device *ctldev;

	if (!ctl)
		return -EINVAL;
	if (dev_id >= XH2A_MAX_PCIE_CARDS)
		return -EINVAL;

	ctldev = ctl->miscdev.this_device;
	tgid = task_tgid_vnr(current);

	dev_dbg(ctldev, "%s: LOCK dev_id=%u tgid=%d timeout_ms=%u flags=0x%x\n",
		__func__, dev_id, tgid, req->timeout_ms, req->flags);

	ctl_ctx = xh2a_ctl_get_lock_ctx(ctl, dev_id);
	if (!ctl_ctx) {
		dev_err(ctldev, "%s: LOCK dev_id=%u tgid=%d no ctx (ENODEV)\n",
			__func__, dev_id, tgid);
		return -ENODEV;
	}

	if (req->timeout_ms)
		timeout_j = msecs_to_jiffies(req->timeout_ms);

	if (req->flags & XH2A_LOCK_F_NONBLOCK) {
		ret = xh2a_ctl_lock_single_dev_nonblock(ctl_ctx, tgid, ctldev,
							dev_id);
	} else {
		ret = xh2a_ctl_lock_single_dev_block(ctl_ctx, tgid, ctldev,
						     dev_id, timeout_j);
	}

	dev_dbg(ctldev, "%s: LOCK dev_id=%u tgid=%d final ret=%ld\n", __func__,
		dev_id, tgid, ret);
	xh2a_ctl_ctx_put(ctl_ctx);

	return ret;
}

/**
 * xh2a_ctl_lock_rollback - Rollback previously locked devices.
 * @filp: File pointer for the ctl device.
 * @req: Lock request structure containing flags and other parameters.
 * @devmask: Device bitmask indicating which devices were being locked.
 * @last_dev: The last device ID that was attempted to lock (exclusive).
 * @ctldev: Device pointer for logging.
 *
 * Unlocks all devices in the bitmask that were previously locked (device IDs
 * less than last_dev), in reverse order to avoid deadlocks.
 */
static void xh2a_ctl_lock_rollback(struct file *filp,
				   const struct xh2a_ctl_lock_req *req,
				   uint64_t devmask, int last_dev,
				   struct device *ctldev)
{
	int j;

	/* Unlock all previously locked devices in reverse order */
	for (j = last_dev - 1; j >= 0; j--) {
		if (devmask & (1ULL << j)) {
			long rollback_ret =
				xh2a_ctl_unlock_single_dev(filp, j, req);
			if (rollback_ret != 0) {
				dev_err(ctldev,
					"%s: ROLLBACK dev_id=%u failed "
					"ret=%ld\n",
					__func__, j, rollback_ret);
			}
		}
	}
}

long xh2a_ctl_lock_dev(struct file *filp, const struct xh2a_ctl_lock_req *req)
{
	pid_t tgid;
	long ret;
	struct xh2a_ctl_ctx *ctl = filp->private_data;
	struct device *ctldev;
	uint64_t devmask = req->devmask;
	int i;

	if (!ctl)
		return -EINVAL;

	ctldev = ctl->miscdev.this_device;
	tgid = task_tgid_vnr(current);

	/* Handle bitmask case */
	dev_dbg(ctldev,
		"%s: LOCK devmask=0x%llx tgid=%d timeout_ms=%u flags=0x%x\n",
		__func__, (unsigned long long)devmask, tgid, req->timeout_ms,
		req->flags);

	/*
	 * Lock all devices in ascending order by device ID to prevent
	 * deadlock when multiple processes try to lock overlapping sets
	 * of devices in different orders.
	 */
	for (i = 0; i < XH2A_MAX_PCIE_CARDS; i++) {
		if (devmask & (1ULL << i)) {
			ret = xh2a_ctl_lock_single_dev(filp, i, req);
			if (ret != 0) {
				/* If any lock fails, rollback all previously
				 * locked devices */
				xh2a_ctl_lock_rollback(filp, req, devmask, i,
						       ctldev);
				return ret;
			}
		}
	}

	return 0;
}

/**
 * xh2a_ctl_cleanup_all_locks - Cleanup helper for ctl exit.
 * @ctl: ctl context.
 *
 * Marks all devices offline, wakes up waiters and drops kref on all
 * per-device contexts.
 */
void xh2a_ctl_cleanup_all_locks(struct xh2a_ctl_ctx *ctl)
{
	struct xh2a_ctl_dev_ctx *cur, *tmp;
	struct xh2a_ctl_dev_ctx *to_free[XH2A_MAX_PCIE_CARDS];
	unsigned int i, n = 0;

	if (!ctl)
		return;

	/* First, collect all contexts and mark them offline */
	mutex_lock(&ctl->lock);
	list_for_each_entry_safe(cur, tmp, &ctl->ctl_list, node) {
		list_del_init(&cur->node);
		cur->online = false;
		cur->owner_tgid = 0;
		cur->refcnt = 0;
		cur->pcie_dev = NULL;

		if (n < XH2A_MAX_PCIE_CARDS)
			to_free[n++] = cur;
	}
	mutex_unlock(&ctl->lock);

	/* Then wake up waiters and drop kref outside the mutex */
	for (i = 0; i < n; i++) {
		wake_up_all(&to_free[i]->wq);
		xh2a_ctl_ctx_put(to_free[i]);
	}
}

MODULE_LICENSE("GPL");
