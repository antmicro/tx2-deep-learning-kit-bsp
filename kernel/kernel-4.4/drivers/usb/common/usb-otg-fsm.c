/*
 * OTG Finite State Machine from OTG spec
 *
 * Copyright (C) 2007,2008 Freescale Semiconductor, Inc.
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author:	Li Yang <LeoLi@freescale.com>
 *		Jerry Huang <Chang-Ming.Huang@freescale.com>
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
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/usb.h>
#include <linux/usb/gadget.h>
#include <linux/usb/otg.h>
#include <linux/usb/otg-fsm.h>

#include "../core/hub.h" /* for usb_hub_to_struct_hub() */

/*
 * state operation which will be called during state transition
 * (note that it will be called after otg_leave_state() is executed)
 */
struct otg_fsm_state_op {
	struct list_head list;
	enum usb_otg_state from_state;
	enum usb_otg_state to_state;
	void (*op)(struct usb_otg *otg);
};

/* Change USB protocol when there is a protocol change */
static int otg_set_protocol(struct otg_fsm *fsm, int protocol)
{
	struct usb_otg *otg = container_of(fsm, struct usb_otg, fsm);
	int ret = 0;

	if (fsm->protocol != protocol) {
		dev_dbg(otg->dev,
			"FSM:Changing role, protocol = %s -> %s\n",
			otg_proto_string(fsm->protocol),
			otg_proto_string(protocol));
		/* stop old protocol */
		if (fsm->protocol == PROTO_HOST)
			ret = otg_start_host(otg, 0);
		else if (fsm->protocol == PROTO_GADGET)
			ret = otg_start_gadget(otg, 0);
		if (ret) {
			dev_info(otg->dev, "FSM:stopping %s failed: %d\n",
				 otg_proto_string(fsm->protocol), ret);
			return ret;
		}

		/* start new protocol */
		if (protocol == PROTO_HOST)
			ret = otg_start_host(otg, 1);
		else if (protocol == PROTO_GADGET)
			ret = otg_start_gadget(otg, 1);
		if (ret) {
			dev_info(otg->dev, "FSM:starting %s failed: %d\n",
				 otg_proto_string(protocol), ret);
			return ret;
		}

		fsm->protocol = protocol;
		return 0;
	}

	return 0;
}

/* Called when leaving a state.  Do state clean up jobs here */
static void otg_leave_state(struct otg_fsm *fsm, enum usb_otg_state old_state)
{
	struct usb_otg *otg = container_of(fsm, struct usb_otg, fsm);

	switch (old_state) {
	case OTG_STATE_B_IDLE:
		otg_del_timer(otg, B_SE0_SRP);
		otg_del_timer(otg, B_SSEND_SRP);
		fsm->b_se0_srp = 0;
		fsm->b_ssend_srp = 0;
		fsm->adp_sns = 0;
		fsm->adp_prb = 0;
		/* B_SRP_FAIL timeout means A-device doesn't respond to SRP */
		fsm->b_srp_done = 0;
		otg_del_timer(otg, B_SRP_FAIL);
		if (otg->gadget)
			otg->gadget->otg_srp_reqd = 0;
		break;
	case OTG_STATE_B_SRP_INIT:
		fsm->data_pulse = 0;
		fsm->b_srp_done = 0;
		break;
	case OTG_STATE_B_PERIPHERAL:
		if (otg->gadget) {
			otg->gadget->host_request_flag = 0;
			otg->gadget->otg_hnp_reqd = 0;
		}
		fsm->b_aidl_bdis_tmout = 0;
		otg_del_timer(otg, B_AIDL_BDIS);
		break;
	case OTG_STATE_B_WAIT_ACON:
		otg_del_timer(otg, B_ASE0_BRST);
		fsm->b_ase0_brst_tmout = 0;
		break;
	case OTG_STATE_B_HOST:
		break;
	case OTG_STATE_A_IDLE:
		fsm->adp_prb = 0;
		/* clear this which is used when test-device is attached */
		fsm->a_bus_drop = 0;
		break;
	case OTG_STATE_A_WAIT_VRISE:
		otg_del_timer(otg, A_WAIT_VRISE);
		fsm->a_wait_vrise_tmout = 0;
		break;
	case OTG_STATE_A_WAIT_BCON:
		otg_del_timer(otg, A_WAIT_BCON);
		fsm->a_wait_bcon_tmout = 0;
		fsm->otg_vbus_off = 0;
		break;
	case OTG_STATE_A_HOST:
		otg_del_timer(otg, A_WAIT_ENUM);
		break;
	case OTG_STATE_A_SUSPEND:
		otg_del_timer(otg, A_AIDL_BDIS);
		fsm->a_aidl_bdis_tmout = 0;
		fsm->a_suspend_req_inf = 0;
		otg_del_timer(otg, TST_MAINT);
		break;
	case OTG_STATE_A_PERIPHERAL:
		otg_del_timer(otg, A_BIDL_ADIS);
		fsm->a_bidl_adis_tmout = 0;
		if (otg->gadget) {
			otg->gadget->host_request_flag = 0;
			otg->gadget->otg_hnp_reqd = 0;
		}
		break;
	case OTG_STATE_A_WAIT_VFALL:
		otg_del_timer(otg, A_WAIT_VFALL);
		fsm->a_wait_vfall_tmout = 0;
		fsm->b_conn = 0; /* clear b_conn */
		fsm->otg_vbus_off = 0;
		break;
	case OTG_STATE_A_VBUS_ERR:
		break;
	default:
		break;
	}
}

