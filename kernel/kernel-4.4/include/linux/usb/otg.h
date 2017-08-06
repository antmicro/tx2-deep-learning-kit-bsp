/* USB OTG (On The Go) defines */
/*
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the  GNU General Public License along
 * with this program; if not, write  to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * USB OTG (On The Go) defines
 *
 * These APIs may be used between USB controllers.  USB device drivers
 * (for either host or peripheral roles) don't use these calls; they
 * continue to use just usb_device and usb_gadget.
 */

#ifndef __LINUX_USB_OTG_H
#define __LINUX_USB_OTG_H

#include <linux/phy/phy.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/usb/gadget.h>
#include <linux/usb/otg-fsm.h>
#include <linux/usb/phy.h>

/**
 * struct usb_otg_caps - describes the otg capabilities of the device
 * @otg_rev: The OTG revision number the device is compliant with, it's
 *		in binary-coded decimal (i.e. 2.0 is 0200H).
 * @hnp_support: Indicates if the device supports HNP.
 * @srp_support: Indicates if the device supports SRP.
 * @adp_support: Indicates if the device supports ADP.
 */
struct usb_otg_caps {
	u16 otg_rev;
	bool hnp_support;
	bool srp_support;
	bool adp_support;
	bool rsp_support;
};

/**
 * enum usb_otg_event - otg event for HCD/PCD to notify to OTG controller
 */
enum usb_otg_event {
	OTG_EVENT_HCD_PORT_SUSPEND = 0,
	OTG_EVENT_HCD_PORT_RESUME,
	OTG_EVENT_HCD_PORT_CONNECT,
	OTG_EVENT_HCD_PORT_DISCONNECT,
	OTG_EVENT_PCD_PORT_SUSPEND,
	OTG_EVENT_PCD_PORT_RESUME,
	OTG_EVENT_PCD_PORT_CONNECT,
	OTG_EVENT_PCD_PORT_DISCONNECT,
	OTG_EVENT_HCD_TEST_DEVICE,
};

/**
 * struct usb_otg - usb otg controller state
 *
 * @default_a: Indicates we are an A device. i.e. Host.
 * @phy: USB PHY interface
 * @usb_phy: old usb_phy interface
 * @host: host controller bus
 * @gadget: gadget device
 * @state: current OTG state
 * @dev: OTG controller device
 * @caps: OTG capabilities revision, hnp, srp, etc
 * @fsm: OTG finite state machine
 * @hcd_ops: host controller interface
 * ------- internal use only -------
 * @primary_hcd: primary host state and interface
 * @shared_hcd: shared host state and interface
 * @gadget_ops: gadget controller interface
 * @list: list of OTG controllers
 * @work: OTG state machine work
 * @wq: OTG state machine work queue
 * @flags: to track if host/gadget is running
 */
struct usb_otg {
	u8			default_a;

	struct phy		*phy;
	/* old usb_phy interface */
	struct usb_phy		*usb_phy;
	struct usb_bus		*host;
	struct usb_gadget	*gadget;

	enum usb_otg_state	state;
	struct device *dev;
	struct usb_otg_caps	caps;
	struct otg_fsm fsm;

	/* internal use only */
	struct usb_hcd *hcd;
	struct otg_hcd_ops *hcd_ops;
	struct otg_gadget_ops *gadget_ops;
	bool gadget_ready;
	struct list_head list;
	struct work_struct work;
	struct workqueue_struct *wq;
	u32 flags;
#define OTG_FLAG_GADGET_RUNNING (1 << 0)
#define OTG_FLAG_HOST_RUNNING (1 << 1)
	/* use otg->fsm.lock for serializing access */

	/* for notification of OTG events */
	struct atomic_notifier_head notifier;

/*------------- deprecated interface -----------------------------*/
	/* bind/unbind the host controller */
	int	(*set_host)(struct usb_otg *otg, struct usb_bus *host);

	/* bind/unbind the peripheral controller */
	int	(*set_peripheral)(struct usb_otg *otg,
					struct usb_gadget *gadget);

	/* effective for A-peripheral, ignored for B devices */
	int	(*set_vbus)(struct usb_otg *otg, bool enabled);

	/* for B devices only:  start session with A-Host */
	int	(*start_srp)(struct usb_otg *otg);

	/* start or continue HNP role switch */
	int	(*start_hnp)(struct usb_otg *otg);
/*---------------------------------------------------------------*/
};

