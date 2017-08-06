/**
 * drivers/usb/common/usb-otg.c - USB OTG core
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Roger Quadros <rogerq@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/usb/of.h>
#include <linux/usb/otg.h>
#include <linux/usb/gadget.h>
#include <linux/workqueue.h>

/* OTG device list */
LIST_HEAD(otg_list);
static DEFINE_MUTEX(otg_list_mutex);

/**
 * usb_otg_get_data() - get usb_otg data structure
 * @otg_dev:	OTG controller device
 *
 * Check if the OTG device is in our OTG list and return
 * usb_otg data, else NULL.
 *
 * otg_list_mutex must be held.
 *
 * Return: usb_otg data on success, NULL otherwise.
 */
static struct usb_otg *usb_otg_get_data(struct device *otg_dev)
{
	struct usb_otg *otg;

	if (!otg_dev)
		return NULL;

	list_for_each_entry(otg, &otg_list, list) {
		if (otg->dev == otg_dev)
			return otg;
	}

	return NULL;
}

/**
 * usb_otg_start_host() - start/stop the host controller
 * @otg:	usb_otg instance
 * @on:		true to start, false to stop
 *
 * Start/stop the USB host controller. This function is meant
 * for use by the OTG controller driver.
 *
 * Return: 0 on success, error value otherwise.
 */