static int suspend_usb_device(struct usb_device *udev)
{
	struct usb_hub *hub;

	hub = usb_hub_to_struct_hub(udev->parent);
	if (!hub) {
		dev_err(&udev->dev, "can't get parent hub\n");
		return -EINVAL;
	}

	/* the parameter comes from set_port_feature() in hub.c */
	return usb_control_msg(hub->hdev,
			usb_sndctrlpipe(hub->hdev, 0),
			USB_REQ_SET_FEATURE,
			USB_RT_PORT,
			USB_PORT_FEAT_SUSPEND,
			udev->portnum,
			NULL,
			0,
			1000);
}

static void otg_hnp_polling_work(struct work_struct *work)
{
	struct otg_fsm *fsm = container_of(to_delayed_work(work),
				struct otg_fsm, hnp_polling_work);
	struct usb_otg *otg = container_of(fsm, struct usb_otg, fsm);
	struct usb_device *udev;
	enum usb_otg_state state = otg->state;
	u8 flag;
	int retval;

	if (state != OTG_STATE_A_HOST && state != OTG_STATE_B_HOST)
		return;

	udev = usb_hub_find_child(otg->host->root_hub, otg->host->otg_port);
	/* only do HNP polling when the device is FS or HS */
	if (!udev || (udev && udev->speed == USB_SPEED_LOW)) {
		dev_dbg(otg->host->controller,
			"no FS/HS dev connected, won't start HNP polling\n");
		return;
	}

	/* support for OTG V1.3 */
	if (otg->host->otgv13_hnp && state == OTG_STATE_A_HOST) {
		dev_info(&udev->dev, "otgv13_hnp for PET\n");
		otg->host->otgv13_hnp = 0;
		fsm->a_bus_req = 0;

		/*
		 * make sure we go to A_SUSPEND first. If we don't do this,
		 * the B-device (in this case, the USB-PET) may disconnect D+
		 * as soon as we suspend it (the control transfer below),
		 * causing b_conn=0 and we ended up moving to a_wait_bcon
		 * instead of a_suspend state. This will fail the HNP test.
		 */
		otg_statemachine(otg);

		dev_dbg(otg->host->controller, "suspend port %d\n",
			udev->portnum);
		retval = suspend_usb_device(udev);
		if (retval < 0) {
			dev_err(otg->host->controller,
				"otgv13 suspend port %d failed: %d\n",
				udev->portnum, retval);
			otg->host->otg_quick_hnp = 0;
			fsm->a_bus_req = 1;
		} else {
			otg->host->otg_quick_hnp = 1;
		}

		return;
	}

	*fsm->host_req_flag = 0;
	/* Get host request flag from connected USB device */
	retval = usb_control_msg(udev,
				usb_rcvctrlpipe(udev, 0),
				USB_REQ_GET_STATUS,
				USB_DIR_IN | USB_RECIP_DEVICE,
				0,
				OTG_STS_SELECTOR,
				fsm->host_req_flag,
				1,
				USB_CTRL_GET_TIMEOUT);
	if (retval != 1) {
		/* rewording error message for USB-PET test requirement */
		dev_dbg(&udev->dev, "device doesn't support HNP polling\n");
		return;
	}

	flag = *fsm->host_req_flag;
	if (flag == 0) {
		/* Continue HNP polling */
		schedule_delayed_work(&fsm->hnp_polling_work,
					msecs_to_jiffies(T_HOST_REQ_POLL));
		return;
	} else if (flag != HOST_REQUEST_FLAG) {
		dev_err(&udev->dev, "host request flag %d is invalid\n", flag);
		return;
	}

	/* Host request flag is set */
	if (state == OTG_STATE_A_HOST) {
		/* Set b_hnp_enable */
		if (!otg->host->b_hnp_enable) {
			retval = usb_control_msg(udev,
					usb_sndctrlpipe(udev, 0),
					USB_REQ_SET_FEATURE, 0,
					USB_DEVICE_B_HNP_ENABLE,
					0, NULL, 0,
					USB_CTRL_SET_TIMEOUT);
			if (retval >= 0) {
				otg->host->b_hnp_enable = 1;
			} else {
				dev_err(&udev->dev,
					"Device no response: can't set HNP mode %d\n",
					retval);
				return;
			}
		}
		fsm->a_bus_req = 0;

		/* make sure we go to A_SUSPEND first, same reason as above */
		otg_statemachine(otg);

		dev_dbg(otg->host->controller, "suspend port %d\n",
			udev->portnum);
		retval = suspend_usb_device(udev);
		if (retval < 0)
			dev_err(otg->host->controller,
				"suspend port %d failed: %d\n", udev->portnum,
				retval);
	} else if (state == OTG_STATE_B_HOST) {
		fsm->b_bus_req = 0;
		otg_statemachine(otg);
	}

}