/**
 * struct usb_otg_config - OTG controller configuration
 * @caps: OTG capabilities of the controller
 * @ops: OTG FSM operations
 * @otg_work: optional custom OTG state machine work function
 */
struct usb_otg_config {
	struct usb_otg_caps otg_caps;
	struct otg_fsm_ops *fsm_ops;
	void (*otg_work)(struct work_struct *work);
};

extern const char *usb_otg_state_string(enum usb_otg_state state);
extern const char *usb_otg_event_string(enum usb_otg_event state);

#if IS_ENABLED(CONFIG_USB_OTG)
struct usb_otg *usb_otg_register(struct device *dev,
				 struct usb_otg_config *config);
int usb_otg_unregister(struct device *dev);
int usb_otg_register_hcd(struct usb_hcd *hcd, struct otg_hcd_ops *ops);
int usb_otg_unregister_hcd(struct usb_hcd *hcd);
int usb_otg_register_gadget(struct usb_gadget *gadget,
			    struct otg_gadget_ops *ops);
int usb_otg_unregister_gadget(struct usb_gadget *gadget);
int usb_otg_kick_fsm(struct device *hcd_gcd_device);
void usb_otg_sync_inputs(struct usb_otg *otg);
int usb_otg_start_host(struct usb_otg *otg, int on);
int usb_otg_start_gadget(struct usb_otg *otg, int on);

/* used by HCD/PCD to notify events to USB core */
int usb_otg_notify(struct device *dev, enum usb_otg_event event);

/* used by OTG controller to register OTG event notifier */
static inline int usb_otg_register_notifier(struct usb_otg *otg,
					    struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&otg->notifier, nb);
}

/* used by OTG controller to unregister OTG event notifier */
static inline void usb_otg_unregister_notifier(struct usb_otg *otg,
					       struct notifier_block *nb)
{
	atomic_notifier_chain_unregister(&otg->notifier, nb);
}

#else /* CONFIG_USB_OTG */

static inline struct usb_otg *usb_otg_register(struct device *dev,
					       struct usb_otg_config *config)
{
	return ERR_PTR(-ENOTSUPP);
}

static inline int usb_otg_unregister(struct device *dev)
{
	return -ENOTSUPP;
}

static inline int usb_otg_register_hcd(struct usb_hcd *hcd,
				       struct otg_hcd_ops *ops)
{
	return -ENOTSUPP;
}

static inline int usb_otg_unregister_hcd(struct usb_hcd *hcd)
{
	return -ENOTSUPP;
}

static inline int usb_otg_register_gadget(struct usb_gadget *gadget,
					  struct otg_gadget_ops *ops)
{
	return -ENOTSUPP;
}

static inline int usb_otg_unregister_gadget(struct usb_gadget *gadget)
{
	return -ENOTSUPP;
}

static inline int usb_otg_kick_fsm(struct device *hcd_gcd_device)
{
	return -ENOTSUPP;
}

static inline void usb_otg_sync_inputs(struct usb_otg *otg)
{
}

static inline int usb_otg_start_host(struct usb_otg *otg, int on)
{
	return -ENOTSUPP;
}

static inline int usb_otg_start_gadget(struct usb_otg *otg, int on)
{
	return -ENOTSUPP;
}

static inline int usb_otg_notify(struct device *dev, enum usb_otg_event event)
{
	return -ENOTSUPP;
}

static inline int usb_otg_register_notifier(struct usb_otg *otg,
					    struct notifier_block *nb)
{
	return -ENOTSUPP;
}

static inline void usb_otg_unregister_notifier(struct usb_otg *otg,
					       struct notifier_block *nb)
{
}

#endif /* CONFIG_USB_OTG */

/*------------- deprecated interface -----------------------------*/
/* Context: can sleep */
static inline int
otg_start_hnp(struct usb_otg *otg)
{
	if (otg && otg->start_hnp)
		return otg->start_hnp(otg);

	return -ENOTSUPP;
}

/* Context: can sleep */
static inline int
otg_set_vbus(struct usb_otg *otg, bool enabled)
{
	if (otg && otg->set_vbus)
		return otg->set_vbus(otg, enabled);

	return -ENOTSUPP;
}

/* for HCDs */
static inline int
otg_set_host(struct usb_otg *otg, struct usb_bus *host)
{
	if (otg && otg->set_host)
		return otg->set_host(otg, host);

	return -ENOTSUPP;
}

/* for usb peripheral controller drivers */