int usb_otg_start_host(struct usb_otg *otg, int on)
{
	struct otg_hcd_ops *hcd_ops = otg->hcd_ops;
	int ret;

	dev_dbg(otg->dev, "otg: %s %d\n", __func__, on);
	if (!otg->host) {
		dev_info(otg->dev, "otg: fsm running without host\n");
		return -EINVAL;
	}

	if (on) {
		if (otg->flags & OTG_FLAG_HOST_RUNNING)
			return 0;

		/* start host */
		ret = hcd_ops->start(otg->hcd);
		if (ret) {
			dev_err(otg->dev, "otg: host start failed: %d\n", ret);
			return ret;
		}

		otg->flags |= OTG_FLAG_HOST_RUNNING;
	} else {
		if (!(otg->flags & OTG_FLAG_HOST_RUNNING))
			return 0;

		/* stop host */
		ret = hcd_ops->stop(otg->hcd);
		if (ret) {
			dev_err(otg->dev, "otg: host stop failed: %d\n", ret);
			return ret;
		}

		otg->flags &= ~OTG_FLAG_HOST_RUNNING;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_start_host);

/**
 * usb_otg_start_gadget() - start/stop the gadget controller
 * @otg:	usb_otg instance
 * @on:		true to start, false to stop
 *
 * Start/stop the USB gadget controller. This function is meant
 * for use by the OTG controller driver.
 *
 * Return: 0 on success, error value otherwise.
 */
int usb_otg_start_gadget(struct usb_otg *otg, int on)
{
	struct usb_gadget *gadget = otg->gadget;
	int ret;

	dev_dbg(otg->dev, "otg: %s %d\n", __func__, on);
	if (!gadget) {
		dev_info(otg->dev, "otg: fsm running without gadget\n");
		return -EINVAL;
	}

	if (on) {
		if (otg->flags & OTG_FLAG_GADGET_RUNNING)
			return 0;

		ret = otg->gadget_ops->start(otg->gadget);
		if (ret) {
			dev_err(otg->dev, "otg: gadget start failed: %d\n",
				ret);
			return ret;
		}

		otg->flags |= OTG_FLAG_GADGET_RUNNING;
	} else {
		if (!(otg->flags & OTG_FLAG_GADGET_RUNNING))
			return 0;

		ret = otg->gadget_ops->stop(otg->gadget);
		if (ret) {
			dev_err(otg->dev, "otg: gadget stop failed: %d\n",
				ret);
			return ret;
		}
		otg->flags &= ~OTG_FLAG_GADGET_RUNNING;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_start_gadget);

/**
 * usb_otg_register() - Register the OTG/dual-role device to OTG core
 * @dev: OTG/dual-role controller device.
 * @config: OTG configuration.
 *
 * Registers the OTG/dual-role controller device with the USB OTG core.
 *
 * Return: struct usb_otg * if success, ERR_PTR() otherwise.
 */
struct usb_otg *usb_otg_register(struct device *dev,
				 struct usb_otg_config *config)
{
	struct usb_otg *otg;
	int ret = 0;

	if (!dev || !config || !config->fsm_ops)
		return ERR_PTR(-EINVAL);

	/* already in list? */
	mutex_lock(&otg_list_mutex);
	if (usb_otg_get_data(dev)) {
		dev_err(dev, "otg: %s: device already in otg list\n", __func__);
		ret = -EINVAL;
		goto unlock;
	}

	/* allocate and add to list */
	otg = kzalloc(sizeof(*otg), GFP_KERNEL);
	if (!otg) {
		ret = -ENOMEM;
		goto unlock;
	}

	otg->dev = dev;
	/* otg->caps is controller caps + DT overrides */
	otg->caps = config->otg_caps;
	ret = of_usb_update_otg_caps(dev->of_node, &otg->caps);
	if (ret)
		goto err_wq;

	if (!config->otg_work) {
		dev_err(dev,
			"otg: otg_work must be provided for OTG support\n");
		ret = -EINVAL;
		goto err_wq;
	}

	INIT_WORK(&otg->work, config->otg_work);

	/* We need high priority workqueue to meet strict OTG2.0 timing */
	otg->wq = alloc_workqueue("usb_otg", WQ_HIGHPRI, 0);
	if (!otg->wq) {
		dev_err(dev, "otg: %s: can't create workqueue\n", __func__);
		ret = -ENOMEM;
		goto err_wq;
	}

	/* set otg ops */
	otg->fsm.ops = config->fsm_ops;

	mutex_init(&otg->fsm.lock);

	list_add_tail(&otg->list, &otg_list);
	mutex_unlock(&otg_list_mutex);

	return otg;

err_wq:
	kfree(otg);
unlock:
	mutex_unlock(&otg_list_mutex);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(usb_otg_register);

/**
 * usb_otg_unregister() - Unregister the OTG/dual-role device from USB OTG core
 * @dev: OTG controller device.
 *
 * Unregisters the OTG/dual-role controller device from USB OTG core.
 * Prevents unregistering till both the associated Host and Gadget controllers
 * have unregistered from the OTG core.
 *
 * Return: 0 on success, error value otherwise.
 */
int usb_otg_unregister(struct device *dev)
{
	struct usb_otg *otg;

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(dev);
	if (!otg) {
		dev_err(dev, "otg: %s: device not in otg list\n",
			__func__);
		mutex_unlock(&otg_list_mutex);
		return -EINVAL;
	}

	/* prevent unregister till both host & gadget have unregistered */
	if (otg->host || otg->gadget) {
		dev_err(dev, "otg: %s: host/gadget still registered\n",
			__func__);
		mutex_unlock(&otg_list_mutex);
		return -EBUSY;
	}

	/* OTG FSM is halted when host/gadget unregistered */
	destroy_workqueue(otg->wq);

	/* remove from otg list */
	list_del(&otg->list);
	kfree(otg);
	mutex_unlock(&otg_list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_unregister);

/**
 * usb_otg_start_fsm() - Start the OTG FSM
 * @otg:	usb_otg instance
 *
 * Start the OTG FSM if we can. The HCD, UDC and gadget function driver
 * must be ready for the OTG FSM to start.
 *
 * fsm->lock must be held.
 */
static void usb_otg_start_fsm(struct usb_otg *otg)
{
	struct otg_fsm *fsm = &otg->fsm;

	if (fsm->running)
		goto kick_fsm;

	/* we start FSM even if one of host or gadget hasn't registered */
	if (!otg->host)
		dev_info(otg->dev, "otg: host not registered yet\n");

	if (!otg->gadget)
		dev_info(otg->dev, "otg: gadget not registered yet\n");

	dev_info(otg->dev, "otg: start OTG finite state machine\n");
	fsm->running = true;
kick_fsm:
	queue_work(otg->wq, &otg->work);
}

/**
 * usb_otg_stop_fsm() - Stop the OTG FSM
 * @otg:	usb_otg instance
 *
 * Stops the HCD, UDC and the OTG FSM.
 *
 * fsm->lock must be held.
 */
static void usb_otg_stop_fsm(struct usb_otg *otg)
{
	struct otg_fsm *fsm = &otg->fsm;

	if (!fsm->running)
		return;

	/* no more new events queued */
	fsm->running = false;

	flush_workqueue(otg->wq);
	otg->state = OTG_STATE_UNDEFINED;

	/* stop host/gadget immediately */
	if (fsm->protocol == PROTO_HOST)
		otg_start_host(otg, 0);
	else if (fsm->protocol == PROTO_GADGET)
		otg_start_gadget(otg, 0);
	fsm->protocol = PROTO_UNDEF;
}

/**
 * usb_otg_kick_fsm - Kick the OTG state machine
 * @hcd_gcd_device:	Host/Gadget controller device
 *
 * Used by USB host/device stack to sync OTG related
 * events to the OTG state machine.
 * e.g. change in host_bus->b_hnp_enable, gadget->b_hnp_enable
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_kick_fsm(struct device *hcd_gcd_device)
{
	struct usb_otg *otg;

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(of_usb_get_otg(hcd_gcd_device->of_node));
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_dbg(hcd_gcd_device, "otg: %s: invalid host/gadget device\n",
			__func__);
		return -ENODEV;
	}

	usb_otg_sync_inputs(otg);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_kick_fsm);

/**
 * usb_otg_sync_inputs() - Sync OTG inputs with the OTG state machine
 * @otg:	usb_otg instance
 *
 * Used by the OTG driver to update the inputs to the OTG
 * state machine.
 *
 * Can be called in IRQ context.
 */
void usb_otg_sync_inputs(struct usb_otg *otg)
{
	/* Don't kick FSM till it has started */
	if (!otg->fsm.running)
		return;

	/* Kick FSM */
	queue_work(otg->wq, &otg->work);
}
EXPORT_SYMBOL_GPL(usb_otg_sync_inputs);

/**
 * usb_otg_register_hcd() - Register the host controller to OTG core
 * @hcd:	host controller
 * @irqnum:	interrupt number
 * @irqflags:	interrupt flags
 * @ops:	HCD ops to interface with the HCD
 *
 * This is used by the USB Host stack to register the host controller
 * to the OTG core. Host controller must not be started by the
 * caller as it is left up to the OTG state machine to do so.
 * hcd->otg_dev must contain the related otg controller device.
 *
 * Return: 0 on success, error value otherwise.
 */
int usb_otg_register_hcd(struct usb_hcd *hcd, struct otg_hcd_ops *ops)
{
	struct usb_otg *otg;
	struct device *hcd_dev = hcd->self.controller;
	struct device *otg_dev = hcd->otg_dev;

	if (!otg_dev)
		return -EINVAL;	/* we're definitely not OTG */

	/* we're otg but otg controller might not yet be registered */
	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_dbg(hcd_dev,
			"otg: controller not yet registered. deferring.\n");
		return -EPROBE_DEFER;
	}

	/* HCD will be started by OTG fsm when needed */
	mutex_lock(&otg->fsm.lock);
	otg->hcd = hcd;
	otg->hcd_ops = ops;
	dev_info(otg_dev, "otg: host %s registered\n",
		 dev_name(hcd->self.controller));

	otg->host = hcd_to_bus(hcd);

	/* start FSM */
	usb_otg_start_fsm(otg);
	mutex_unlock(&otg->fsm.lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_register_hcd);

/**
 * usb_otg_unregister_hcd() - Unregister the host controller from OTG core
 * @hcd:	host controller device
 *
 * This is used by the USB Host stack to unregister the host controller
 * from the OTG core. Ensures that host controller is not running
 * on successful return.
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_unregister_hcd(struct usb_hcd *hcd)
{
	struct usb_otg *otg;
	struct device *hcd_dev = hcd_to_bus(hcd)->controller;
	struct device *otg_dev = hcd->otg_dev;

	if (!otg_dev)
		return -EINVAL;	/* we're definitely not OTG */

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_err(hcd_dev, "otg: host %s wasn't registered with otg\n",
			dev_name(hcd_dev));
		return -EINVAL;
	}

	mutex_lock(&otg->fsm.lock);
	if (hcd == otg->hcd) {
		otg->hcd = NULL;
		dev_info(otg_dev, "otg: host %s unregistered\n",
			 dev_name(hcd_dev));
	} else {
		mutex_unlock(&otg->fsm.lock);
		dev_err(otg_dev, "otg: host %s wasn't registered with otg\n",
			dev_name(hcd_dev));
		return -EINVAL;
	}

	/* stop FSM & Host */
	usb_otg_stop_fsm(otg);
	otg->host = NULL;

	mutex_unlock(&otg->fsm.lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_unregister_hcd);

/**
 * usb_otg_register_gadget() - Register the gadget controller to OTG core
 * @gadget:	gadget controller instance
 * @ops:	gadget interface ops
 *
 * This is used by the USB gadget stack to register the gadget controller
 * to the OTG core. Gadget controller must not be started by the
 * caller as it is left up to the OTG state machine to do so.
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_register_gadget(struct usb_gadget *gadget,
			    struct otg_gadget_ops *ops)
{
	struct usb_otg *otg;
	struct device *gadget_dev = &gadget->dev;
	struct device *otg_dev = gadget->otg_dev;

	if (!otg_dev)
		return -EINVAL;	/* we're definitely not OTG */

	/* we're otg but otg controller might not yet be registered */
	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_dbg(gadget_dev,
			"otg: controller not yet registered, deferring.\n");
		return -EPROBE_DEFER;
	}

	mutex_lock(&otg->fsm.lock);
	if (otg->gadget) {
		dev_err(otg_dev, "otg: gadget already registered with otg\n");
		mutex_unlock(&otg->fsm.lock);
		return -EINVAL;
	}

	otg->gadget = gadget;
	otg->gadget_ops = ops;
	dev_info(otg_dev, "otg: gadget %s registered\n",
		 dev_name(&gadget->dev));

	if (!gadget->otg_caps) {
		dev_info(otg_dev, "set gadget otg_caps from OTG controller\n");
		gadget->otg_caps = &otg->caps;
	}

	/* start FSM */
	usb_otg_start_fsm(otg);
	mutex_unlock(&otg->fsm.lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_register_gadget);

/**
 * usb_otg_unregister_gadget() - Unregister the gadget controller from OTG core
 * @gadget:	gadget controller
 *
 * This is used by the USB gadget stack to unregister the gadget controller
 * from the OTG core. Ensures that gadget controller is not running
 * on successful return.
 *
 * Returns: 0 on success, error value otherwise.
 */
int usb_otg_unregister_gadget(struct usb_gadget *gadget)
{
	struct usb_otg *otg;
	struct device *gadget_dev = &gadget->dev;
	struct device *otg_dev = gadget->otg_dev;

	if (!otg_dev)
		return -EINVAL;

	mutex_lock(&otg_list_mutex);
	otg = usb_otg_get_data(otg_dev);
	mutex_unlock(&otg_list_mutex);
	if (!otg) {
		dev_err(gadget_dev,
			"otg: gadget %s wasn't registered with otg\n",
			dev_name(&gadget->dev));
		return -EINVAL;
	}

	mutex_lock(&otg->fsm.lock);
	if (otg->gadget != gadget) {
		mutex_unlock(&otg->fsm.lock);
		dev_err(otg_dev, "otg: gadget %s wasn't registered with otg\n",
			dev_name(&gadget->dev));
		return -EINVAL;
	}

	/* stop FSM & gadget */
	usb_otg_stop_fsm(otg);
	otg->gadget = NULL;
	mutex_unlock(&otg->fsm.lock);

	dev_info(otg_dev, "otg: gadget %s unregistered\n",
		 dev_name(&gadget->dev));

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_unregister_gadget);

int usb_otg_notify(struct device *dev, enum usb_otg_event event)
{
	struct usb_otg *otg = usb_otg_get_data(of_usb_get_otg(dev->of_node));

	if (!otg) {
		dev_dbg(dev, "otg: %s: no otg controller, skip event\n",
				__func__);
		return -EINVAL;
	}

	atomic_notifier_call_chain(&otg->notifier, event, dev);

	return 0;
}
EXPORT_SYMBOL_GPL(usb_otg_notify);

const char *usb_otg_event_string(enum usb_otg_event event)
{
	static const char *const names[] = {
		[OTG_EVENT_HCD_PORT_SUSPEND] = "HCD_PORT_SUSPEND",
		[OTG_EVENT_HCD_PORT_RESUME] = "HCD_PORT_RESUME",
		[OTG_EVENT_HCD_PORT_CONNECT] = "HCD_PORT_CONNECT",
		[OTG_EVENT_HCD_PORT_DISCONNECT] = "HCD_PORT_DISCONNECT",
		[OTG_EVENT_PCD_PORT_SUSPEND] = "PCD_PORT_SUSPEND",
		[OTG_EVENT_PCD_PORT_RESUME] = "PCD_PORT_RESUME",
		[OTG_EVENT_PCD_PORT_CONNECT] = "PCD_PORT_CONNECT",
		[OTG_EVENT_PCD_PORT_DISCONNECT] = "PCD_PORT_DISCONNECT",
		[OTG_EVENT_HCD_TEST_DEVICE] = "OTG_EVENT_HCD_TEST_DEVICE",
	};

	if (event < 0 || event >= ARRAY_SIZE(names))
		return "UNDEFINED";

	return names[event];
}
EXPORT_SYMBOL_GPL(usb_otg_event_string);

MODULE_LICENSE("GPL");