static void otg_start_hnp_polling(struct otg_fsm *fsm)
{
	/*
	 * The memory of host_req_flag should be allocated by
	 * controller driver, otherwise, hnp polling is not started.
	 */
	if (!fsm->host_req_flag)
		return;

	if (!fsm->hnp_polling_initialized) {
		INIT_DELAYED_WORK(&fsm->hnp_polling_work, otg_hnp_polling_work);
		fsm->hnp_polling_initialized = true;
	}
	schedule_delayed_work(&fsm->hnp_polling_work,
					msecs_to_jiffies(T_HOST_REQ_POLL));
}

static inline void debug_state_op(struct otg_fsm_state_op *entry)
{
	pr_debug("[FSM state OP] %s->%s: %pF\n",
		 usb_otg_state_string(entry->from_state),
		 usb_otg_state_string(entry->to_state), entry->op);
}

/* Called when entering a state */
static int otg_set_state(struct otg_fsm *fsm, enum usb_otg_state new_state)
{
	struct usb_otg *otg = container_of(fsm, struct usb_otg, fsm);
	struct list_head *ptr;
	struct otg_fsm_state_op *entry;

	if (otg->state == new_state)
		return 0;
	dev_dbg(otg->dev, "FSM:state: [%s] -> [%s]\n",
			usb_otg_state_string(otg->state),
			usb_otg_state_string(new_state));
	otg_leave_state(fsm, otg->state);

	/* executing extra transition callbacks when doing state transition */
	list_for_each(ptr, &fsm->state_op_list) {
		entry = list_entry(ptr, struct otg_fsm_state_op, list);
		/* if state is UNDEFINED, it means any state */
		if (entry->op &&
		    ((entry->from_state == otg->state &&
			entry->to_state == new_state) ||
		    (entry->from_state == OTG_STATE_UNDEFINED &&
			entry->to_state == new_state) ||
		    (entry->from_state == otg->state &&
			entry->to_state == OTG_STATE_UNDEFINED))) {
			debug_state_op(entry);
			entry->op(otg);
		}
	}

	switch (new_state) {
	case OTG_STATE_B_IDLE:
		/* clear previous HNP request from B-device */
		fsm->b_bus_req = 0;
		otg_drv_vbus(otg, 0);
		otg_chrg_vbus(otg, 0);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		/*
		 * Driver is responsible for starting ADP probing
		 * if ADP sensing times out.
		 */
		otg_start_adp_sns(otg);
		otg_set_protocol(fsm, PROTO_UNDEF);
		otg_add_timer(otg, B_SE0_SRP);
		otg_add_timer(otg, B_SSEND_SRP);
		break;
	case OTG_STATE_B_SRP_INIT:
		otg_start_pulse(otg);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_UNDEF);
		otg_add_timer(otg, B_SRP_FAIL);
		break;
	case OTG_STATE_B_PERIPHERAL:
		otg_chrg_vbus(otg, 0);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_GADGET);
		otg_loc_conn(otg, 1);
		break;
	case OTG_STATE_B_WAIT_ACON:
		otg_chrg_vbus(otg, 0);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_HOST);
		otg_add_timer(otg, B_ASE0_BRST);
		fsm->a_bus_suspend = 0;
		break;
	case OTG_STATE_B_HOST:
		otg_chrg_vbus(otg, 0);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 1);
		otg_set_protocol(fsm, PROTO_HOST);
		usb_bus_start_enum(otg->host, otg->host->otg_port);
		otg_start_hnp_polling(fsm);
		break;
	case OTG_STATE_A_IDLE:
		otg_drv_vbus(otg, 0);
		otg_chrg_vbus(otg, 0);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		otg_start_adp_prb(otg);
		otg_set_protocol(fsm, PROTO_HOST);
		break;
	case OTG_STATE_A_WAIT_VRISE:
		otg_drv_vbus(otg, 1);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_HOST);
		otg_add_timer(otg, A_WAIT_VRISE);
		break;
	case OTG_STATE_A_WAIT_BCON:
		otg_del_timer(otg, TST_MAINT);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_HOST);
		otg_add_timer(otg, A_WAIT_BCON);
		break;
	case OTG_STATE_A_HOST:
		otg_drv_vbus(otg, 1);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 1);
		otg_set_protocol(fsm, PROTO_HOST);
		/*
		 * When HNP is triggered while a_bus_req = 0, a_host will
		 * suspend too fast to complete a_set_b_hnp_en
		 */
		if (!fsm->a_bus_req || fsm->a_suspend_req_inf)
			otg_add_timer(otg, A_WAIT_ENUM);
		otg_start_hnp_polling(fsm);
		break;
	case OTG_STATE_A_SUSPEND:
		otg_drv_vbus(otg, 1);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_HOST);
		otg_add_timer(otg, A_AIDL_BDIS);
		break;
	case OTG_STATE_A_PERIPHERAL:
		fsm->a_bidl_adis_tmout = 0;
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_GADGET);
		otg_drv_vbus(otg, 1);
		otg_loc_conn(otg, 1);
		break;
	case OTG_STATE_A_WAIT_VFALL:
		otg_del_timer(otg, TST_MAINT);
		otg_drv_vbus(otg, 0);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_HOST);
		otg_add_timer(otg, A_WAIT_VFALL);
		break;
	case OTG_STATE_A_VBUS_ERR:
		otg_del_timer(otg, TST_MAINT);
		otg_drv_vbus(otg, 0);
		otg_loc_conn(otg, 0);
		otg_loc_sof(otg, 0);
		otg_set_protocol(fsm, PROTO_UNDEF);
		break;
	default:
		break;
	}

	otg->state = new_state;
	fsm->state_changed = 1;
	return 0;
}