/* Context: can sleep */
static inline int
otg_set_peripheral(struct usb_otg *otg, struct usb_gadget *periph)
{
	if (otg && otg->set_peripheral)
		return otg->set_peripheral(otg, periph);

	return -ENOTSUPP;
}

static inline int
otg_start_srp(struct usb_otg *otg)
{
	if (otg && otg->start_srp)
		return otg->start_srp(otg);

	return -ENOTSUPP;
}

/*---------------------------------------------------------------*/

/* for OTG controller drivers (and maybe other stuff) */
extern int usb_bus_start_enum(struct usb_bus *bus, unsigned port_num);

enum usb_dr_mode {
	USB_DR_MODE_UNKNOWN,
	USB_DR_MODE_HOST,
	USB_DR_MODE_PERIPHERAL,
	USB_DR_MODE_OTG,
};

/**
 * usb_get_dr_mode - Get dual role mode for given device
 * @dev: Pointer to the given device
 *
 * The function gets phy interface string from property 'dr_mode',
 * and returns the correspondig enum usb_dr_mode
 */
extern enum usb_dr_mode usb_get_dr_mode(struct device *dev);

static inline int otg_chrg_vbus(struct usb_otg *otg, int on)
{
	if (!otg->fsm.ops->chrg_vbus)
		return -EOPNOTSUPP;
	otg->fsm.ops->chrg_vbus(otg, on);
	return 0;
}

static inline int otg_drv_vbus(struct usb_otg *otg, int on)
{
	if (!otg->fsm.ops->drv_vbus)
		return -EOPNOTSUPP;
	if (otg->fsm.drv_vbus != on) {
		otg->fsm.drv_vbus = on;
		otg->fsm.ops->drv_vbus(otg, on);
	}
	return 0;
}

static inline int otg_loc_conn(struct usb_otg *otg, int on)
{
	if (!otg->fsm.ops->loc_conn)
		return -EOPNOTSUPP;
	if (otg->fsm.loc_conn != on) {
		otg->fsm.loc_conn = on;
		otg->fsm.ops->loc_conn(otg, on);
	}
	return 0;
}

static inline int otg_loc_sof(struct usb_otg *otg, int on)
{
	if (!otg->fsm.ops->loc_sof)
		return -EOPNOTSUPP;
	if (otg->fsm.loc_sof != on) {
		otg->fsm.loc_sof = on;
		otg->fsm.ops->loc_sof(otg, on);
	}
	return 0;
}

static inline int otg_start_pulse(struct usb_otg *otg)
{
	if (!otg->fsm.ops->start_pulse)
		return -EOPNOTSUPP;
	if (!otg->fsm.data_pulse) {
		otg->fsm.data_pulse = 1;
		otg->fsm.ops->start_pulse(otg);
	}
	return 0;
}

static inline int otg_start_adp_prb(struct usb_otg *otg)
{
	if (!otg->fsm.ops->start_adp_prb)
		return -EOPNOTSUPP;
	if (!otg->fsm.adp_prb) {
		otg->fsm.adp_sns = 0;
		otg->fsm.adp_prb = 1;
		otg->fsm.ops->start_adp_prb(otg);
	}
	return 0;
}

static inline int otg_start_adp_sns(struct usb_otg *otg)
{
	if (!otg->fsm.ops->start_adp_sns)
		return -EOPNOTSUPP;
	if (!otg->fsm.adp_sns) {
		otg->fsm.adp_sns = 1;
		otg->fsm.ops->start_adp_sns(otg);
	}
	return 0;
}

static inline int otg_add_timer(struct usb_otg *otg, enum otg_fsm_timer timer)
{
	if (!otg->fsm.ops->add_timer)
		return -EOPNOTSUPP;
	otg->fsm.ops->add_timer(otg, timer);
	return 0;
}

static inline int otg_del_timer(struct usb_otg *otg, enum otg_fsm_timer timer)
{
	if (!otg->fsm.ops->del_timer)
		return -EOPNOTSUPP;
	otg->fsm.ops->del_timer(otg, timer);
	return 0;
}

static inline int otg_start_host(struct usb_otg *otg, int on)
{
	if (!otg->fsm.ops->start_host)
		return -EOPNOTSUPP;
	return otg->fsm.ops->start_host(otg, on);
}

static inline int otg_start_gadget(struct usb_otg *otg, int on)
{
	if (!otg->fsm.ops->start_gadget)
		return -EOPNOTSUPP;
	return otg->fsm.ops->start_gadget(otg, on);
}

int drd_statemachine(struct usb_otg *otg);

#endif /* __LINUX_USB_OTG_H */