void otg_init_state_op(struct usb_otg *otg)
{
	INIT_LIST_HEAD(&otg->fsm.state_op_list);
}
EXPORT_SYMBOL_GPL(otg_init_state_op);

int otg_set_state_op(struct usb_otg *otg, enum usb_otg_state from,
	enum usb_otg_state to, void (*op)(struct usb_otg *otg))
{
	struct list_head *ptr;
	struct otg_fsm_state_op *entry;

	list_for_each(ptr, &otg->fsm.state_op_list) {
		entry = list_entry(ptr, struct otg_fsm_state_op, list);
		if (entry->from_state == from && entry->to_state == to) {
			entry->op = op;
			debug_state_op(entry);
			return 0;
		}
	}

	/* add to fsm->state_op_list */
	entry = devm_kzalloc(otg->dev, sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return -ENOMEM;

	entry->from_state = from;
	entry->to_state = to;
	entry->op = op;
	debug_state_op(entry);
	list_add_tail(&entry->list, &otg->fsm.state_op_list);

	return 0;
}
EXPORT_SYMBOL_GPL(otg_set_state_op);

const char *otg_proto_string(int proto)
{
	static const char *const names[] = {
		[PROTO_UNDEF] = "undefined",
		[PROTO_HOST] = "host",
		[PROTO_GADGET] = "peripheral",
	};

	if (proto < 0 || proto >= ARRAY_SIZE(names))
		return "undefined";

	return names[proto];
}

const char *otg_fsm_timer_string(enum otg_fsm_timer timer)
{
	static const char *const names[] = {
		[A_WAIT_VRISE] = "A_WAIT_VRISE",
		[A_WAIT_VFALL] = "A_WAIT_VFALL",
		[A_WAIT_BCON] = "A_WAIT_BCON",
		[A_AIDL_BDIS] = "A_AIDL_BDIS",
		[B_ASE0_BRST] = "B_ASE0_BRST",
		[A_BIDL_ADIS] = "A_BIDL_ADIS",
		[B_AIDL_BDIS] = "B_AIDL_BDIS",
		[B_SE0_SRP] = "B_SE0_SRP",
		[B_SRP_FAIL] = "B_SRP_FAIL",
		[A_WAIT_ENUM] = "A_WAIT_ENUM",
		[B_DATA_PLS] = "B_DATA_PLS",
		[B_SSEND_SRP] = "B_SSEND_SRP",
		[TST_MAINT] = "TST_MAINT",
	};

	if (timer < 0 || timer >= ARRAY_SIZE(names))
		return "UNDEFINED";

	return names[timer];
}
EXPORT_SYMBOL_GPL(otg_fsm_timer_string);

/* State change judgement */
int otg_statemachine(struct usb_otg *otg)
{
	enum usb_otg_state state;
	struct otg_fsm *fsm = &otg->fsm;

	mutex_lock(&fsm->lock);

	dev_vdbg(otg->dev, "FSM:start statemachine\n");
	state = otg->state;
	fsm->state_changed = 0;

	switch (state) {
	case OTG_STATE_UNDEFINED:
		dev_vdbg(otg->dev, "FSM: id=%d\n", fsm->id);
		if (fsm->id)
			otg_set_state(fsm, OTG_STATE_B_IDLE);
		else {
			/* do not move on to A-device FSM if host isn't ready */
			if (otg->host)
				otg_set_state(fsm, OTG_STATE_A_IDLE);
			else
				dev_info(otg->dev,
					 "FSM: can't move to a_idle, host isn't ready\n");
		}
		break;
	case OTG_STATE_B_IDLE:
		if (!fsm->id) {
			/* do not move on to A-device FSM if host isn't ready */
			if (otg->host)
				otg_set_state(fsm, OTG_STATE_A_IDLE);
			else
				dev_info(otg->dev,
					 "FSM: can't move to a_idle, host isn't ready\n");
		} else if (fsm->b_sess_vld && otg->gadget)
			otg_set_state(fsm, OTG_STATE_B_PERIPHERAL);
		else if (((otg->gadget && otg->gadget->otg_srp_reqd) ||
			fsm->b_bus_req || fsm->adp_change || fsm->power_up) &&
			fsm->b_ssend_srp && fsm->b_se0_srp)
			otg_set_state(fsm, OTG_STATE_B_SRP_INIT);
		else if (fsm->b_srp_done)
			dev_warn(otg->dev, "A-device doesn't respond to SRP\n");
		break;
	case OTG_STATE_B_SRP_INIT:
		if (!fsm->id || fsm->b_srp_done)
			otg_set_state(fsm, OTG_STATE_B_IDLE);
		break;
	case OTG_STATE_B_PERIPHERAL:
		if (!fsm->id || !fsm->b_sess_vld)
			otg_set_state(fsm, OTG_STATE_B_IDLE);
		else if ((fsm->b_bus_req || otg->gadget->otg_hnp_reqd) &&
			   otg->gadget->b_hnp_enable && fsm->a_bus_suspend &&
			   fsm->b_aidl_bdis_tmout)
			otg_set_state(fsm, OTG_STATE_B_WAIT_ACON);
		else if (fsm->a_bus_suspend && !fsm->b_aidl_bdis_tmout)
			otg_add_timer(otg, B_AIDL_BDIS);
		break;
	case OTG_STATE_B_WAIT_ACON:
		if (fsm->a_conn)
			otg_set_state(fsm, OTG_STATE_B_HOST);
		else if (!fsm->id || !fsm->b_sess_vld)
			otg_set_state(fsm, OTG_STATE_B_IDLE);
		else if (fsm->a_bus_resume || fsm->b_ase0_brst_tmout) {
			if (fsm->b_ase0_brst_tmout)
				dev_warn(otg->dev, "No response from A-device\n");
			fsm->b_ase0_brst_tmout = 0;
			otg_set_state(fsm, OTG_STATE_B_PERIPHERAL);
		}
		break;
	case OTG_STATE_B_HOST:
		if (!fsm->id || !fsm->b_sess_vld)
			otg_set_state(fsm, OTG_STATE_B_IDLE);
		else if (!fsm->b_bus_req || !fsm->a_conn || fsm->test_device)
			otg_set_state(fsm, OTG_STATE_B_PERIPHERAL);
		break;
	case OTG_STATE_A_IDLE:
		if (fsm->id)
			otg_set_state(fsm, OTG_STATE_B_IDLE);
		else if (!fsm->a_bus_drop && (fsm->a_bus_req ||
			  fsm->a_srp_det || fsm->adp_change || fsm->power_up))
			otg_set_state(fsm, OTG_STATE_A_WAIT_VRISE);
		break;
	case OTG_STATE_A_WAIT_VRISE:
		if (fsm->a_vbus_vld)
			otg_set_state(fsm, OTG_STATE_A_WAIT_BCON);
		else if (fsm->id || fsm->a_bus_drop ||
				fsm->a_wait_vrise_tmout)
			otg_set_state(fsm, OTG_STATE_A_WAIT_VFALL);
		break;
	case OTG_STATE_A_WAIT_BCON:
		if (!fsm->a_vbus_vld)
			otg_set_state(fsm, OTG_STATE_A_VBUS_ERR);
		else if (fsm->b_conn)
			otg_set_state(fsm, OTG_STATE_A_HOST);
		else if (fsm->id || fsm->a_bus_drop || fsm->a_wait_bcon_tmout) {
			if (fsm->a_wait_bcon_tmout)
				dev_dbg(otg->dev, "No response from device\n");
			otg_set_state(fsm, OTG_STATE_A_WAIT_VFALL);
		} else if (fsm->otg_vbus_off && !fsm->tst_maint_tmout) {
			otg_set_state(fsm, OTG_STATE_A_WAIT_VFALL);
			otg->host->otg_vbus_off = 0;
			fsm->otg_vbus_off = 0;
		}
		break;
	case OTG_STATE_A_HOST:
		if (fsm->id || fsm->a_bus_drop)
			otg_set_state(fsm, OTG_STATE_A_WAIT_VFALL);
		else if ((!fsm->a_bus_req || fsm->a_suspend_req_inf) &&
				otg->host->b_hnp_enable)
			otg_set_state(fsm, OTG_STATE_A_SUSPEND);
		else if (!fsm->b_conn)
			otg_set_state(fsm, OTG_STATE_A_WAIT_BCON);
		else if (!fsm->a_vbus_vld)
			otg_set_state(fsm, OTG_STATE_A_VBUS_ERR);
		else if (fsm->tst_maint_tmout) {
			/* turn vbus off */
			fsm->a_bus_drop = 1;
			fsm->state_changed = 1;
			fsm->tst_maint_tmout = 0;
			fsm->otg_vbus_off = 0;
			if (otg->host)
				otg->host->otg_vbus_off = 0;
		} else if (fsm->start_tst_maint_timer) {
			otg_add_timer(otg, TST_MAINT);
			fsm->start_tst_maint_timer = 0;
			fsm->tst_maint_tmout = 0;
			if (otg->host && otg->host->otg_vbus_off)
				fsm->otg_vbus_off = 1;
		}
		break;
	case OTG_STATE_A_SUSPEND:
		if (!fsm->b_conn && otg->host->b_hnp_enable)
			otg_set_state(fsm, OTG_STATE_A_PERIPHERAL);
		else if (!fsm->b_conn && !otg->host->b_hnp_enable)
			otg_set_state(fsm, OTG_STATE_A_WAIT_BCON);
		else if (fsm->a_bus_req || fsm->b_bus_resume)
			otg_set_state(fsm, OTG_STATE_A_HOST);
		else if (fsm->id || fsm->a_bus_drop || fsm->a_aidl_bdis_tmout)
			otg_set_state(fsm, OTG_STATE_A_WAIT_VFALL);
		else if (!fsm->a_vbus_vld)
			otg_set_state(fsm, OTG_STATE_A_VBUS_ERR);
		else if (fsm->tst_maint_tmout) {
			/* turn vbus off */
			fsm->a_bus_drop = 1;
			fsm->state_changed = 1;
			fsm->tst_maint_tmout = 0;
		}
		break;
	case OTG_STATE_A_PERIPHERAL:
		if (fsm->id || fsm->a_bus_drop)
			otg_set_state(fsm, OTG_STATE_A_WAIT_VFALL);
		else if (fsm->a_bidl_adis_tmout)
			otg_set_state(fsm, OTG_STATE_A_WAIT_BCON);
		else if (!fsm->a_vbus_vld)
			otg_set_state(fsm, OTG_STATE_A_VBUS_ERR);
		else if (fsm->b_bus_suspend)
			otg_add_timer(otg, A_BIDL_ADIS);
		break;
	case OTG_STATE_A_WAIT_VFALL:
		if (fsm->a_wait_vfall_tmout)
			otg_set_state(fsm, OTG_STATE_A_IDLE);
		break;
	case OTG_STATE_A_VBUS_ERR:
		if (fsm->id || fsm->a_bus_drop || fsm->a_clr_err)
			otg_set_state(fsm, OTG_STATE_A_WAIT_VFALL);
		break;
	default:
		break;
	}
	mutex_unlock(&fsm->lock);

	dev_vdbg(otg->dev, "FSM:quit statemachine (state changed = %d)\n",
		 fsm->state_changed);
	return fsm->state_changed;
}
EXPORT_SYMBOL_GPL(otg_statemachine);
MODULE_LICENSE("GPL");
