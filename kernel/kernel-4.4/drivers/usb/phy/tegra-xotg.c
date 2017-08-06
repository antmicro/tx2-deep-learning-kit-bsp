/*
* tegra-xotg.c - Nvidia XUSB OTG Controller Driver
*
* Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/extcon.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/usb/gadget.h>
#include <linux/usb/of.h>
#include <linux/usb/otg.h>
#include <linux/usb/otg-fsm.h>
#include <linux/workqueue.h>

#include <soc/tegra/xusb.h>

/* OTG timer values : A-Device */

/*
 * wait for VBUS rise
 * Section 4.4, Table 4-1, OTG2.0 [,100]ms
 * a_wait_vrise: section 7.1.2
 * a_wait_vrise_tmr: section 7.4.5.1
 */
#define XOTG_TA_VBUS_RISE	100

/*
 * wait for VBUS fall
 * Table 4-1, OTG2.0
 * a_wait_vfall: section 7.1.7
 * a_wait_vfall_tmr: section: 7.4.5.2: TSSEND_LKG
 */
#define XOTG_TA_WAIT_VFALL	1000

/*
 * wait for B-Connect
 * Section 5.5, Table 5-1, OTG2.0 [1.1,30]s
 * a_wait_bcon: section 7.1.3
 */
#define XOTG_TA_WAIT_BCON	9000

/*
 * A-Idle to B-Disconnect for A-device
 * Section 5.5, Table 5-1, OTG2.0 [200,]ms
 * a_suspend min 200 ms, section 5.2.1
 */
#define XOTG_TA_AIDL_BDIS	250

/*
 * B-Idle to A-Disconnect
 * Section 5.2.1, Table 5-1, OTG2.0 [155,200]ms
 */
#define XOTG_TA_BIDL_ADIS	155

/* OTG timer values : B-Device */

/*
 * A-SE0 to B-Reset
 * Section 5.3.1, Table 5-1, OTG2.0 [155,]ms
 */
#define XOTG_TB_ASE0_BRST	2000

/*
 * SE0 Time Before SRP
 * Section 5.1.2, Table 5-1, OTG2.0 [1,]s
 */
#define XOTG_TB_SE0_SRP		1000

/*
 * SRP Fail Time
 * Section 5.1.6, Table 5-1, OTG2.0 [5,6]s
 */
#define XOTG_TB_SRP_FAIL	5500

/*
 * SSEND time before SRP
 * Section 5.1.2, Table 5-1, OTG2.0
 */
#define XOTG_TB_SSEND_SRP	1600

/*
 * A-Idle to B-Disconnect for B-device
 * Section 5.2.1, Table 5-1, OTG2.0 [4,150]ms
 */
#define XOTG_TB_AIDL_BDIS	4

/*
 * Maintaining configured session on test device
 * Table 5-1
 */
#define XOTG_TA_TST_MAINT	9900

/*
 * Session support is disabled by default or most majority of devices won't
 * be able to initiate SRP in case VBUS gets turned off. This should be
 * enabled before performing USB-PET tests.
 */
static bool session_support;
module_param(session_support, bool, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(session_support, "VBUS session support");

struct tegra_xotg_timer {
	enum otg_fsm_timer id;
	struct hrtimer timer;
	ktime_t timeout;
	int *timeout_bit; /* pointer to variable that is set on timeout */
	struct tegra_xotg *xotg;
};

struct tegra_xotg_soc_config {
	int (*utmi_vbus_power_on)(struct phy *phy);
	int (*utmi_vbus_power_off)(struct phy *phy);
	int (*set_id_override)(struct phy *phy);
	int (*clear_id_override)(struct phy *phy);
	int (*set_vbus_override)(struct phy *phy);
	int (*clear_vbus_override)(struct phy *phy);
	bool (*has_otg_cap)(struct phy *phy);
	int (*set_reverse_id)(struct phy *phy);
	int (*clear_reverse_id)(struct phy *phy);
	int (*generate_srp)(struct phy *phy);
	int (*enable_srp)(struct phy *phy);
	int (*disable_srp)(struct phy *phy);
	bool (*srp_detected)(struct phy *phy);
	int (*enable_otg_int)(struct phy *phy);
	int (*disable_otg_int)(struct phy *phy);
	int (*ack_otg_int)(struct phy *phy);
	int (*get_otg_vbus_id)(struct phy *phy,
			struct tegra_xusb_otg_vbus_id *);
};

struct tegra_xotg {
	struct device *dev;

	const struct tegra_xotg_soc_config *soc_config;

	/* USB OTG controller structure */
	struct usb_otg *otg;
	spinlock_t lock; /* note that there is already a mutex for otg_fsm */

	bool vbus_enabled; /* current vbus state (XOTG SW state) */

	/* OTG port phy */
	struct phy *usb2_phy;
	struct phy *usb3_phy; /* (optional) */

	/* OTG timers for OTG FSM */
	struct tegra_xotg_timer timers[NUM_OTG_FSM_TIMERS];

	/* extcon for ID pin and VBUS */
	struct extcon_dev *id_extcon;
	struct extcon_dev *vbus_extcon;
	struct notifier_block id_extcon_nb;
	struct notifier_block vbus_extcon_nb;

	/* OTG event handler */
	struct notifier_block otg_nb;

	/* USB-PET support */
	bool test_device_enumerated;

	/* A-device HNP state */
	bool a_hnp_active;
};

/* timer callback to set timeout bit and kick FSM */
static enum hrtimer_restart tegra_xotg_timer_timeout(struct hrtimer *data)
{
	struct tegra_xotg_timer *otgtimer;
	struct tegra_xotg *xotg;

	otgtimer = container_of(data, struct tegra_xotg_timer, timer);
	xotg = otgtimer->xotg;

	dev_dbg(xotg->dev, "timer [%s] timeout\n",
		otg_fsm_timer_string(otgtimer->id));
	if (otgtimer->timeout_bit)
		*otgtimer->timeout_bit = 1;

	usb_otg_sync_inputs(xotg->otg);

	return HRTIMER_NORESTART;
}

/* Initialize one OTG timer with callback, timeout and timeout bit */
static void tegra_xotg_timer_init(struct tegra_xotg *xotg,
	enum otg_fsm_timer id, unsigned long expires_ms, int *timeout_bit)
{
	struct tegra_xotg_timer *otgtimer = &xotg->timers[id];
	struct hrtimer *timer = &otgtimer->timer;

	otgtimer->id = id;
	otgtimer->timeout = ms_to_ktime(expires_ms);
	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = tegra_xotg_timer_timeout;

	otgtimer->timeout_bit = timeout_bit;
	otgtimer->xotg = xotg;
}

static void tegra_xotg_init_timers(struct tegra_xotg *xotg)
{
	struct otg_fsm *fsm = &xotg->otg->fsm;

	tegra_xotg_timer_init(xotg, A_WAIT_VRISE, XOTG_TA_VBUS_RISE,
			      &fsm->a_wait_vrise_tmout);
	tegra_xotg_timer_init(xotg, A_WAIT_VFALL, XOTG_TA_WAIT_VFALL,
			      &fsm->a_wait_vfall_tmout);
	tegra_xotg_timer_init(xotg, A_WAIT_BCON, XOTG_TA_WAIT_BCON,
			      &fsm->a_wait_bcon_tmout);
	tegra_xotg_timer_init(xotg, A_AIDL_BDIS, XOTG_TA_AIDL_BDIS,
			      &fsm->a_aidl_bdis_tmout);
	tegra_xotg_timer_init(xotg, A_BIDL_ADIS, XOTG_TA_BIDL_ADIS,
			      &fsm->a_bidl_adis_tmout);
	tegra_xotg_timer_init(xotg, B_ASE0_BRST, XOTG_TB_ASE0_BRST,
			      &fsm->b_ase0_brst_tmout);
	tegra_xotg_timer_init(xotg, B_SE0_SRP, XOTG_TB_SE0_SRP,
			      &fsm->b_se0_srp);
	tegra_xotg_timer_init(xotg, B_SRP_FAIL, XOTG_TB_SRP_FAIL,
			      &fsm->b_srp_done);
	tegra_xotg_timer_init(xotg, B_SSEND_SRP, XOTG_TB_SSEND_SRP,
			      &fsm->b_ssend_srp);
	tegra_xotg_timer_init(xotg, B_AIDL_BDIS, XOTG_TB_AIDL_BDIS,
			      &fsm->b_aidl_bdis_tmout);
	tegra_xotg_timer_init(xotg, TST_MAINT, XOTG_TA_TST_MAINT,
			      &fsm->tst_maint_tmout);
}

static int tegra_xotg_set_vbus(struct tegra_xotg *xotg, bool on)
{
	int ret = 0;

	if (on && !xotg->vbus_enabled) {
		if (xotg->soc_config->utmi_vbus_power_on)
			ret = xotg->soc_config->utmi_vbus_power_on(
					xotg->usb2_phy);
		if (ret) {
			dev_err(xotg->dev, "failed to enable vbus: %d\n", ret);
			return ret;
		}
		dev_dbg(xotg->dev, "OTG port vbus enabled\n");
		xotg->vbus_enabled = true;
	} else if (!on && xotg->vbus_enabled) {
		if (xotg->soc_config->utmi_vbus_power_off)
			ret = xotg->soc_config->utmi_vbus_power_off(
					xotg->usb2_phy);
		if (ret) {
			dev_err(xotg->dev, "failed to disable vbus: %d\n", ret);
			return ret;
		}
		xotg->vbus_enabled = false;
		dev_dbg(xotg->dev, "OTG port vbus disabled\n");
	} else {
		dev_warn(xotg->dev, "vbus already %sabled, not turning %s\n",
				xotg->vbus_enabled ? "en" : "dis",
				on ? "on" : "off");
	}

	return ret;
}

static int tegra_xotg_get_id_from_rid(enum tegra_xusb_vbus_rid rid)
{
	if (rid == VBUS_ID_RID_A || rid == VBUS_ID_RID_GND) {
		/* micro-A end attached */
		return 0;
	} else if (rid == VBUS_ID_RID_B || rid == VBUS_ID_RID_C ||
			rid == VBUS_ID_RID_FLOAT) {
		return 1;
	}
	return 0;
}

static bool __tegra_xotg_update_inputs(struct tegra_xotg *xotg,
		struct tegra_xusb_otg_vbus_id *info, bool only_when_changed)
{
	bool changed = false;
	bool ever_changed = false;
	int id;

	/* IDDIG */
	changed = false;
	if (info->iddig_chg || !only_when_changed) {
		id = tegra_xotg_get_id_from_rid(info->iddig);
		if (info->iddig_chg) {
			dev_dbg(xotg->dev, "IDDIG changed: %u (ID:%d)\n",
					info->iddig, id);
			changed = ever_changed = true;
		}
		if (id)	/* B-device */
			dev_dbg(xotg->dev, "IDDIG -> B_IDLE\n");
		else	/* A-device */
			dev_dbg(xotg->dev, "IDDIG -> A_IDLE\n");
	}

	/* VBUS session valid */
	changed = false;
	if (info->vbus_sess_vld_chg) {
		dev_dbg(xotg->dev, "VBUS_SESS_VLD changed: %u->%u\n",
				!info->vbus_sess_vld, info->vbus_sess_vld);
		changed = ever_changed = true;
	}
	if (changed || !only_when_changed)
		xotg->otg->fsm.b_sess_vld = info->vbus_sess_vld;

	/* VBUS valid */
	changed = false;
	if (info->vbus_vld_chg) {
		dev_dbg(xotg->dev, "VBUS_VLD changed: %u->%u\n",
				!info->vbus_vld, info->vbus_vld);
		changed = ever_changed = true;
	}

	/*
	 * sometimes VBUS_VLD could do 1->0 even when we already enable vbus,
	 * so not setting a_vbus_vld from info->vbus_vld
	 */

	/* VBUS wakeup: not used */
	changed = false;
	if (info->vbus_wakeup_chg) {
		dev_dbg(xotg->dev, "VBUS_WAKEUP changed: %u->%u\n",
				!info->vbus_wakeup, info->vbus_wakeup);
		changed = ever_changed = true;
	}

	usb_otg_sync_inputs(xotg->otg);

	return ever_changed;
}

static bool tegra_xotg_update_inputs(struct tegra_xotg *xotg,
		bool only_when_changed)
{
	struct tegra_xusb_otg_vbus_id info;

	if (xotg->soc_config->get_otg_vbus_id)
		xotg->soc_config->get_otg_vbus_id(xotg->usb2_phy, &info);
	else
		return false;

	return __tegra_xotg_update_inputs(xotg, &info, only_when_changed);
}

static void tegra_xotg_drv_vbus(struct usb_otg *otg, int on)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);

	dev_dbg(xotg->dev, "%s: %s\n", __func__, on ? "on" : "off");

	tegra_xotg_set_vbus(xotg, on);

	xotg->otg->fsm.a_vbus_vld = xotg->vbus_enabled;
	usb_otg_sync_inputs(xotg->otg);
}

static void tegra_xotg_enable_srp(struct usb_otg *otg)
{
	struct tegra_xotg *xotg = dev_get_drvdata(otg->dev);

	dev_dbg(xotg->dev, "enable SRP detection\n");
	if (xotg->soc_config->enable_srp)
		xotg->soc_config->enable_srp(xotg->usb2_phy);
}

static void tegra_xotg_disable_srp(struct usb_otg *otg)
{
	struct tegra_xotg *xotg = dev_get_drvdata(otg->dev);

	dev_dbg(xotg->dev, "disable SRP detection\n");
	if (xotg->soc_config->disable_srp)
		xotg->soc_config->disable_srp(xotg->usb2_phy);
}

static void tegra_xotg_a_hnp_enter(struct usb_otg *otg)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);
	unsigned long flags;

	dev_dbg(xotg->dev, "Enter A-device HNP\n");

	dev_dbg(xotg->dev, "set reverse ID\n");
	if (xotg->soc_config->set_reverse_id)
		xotg->soc_config->set_reverse_id(xotg->usb2_phy);

	dev_dbg(xotg->dev, "set vbus override\n");
	spin_lock_irqsave(&xotg->lock, flags);
	if (xotg->soc_config->set_vbus_override)
		xotg->soc_config->set_vbus_override(xotg->usb2_phy);
	spin_unlock_irqrestore(&xotg->lock, flags);

	if (otg->gadget)
		otg->gadget->is_a_peripheral = 1;

	xotg->a_hnp_active = true;
}

static void tegra_xotg_a_hnp_exit(struct usb_otg *otg)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);
	unsigned long flags;

	dev_dbg(xotg->dev, "Exit A-device HNP\n");

	dev_dbg(xotg->dev, "clear reverse ID\n");
	if (xotg->soc_config->clear_reverse_id)
		xotg->soc_config->clear_reverse_id(xotg->usb2_phy);

	dev_dbg(xotg->dev, "clear vbus override\n");
	spin_lock_irqsave(&xotg->lock, flags);
	if (xotg->soc_config->clear_vbus_override)
		xotg->soc_config->clear_vbus_override(xotg->usb2_phy);
	spin_unlock_irqrestore(&xotg->lock, flags);

	if (otg->gadget)
		otg->gadget->is_a_peripheral = 0;
	/* A-device now wants to use the bus so set this */
	xotg->otg->fsm.a_bus_req = 1;

	/*
	 * Note that we must not set a_hnp_active = false here
	 * because there are state transitions to run after this
	 * callback (a_peripheral->a_wait_bcon or to a_wait_vfall)
	 * before HNP is actually completed.
	 * The actual end of HNP is when the state reaches a_host
	 * again.
	 */
}

static void tegra_xotg_b_hnp_enter(struct usb_otg *otg)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);

	dev_dbg(xotg->dev, "Enter B-device HNP\n");

	dev_dbg(xotg->dev, "set reverse ID\n");
	if (xotg->soc_config->set_reverse_id)
		xotg->soc_config->set_reverse_id(xotg->usb2_phy);

	if (otg->hcd)
		otg->hcd->self.is_b_host = 1;
}

static void tegra_xotg_b_hnp_exit(struct usb_otg *otg)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);

	dev_dbg(xotg->dev, "Exit B-device HNP\n");

	if (otg->gadget)
		otg->gadget->b_hnp_enable = 0;

	dev_dbg(xotg->dev, "clear reverse ID\n");
	if (xotg->soc_config->clear_reverse_id)
		xotg->soc_config->clear_reverse_id(xotg->usb2_phy);

	if (otg->hcd)
		otg->hcd->self.is_b_host = 0;
}

static void tegra_xotg_vbus_session(struct usb_otg *otg)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);

	/* do not turn on VBUS by not moving FSM to a_wait_vrise */
	if (session_support && !xotg->a_hnp_active) {
		dev_dbg(otg->dev,
			"session supported, not moving to a_wait_vrise\n");
		otg->fsm.a_bus_drop = 1;
	}
}

static void tegra_xotg_mark_a_hnp_end(struct usb_otg *otg)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);

	/*
	 * this boolean is used to denote if A-device HNP has ended
	 * (state back to a_host). This is to determine if session
	 * can be ended by A-device when session support is enabled
	 */
	xotg->a_hnp_active = false;
}

static void tegra_xotg_start_pulse(struct usb_otg *otg)
{
	struct device *dev = otg->dev;
	struct tegra_xotg *xotg = dev_get_drvdata(dev);

	dev_dbg(dev, "%s: [%s] generate SRP\n", __func__,
		usb_otg_state_string(otg->state));

	if (xotg->soc_config->generate_srp)
		xotg->soc_config->generate_srp(xotg->usb2_phy);
	otg->fsm.b_srp_done = 1;
	otg->fsm.b_bus_req = 0;

	usb_otg_sync_inputs(otg);
}

static void tegra_xotg_add_timer(struct usb_otg *otg, enum otg_fsm_timer id)
{
	struct tegra_xotg *xotg = dev_get_drvdata(otg->dev);
	struct tegra_xotg_timer *otgtimer = &xotg->timers[id];
	struct hrtimer *timer = &otgtimer->timer;

	/* not defined in spec, ignore it */
	if (id == A_WAIT_ENUM)
		return;

	if (!otg->fsm.running)
		return;

	/* if timer is already active, exit */
	if (hrtimer_active(timer)) {
		dev_err(xotg->dev, "timer [%s] is already running\n",
		otg_fsm_timer_string(id));
		return;
	}

	dev_dbg(xotg->dev, "timer [%s] started\n", otg_fsm_timer_string(id));
	hrtimer_start(timer, otgtimer->timeout, HRTIMER_MODE_REL);
}

static void tegra_xotg_del_timer(struct usb_otg *otg, enum otg_fsm_timer id)
{
	struct tegra_xotg *xotg = dev_get_drvdata(otg->dev);
	struct tegra_xotg_timer *otgtimer = &xotg->timers[id];
	struct hrtimer *timer = &otgtimer->timer;
	int ret;

	/* not defined in spec, ignore it */
	if (id == A_WAIT_ENUM)
		return;

	ret = hrtimer_cancel(timer);
	if (ret != 0)
		dev_dbg(xotg->dev, "timer [%s] cancelled\n",
			otg_fsm_timer_string(id));
}

static int tegra_xotg_start_host(struct usb_otg *otg, int on)
{
	struct device *dev = otg->dev;
	int ret;

	dev_dbg(dev, "host %s\n", on ? "on" : "off");

	ret = usb_otg_start_host(otg, on);
	otg->fsm.power_up = on;

	usb_otg_sync_inputs(otg);

	return ret;
}

static int tegra_xotg_start_gadget(struct usb_otg *otg, int on)
{
	struct device *dev = otg->dev;
	int ret;

	dev_dbg(dev, "gadget %s\n", on ? "on" : "off");

	ret = usb_otg_start_gadget(otg, on);

	if (on) {
		otg->fsm.power_up = 1;
	} else {
		otg->fsm.power_up = 0;
		otg->fsm.a_bus_suspend = 0;
		otg->fsm.a_bus_resume = 0;
	}

	usb_otg_sync_inputs(otg);

	return ret;
}

static int tegra_xotg_event_handler(struct notifier_block *nb,
		unsigned long event, void *src_dev)
{
	struct tegra_xotg *xotg = container_of(nb, struct tegra_xotg,
			otg_nb);
	struct usb_otg *otg = xotg->otg;

	dev_dbg(xotg->dev, "event [%s] from %s, id=%d\n",
			usb_otg_event_string(event),
			dev_name((struct device *)src_dev),
			otg->fsm.id);

	switch (event) {
	case OTG_EVENT_HCD_PORT_SUSPEND:
		/*
		 * don't have to set a_bus_req=0 since this is done in
		 * hnp polling function in usb-otg-fsm.c
		 */
		break;
	case OTG_EVENT_HCD_PORT_RESUME:
		otg->fsm.a_bus_req = 1;
		break;
	case OTG_EVENT_PCD_PORT_CONNECT:
		/* notified by PCD */
		if (otg->fsm.id)
			/* B-peripheral has detected A-host's connection */
			otg->fsm.a_conn = 1;
		else
			/* A-peripheral has detected B-host's connection */
			otg->fsm.b_conn = 1;
		break;
	case OTG_EVENT_PCD_PORT_DISCONNECT:
		/* notified by PCD */
		if (otg->fsm.id)
			/* B-peripheral has detected A-host's disconnection */
			otg->fsm.a_conn = 0;
		else {
			/* A-peripheral has detected B-host's disconnection */
			otg->fsm.b_conn = 0;
			/* force it out of a_peripheral if port disconnects */
			if (otg->state == OTG_STATE_A_PERIPHERAL)
				otg->fsm.a_bidl_adis_tmout = 1;
		}
		/* clear suspend state */
		otg->fsm.a_bus_suspend = 0;
		otg->fsm.b_bus_suspend = 0;
		/* reset a_bus_drop */
		otg->fsm.a_bus_drop = 0;
		break;
	case OTG_EVENT_PCD_PORT_RESUME:
		otg->fsm.a_bus_suspend = 0;
		otg->fsm.a_bus_resume = 1;
		break;
	case OTG_EVENT_PCD_PORT_SUSPEND:
		otg->fsm.a_bus_suspend = 1;
		otg->fsm.a_bus_resume = 0;
		/*
		 * when A-device in A-peripheral state, if PCD port enters
		 * suspend state, set b_bus_suspend so timer A_BIDL_ADIS will
		 * start in OTG FSM. When the timer expires, the A-device
		 * should transition from a_peripheral to a_wait_bcon state
		 */
		if (otg->state == OTG_STATE_A_PERIPHERAL)
			otg->fsm.b_bus_suspend = 1;
		break;
	case OTG_EVENT_HCD_PORT_CONNECT:
		/* notified by HCD (hub.c) */
		if (otg->fsm.id)
			/* B-host has detected A-peripheral's connection */
			otg->fsm.a_conn = 1;
		else
			/* A-host has detected B-peripheral's connection */
			otg->fsm.b_conn = 1;
		break;
	case OTG_EVENT_HCD_PORT_DISCONNECT:
		/* notified by HCD (hub.c) */
		if (otg->fsm.id)
			/* B-host has detected A-peripheral's disconnection */
			otg->fsm.a_conn = 0;
		else
			/* A-host has detected B-peripheral's disconnection */
			otg->fsm.b_conn = 0;
		/* avoid starting TST_MAINT when device is disconnected */
		otg->fsm.start_tst_maint_timer = 0;
		/* reset a-variables */
		otg->fsm.a_bus_req = 1;
		break;
	case OTG_EVENT_HCD_TEST_DEVICE:
		otg->fsm.start_tst_maint_timer = 1;
		xotg->test_device_enumerated = 1;
		break;
	}

	usb_otg_sync_inputs(xotg->otg);

	return NOTIFY_OK;
}

static struct otg_fsm_ops tegra_xotg_fsm_ops = {
	.drv_vbus = tegra_xotg_drv_vbus,
	.start_pulse = tegra_xotg_start_pulse,
	.add_timer = tegra_xotg_add_timer,
	.del_timer = tegra_xotg_del_timer,
	.start_host = tegra_xotg_start_host,
	.start_gadget = tegra_xotg_start_gadget,
};

static void tegra_xotg_work(struct work_struct *work)
{
	struct usb_otg *otg = container_of(work, struct usb_otg, work);

	pm_runtime_get_sync(otg->dev);
	while (otg_statemachine(otg))
		;
	pm_runtime_put_sync(otg->dev);
}

static struct usb_otg_config tegra_xotg_otg_config = {
	.otg_caps = {
		/*
		 * we support rev 3.0, but set rev 2.0 because RSP hasn't
		 * been implemented in this driver as of now
		 * note that if device tree (DT) has specified different
		 * version, the lesser one will be selected
		 * TODO: implement RSP and change to 0x0300
		 */
		.otg_rev = 0x0200,
		/*
		 * note that the following 4 properties can be overwritten by
		 * DT configuration
		 */
		.hnp_support = true,
		.srp_support = true,
		.adp_support = false, /* Nvidia XOTG doesn't support ADP */
		.rsp_support = false, /* TODO: to implement */
	},
	.fsm_ops = &tegra_xotg_fsm_ops,
	.otg_work = &tegra_xotg_work,
};

static irqreturn_t tegra_xotg_irq(int irq, void *data)
{
	struct tegra_xotg *xotg = (struct tegra_xotg *)data;
	bool srp_detected = false;
	unsigned long flags;


	spin_lock_irqsave(&xotg->lock, flags);
	if (xotg->soc_config->srp_detected)
		srp_detected = xotg->soc_config->srp_detected(xotg->usb2_phy);
	/* update FSM inputs according to register state */
	if (!(tegra_xotg_update_inputs(xotg, true) || srp_detected)) {
		dev_vdbg(xotg->dev, "padctl interrupt isn't for XOTG\n");
		spin_unlock_irqrestore(&xotg->lock, flags);
		return IRQ_NONE;
	}

	dev_vdbg(xotg->dev, "XOTG interrupt\n");

	/* handle SRP detection */
	if (srp_detected) {
		dev_info(xotg->dev, "SRP detected\n");
		if (xotg->soc_config->disable_srp)
			xotg->soc_config->disable_srp(xotg->usb2_phy);
		xotg->otg->fsm.a_srp_det = 1;
		xotg->otg->fsm.a_bus_drop = 0;
	} else
		xotg->otg->fsm.a_srp_det = 0;

	/* ack the OTG interrupt */
	if (xotg->soc_config->ack_otg_int)
		xotg->soc_config->ack_otg_int(xotg->usb2_phy);
	spin_unlock_irqrestore(&xotg->lock, flags);

	usb_otg_sync_inputs(xotg->otg);

	return IRQ_HANDLED;
}

static void update_id_state(struct tegra_xotg *xotg)
{
	unsigned long flags;

	spin_lock_irqsave(&xotg->lock, flags);
	if (extcon_get_cable_state_(xotg->id_extcon, EXTCON_USB_HOST)) {
		dev_info(xotg->dev, "%s: ID grounded\n", __func__);
		if (xotg->soc_config->set_id_override)
			xotg->soc_config->set_id_override(xotg->usb2_phy);
		xotg->otg->fsm.id = 0;
	} else {
		dev_info(xotg->dev, "%s: ID floating\n", __func__);
		if (xotg->soc_config->clear_id_override)
			xotg->soc_config->clear_id_override(xotg->usb2_phy);
		xotg->otg->fsm.id = 1;
	}
	spin_unlock_irqrestore(&xotg->lock, flags);
	usb_otg_sync_inputs(xotg->otg);
}

static void update_vbus_state(struct tegra_xotg *xotg)
{
	unsigned long flags;

	spin_lock_irqsave(&xotg->lock, flags);
	if (extcon_get_cable_state_(xotg->vbus_extcon, EXTCON_USB)) {
		dev_info(xotg->dev, "%s: VBUS detected\n", __func__);
		xotg->otg->fsm.b_sess_vld = 1;
	} else {
		dev_info(xotg->dev, "%s: VBUS not detected\n", __func__);
		/*
		 * don't do this again because when host mode is active,
		 * if we clear vbus_override, USB device will disconnect
		 */
		if (xotg->otg->fsm.b_sess_vld &&
				xotg->soc_config->clear_vbus_override)
			xotg->soc_config->clear_vbus_override(xotg->usb2_phy);
		xotg->otg->fsm.b_sess_vld = 0;
	}
	spin_unlock_irqrestore(&xotg->lock, flags);
	usb_otg_sync_inputs(xotg->otg);
}

static void tegra_xotg_update_data_role(struct tegra_xotg *xotg)
{
	dev_dbg(xotg->dev, "sync FSM according to extcon state\n");
	update_id_state(xotg);
	update_vbus_state(xotg);
}

static int tegra_xotg_id_extcon_notifier(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct tegra_xotg *xotg = container_of(nb, struct tegra_xotg,
			id_extcon_nb);

	update_id_state(xotg);

	return NOTIFY_DONE;
}

static int tegra_xotg_vbus_extcon_notifier(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct tegra_xotg *xotg = container_of(nb, struct tegra_xotg,
			vbus_extcon_nb);

	update_vbus_state(xotg);

	return NOTIFY_DONE;
}

static ssize_t otg_state_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_xotg *xotg = platform_get_drvdata(pdev);
	struct otg_fsm *fsm = &xotg->otg->fsm;
	struct usb_hcd *hcd = xotg->otg->hcd;
	struct usb_gadget *gadget = xotg->otg->gadget;
	unsigned size = PAGE_SIZE;
	char *next = buf;
	int t, i;
	struct tegra_xotg_timer *timer;

	mutex_lock(&fsm->lock);

	/* OTG state and driver info */
	t = scnprintf(next, size,
			"OTG state: [%s]\n"
			"OTG port vbus: %s\n"
			"HCD: %s\n"
			"HCD usb_bus otg_port: %d\n"
			"HCD usb_bus is_b_host: %d\n"
			"HCD usb_bus b_hnp_enable: %d\n"
			"HCD usb_bus otg_quick_hnp: %d\n"
			"HCD usb_bus otgv13_hnp: %d\n\n"
			"PCD: %s\n"
			"PCD gadget is_otg: %d\n"
			"PCD gadget is_a_peripheral: %d\n"
			"PCD gadget b_hnp_enable: %d\n"
			"PCD gadget a_hnp_support: %d\n"
			"PCD gadget a_alt_hnp_support: %d\n"
			"PCD gadget hnp_polling_support: %d\n"
			"PCD gadget host_request_flag: %d\n"
			"PCD gadget otg_srp_reqd: %d\n"
			"PCD gadget otg_hnp_reqd: %d\n\n",
			usb_otg_state_string(xotg->otg->state),
			xotg->vbus_enabled ? "on" : "off",
			hcd ? dev_name(hcd->self.controller) : "not available",
			hcd ? hcd->self.otg_port : -1,
			hcd ? hcd->self.is_b_host : -1,
			hcd ? hcd->self.b_hnp_enable : -1,
			hcd ? hcd->self.otg_quick_hnp : -1,
			hcd ? hcd->self.otgv13_hnp : -1,
			gadget ? dev_name(gadget->dev.parent) : "not available",
			gadget ? gadget->is_otg : -1,
			gadget ? gadget->is_a_peripheral : -1,
			gadget ? gadget->b_hnp_enable : -1,
			gadget ? gadget->a_hnp_support : -1,
			gadget ? gadget->a_alt_hnp_support : -1,
			gadget ? gadget->hnp_polling_support : -1,
			gadget ? gadget->host_request_flag : -1,
			gadget ? gadget->otg_srp_reqd : -1,
			gadget ? gadget->otg_hnp_reqd : -1);
	size -= t;
	next += t;

	/* State machine variables */
	t = scnprintf(next, size,
			"Inputs:\n"
			"------------------\n"
			"id: %d\n"
			"adp_change: %d\n"
			"power_up: %d\n"
			"a_srp_det: %d\n"
			"a_vbus_vld: %d\n"
			"b_conn: %d\n"
			"a_bus_resume: %d\n"
			"a_bus_suspend: %d\n"
			"a_conn: %d\n"
			"b_se0_srp: %d\n"
			"b_ssend_srp: %d\n"
			"b_sess_vld: %d\n"
			"test_device: %d\n"
			"a_bus_drop: %d\n"
			"a_bus_req: %d\n"
			"b_bus_req: %d\n"
			"\n"
			"Auxilary inputs:\n"
			"------------------\n"
			"b_bus_suspend: %d\n"
			"\n"
			"Output:\n"
			"------------------\n"
			"drv_vbus: %d\n"
			"loc_conn: %d\n"
			"loc_sof: %d\n"
			"adp_prb: %d\n"
			"adp_sns: %d\n"
			"data_pulse: %d\n"
			"\n"
			"Internal variables:\n"
			"------------------\n"
			"a_set_b_hnp_en: %d\n"
			"b_srp_done: %d\n"
			"b_hnp_enable: %d\n"
			"a_clr_err: %d\n"
			"a_suspend_req_inf: %d\n"
			"otg_vbus_off: %d\n"
			"\n",
			fsm->id,
			fsm->adp_change,
			fsm->power_up,
			fsm->a_srp_det,
			fsm->a_vbus_vld,
			fsm->b_conn,
			fsm->a_bus_resume,
			fsm->a_bus_suspend,
			fsm->a_conn,
			fsm->b_se0_srp,
			fsm->b_ssend_srp,
			fsm->b_sess_vld,
			fsm->test_device,
			fsm->a_bus_drop,
			fsm->a_bus_req,
			fsm->b_bus_req,
			fsm->b_bus_suspend,
			fsm->drv_vbus,
			fsm->loc_conn,
			fsm->loc_sof,
			fsm->adp_prb,
			fsm->adp_sns,
			fsm->data_pulse,
			fsm->a_set_b_hnp_en,
			fsm->b_srp_done,
			fsm->b_hnp_enable,
			fsm->a_clr_err,
			fsm->a_suspend_req_inf,
			fsm->otg_vbus_off);
	size -= t;
	next += t;

	/* Timer */
	t = scnprintf(next, size,
			"Timers:\n"
			"------------------\n");
	size -= t;
	next += t;

	for (i = 0; i < NUM_OTG_FSM_TIMERS; i++) {
		timer = &xotg->timers[i];
		t = scnprintf(next, size,
				"%s (%lld ms): \ttimeout: %d\n",
				otg_fsm_timer_string(i),
				ktime_to_ms(timer->timeout),
				timer->timeout_bit ? *(timer->timeout_bit) : 0);
		size -= t;
		next += t;
	}

	mutex_unlock(&fsm->lock);

	return PAGE_SIZE - size;
}
static DEVICE_ATTR_RO(otg_state);

static int tegra_xotg_start_srp(struct tegra_xotg *xotg)
{
	if (xotg->otg->state != OTG_STATE_B_IDLE) {
		dev_warn(xotg->dev, "%s: skip SRP, state = %s (!=b_idle)\n",
			__func__,
			usb_otg_state_string(xotg->otg->state));
		return -ENODEV;
	}

	dev_dbg(xotg->dev, "%s, state = %s\n", __func__,
			usb_otg_state_string(xotg->otg->state));

	/*
	 * The detailed steps of SRP:
	 *
	 * When acting as A-device:
	 * 1. Test device enumerates (VID=0x1a0a, PID=0x0200),
	 *    usb_enumerate_device_otg() from hub.c sends an OTG event:
	 *    OTG_EVENT_HCD_TEST_DEVICE
	 * 2. XOTG receives it, sets FSM: start_tst_maint_timer=1
	 * 3. OTG FSM in a_host state will start TST_MAINT timer (9.9s)
	 * 4. when TST_MAINT times out it will set FSM: a_bus_drop=1, and
	 *    turn off VBUS.
	 * 5. FSM will do [a_host] -> [a_wait_vfall] -> [a_idle] transition
	 * 6. in [a_idle] state, test device will send SRP data line pulsing
	 * 7. XOTG will receives a padctrl interrupt indicating the SRP is
	 *    detected. XOTG then sets FSM: a_bus_drop=0, a_srp_det=1
	 * 8. FSM will do [a_idle] -> [a_wait_vrise] so VBUS is powered again
	 * 9. FSM will do [a_wait_vrise] -> [a_wait_bcon] and then test device
	 *    is enumerated again.
	 * 10.FSM does [a_wait_bcon] -> [a_host]
	 *
	 * When acting as B-device: (using this function)
	 * 1. Test device enumerates Tegra, XOTG stays at b_peripheral state
	 * 2. A-device (test device) powers off VBUS, we sees PCD_PORT_SUSPEND
	 *    event and then receives extcon cable 0x0 event, saying that
	 *    VBUS is not detected. FSM moves to b_idle state.
	 * 3. Test device asks for triggering SRP. We do so through sysfs which
	 *    will call this function. XOTG sets FSM: b_bus_req=1
	 * 4. FSM will do [b_idle] -> [b_srp_init], and tegra_xotg_start_pulse()
	 *    will be invoked. It also sets FSM: b_srp_done=1
	 * 5. FSM will do [b_srp_init] -> [b_idle], waiting for A-device to
	 *    power on VBUS.
	 * 6. When A-device (test device) powers up, it will enumerates us and
	 *    moves [b_idle] -> [b_peripheral]
	 *
	 *
	 */
	xotg->otg->fsm.b_bus_req = 1;

	usb_otg_sync_inputs(xotg->otg);

	return 0;
}

static ssize_t trigger_srp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_xotg *xotg = platform_get_drvdata(pdev);
	bool trigger = false;

	if (strtobool(buf, &trigger) < 0)
		return -EINVAL;

	if (trigger)
		tegra_xotg_start_srp(xotg);

	return count;
}
static DEVICE_ATTR_WO(trigger_srp);

static int tegra_xotg_start_hnp(struct tegra_xotg *xotg)
{
	/*
	 * The detailed steps of HNP:
	 *
	 *	When a OTG-capable device is connected, host learns about it
	 *	by usb_enumerate_device_otg() by USB_DT_OTG descriptor. When an
	 *	OTG-capable device is connected, host sets host->
	 *	b_hnp_enable=1, and then sends a control transfer:
	 *	USB_REQ_SET_FEATURE with value USB_DEVICE_B_HNP_ENABLE.
	 *	When XUDC receives this set feature, it also sets
	 *	gadget->b_hnp_enable=1.
	 *
	 *	Host now starts doing HNP polling (otg_hnp_polling_work in
	 *	usb-otg-fsm.c) by OTG statemachine in usb-otg-fsm.c too. The
	 *	polling makes use of control transfer to poll the device for
	 *	request type: USB_REQ_GET_STATUS, index: OTG_STS_SELECTOR every
	 *	1.5 seconds.
	 *
	 * Case: B-peripheral (device side) requests host role:
	 *      This case means that B-device with default peripheral role
	 *      wants to become host.
	 *
	 *	1. [B]: user/app on B-device asks for host role
	 *	2. [B]: XOTG sets gadget->host_request_flag=1, FSM: b_bus_req=1
	 *	3. [A]: next HNP polling happens
	 *	4. [B]: when XUDC receives the HNP poll from host
	 *	   (OTG_STS_SELECTOR), since host_request_flag=1, XUDC will
	 *	   respond with HOST_REQUEST_FLAG (defined to be 1)
	 *	5. [A]: host learns about the flag has been set, clear FSM:
	 *	   a_bus_req, which means app on A-device doesn't need to use
	 *	   the bus anymore. Host then suspends the OTG port
	 *	6. [B]: device side receives the PCD_PORT_SUSPEND event, which
	 *	   causes FSM to do [b_peripheral] -> [b_wait_acon] transition
	 *	7. [B]: the state transition will cause XOTG to do
	 *	   xotg_b_hnp_enter() which sets reverse ID
	 *	8. [B]: OTG statemachine then turns off device mode and turns on
	 *	   host mode.
	 *	9. [A]: device has disconnected, so HCD_PORT_DISCONNECT event
	 *	   is received. FSM does [a_host] -> [a_suspend] transition.
	 *	10.[A]: since host->b_hnp_enable=1 and device is disconnected,
	 *	   OTG statemachine does [a_suspend] -> [a_peripheral]
	 *	11.[A]: the state transition will cause XOTG to do
	 *	   xotg_a_hnp_enter() which sets reverse ID and vbus override.
	 *	12.[A]: OTG statemachine then turns off host mode and turns on
	 *	   device mode.
	 *	13.[B]: now it's b_host role. Host mode driver is active, and
	 *	   enumerates the now a_peripheral device.
	 *	14.[B]: when device is enumerated, HCD_PORT_CONNECT event is
	 *	   received, OTG state does [b_wait_acon] -> [b_host].
	 *
	 * Case: A-peripheral (device side) requests host role:
	 *      This case means that A-device which is now in peripheral mode
	 *      (through the above case so it is in a_peripheral state) wants
	 *      to become host again.
	 *
	 *      1. [A]: user/app on A-device asks for host role
	 *      2. [A]: XOTG sets FSM:a_bus_drop=1 to indicate that A-device
	 *         needs to power down the bus.
	 *      3. [A]: FSM does [a_peripheral] -> [a_wait_vfall]
	 *      4. [A]: the state transition will cause XOTG to do
	 *         tegra_xotg_a_hnp_exit() which clears reverse ID and clear
	 *         vbus override. The "clear reverse ID" then results in the
	 *         USB disconnection.
	 *      5. [A]: OTG statemachine then turns off device mode and turns on
	 *         host mode. In a_wait_vfall, A-device also turns off VBUS.
	 *      6. [B]: HCD_PORT_DISCONNECT is received, which causes FSM to do
	 *         [b_host] -> [b_peripheral] transition.
	 *      7. [B]: the state transition will cause XOTG to do
	 *         tegra_xotg_b_hnp_exit() which clears reverse ID.
	 *      8. [B]: OTG statemachine then turns off host mode and turns on
	 *         device mode.
	 *      9. [B]: since A-device turns off VBUS, B-device now gets extcon
	 *	   notification of 0x0 (no cable connected), FSM does:
	 *	   [b_peripheral] -> [b_idle]
	 *	10.[A]: FSM does [a_wait_vfall] -> [a_idle] -> [a_wait_vrise]
	 *	   -> [a_wait_bcon] transitions, which then turns on VBUS on
	 *	   OTG port
	 *	11.[B]: extcon notification of 0x1 (vbus detected), FSM does
	 *	   [b_idle] -> [b_peripheral] transition
	 *	12.[A]: A-device now enumerates B-device. HCD_PORT_CONNECT event
	 *	   is received, OTG state does [a_wait_bcon] -> [a_host].
	 *
	 * Case: B-host (host side) requests peripheral role:
	 *      This case means that after the initial HNP, A becomes peripheral
	 *      and B becomes host. When B (now as host) doesn't want to be host
	 *      anymore, it requests peripheral role.
	 *
	 *      1. [B]: user/app on B-device now is done with host mode. It
	 *         requests to become peripheral role again.
	 *      2. [B]: XOTG sets FSM:b_bus_req=0, indicating that it doesn't
	 *         want to use the bus anymore.
	 *      3. [B]: FSM does [b_host] -> [b_peripheral]
	 *      4. [B]: the state transition will cause XOTG to do
	 *         tegra_xotg_b_hnp_exit() which clears reverse ID. The "clear
	 *         reverse ID" then results in the USB disconnection.
	 *      5. [B]: OTG statemachine then turns off host mode and turns on
	 *         device mode.
	 *      6. [A]: OTG_EVENT_PCD_PORT_SUSPEND is received due to B-device
	 *         clears reverse ID. XOTG sets FSM:b_bus_suspend=1, which
	 *         causes FSM to start A_BIDL_ADIS timer. It is used by A-device
	 *         to determine when the B-device has finished being host.
	 *      7. [A]: A_BIDL_ADIS times out (XOTG_TA_BIDL_ADIS: 155ms). FSM
	 *         does [a_peripheral] -> [a_wait_bcon].
	 *      8. [A]: the state transition will cause XOTG to do
	 *         tegra_xotg_a_hnp_exit() which clears reverse ID and clear
	 *         vbus override. The "clear reverse ID" then cause peripheral
	 *         side to disconnect. (PCD_PORT_DISCONNECT is received).
	 *      9. [A]: OTG statemachine then turns off device mode and turns on
	 *         host mode.
	 *	10.[A]: A-device now enumerates B-device. HCD_PORT_CONNECT event
	 *	   is received, OTG state does [b_wait_acon] -> [b_host].
	 *
	 */
	switch (xotg->otg->state) {
	case OTG_STATE_B_PERIPHERAL:
		dev_info(xotg->dev, "[%s] B-peripheral requests host role\n",
				__func__);
		if (xotg->otg->gadget)
			xotg->otg->gadget->host_request_flag = 1;
		xotg->otg->fsm.b_bus_req = 1;
		break;
	case OTG_STATE_A_PERIPHERAL:
		dev_info(xotg->dev, "[%s] A-peripheral requests host role\n",
				__func__);
		/* reset to 0 in PCD_PORT_DISCONNECT */
		xotg->otg->fsm.a_bus_drop = 1;
		break;
	case OTG_STATE_B_HOST:
		dev_info(xotg->dev, "[%s] B-host requests peripheral role\n",
				__func__);
		/* set back to 1 in HCD_PORT_DISCONNECT */
		xotg->otg->fsm.b_bus_req = 0;
		break;
	default:
		dev_info(xotg->dev, "[%s] ignore HNP request\n",
				usb_otg_state_string(xotg->otg->state));
	}

	usb_otg_sync_inputs(xotg->otg);

	return 0;
}

static ssize_t trigger_hnp_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_xotg *xotg = platform_get_drvdata(pdev);
	bool trigger = false;

	if (strtobool(buf, &trigger) < 0)
		return -EINVAL;

	if (trigger) {
		dev_info(xotg->dev, "trigger HNP...\n");
		tegra_xotg_start_hnp(xotg);
	}

	return count;
}
static DEVICE_ATTR_WO(trigger_hnp);

static struct attribute *tegra_xotg_attrs[] = {
	&dev_attr_otg_state.attr,
	&dev_attr_trigger_srp.attr,
	&dev_attr_trigger_hnp.attr,
	NULL,
};

static struct attribute_group tegra_xotg_attr_group = {
	.attrs = tegra_xotg_attrs,
};

static const struct tegra_xotg_soc_config tegra186_soc_config = {
	.utmi_vbus_power_on = tegra18x_phy_xusb_utmi_vbus_power_on,
	.utmi_vbus_power_off = tegra18x_phy_xusb_utmi_vbus_power_off,
	.set_id_override = tegra18x_phy_xusb_set_id_override,
	.clear_id_override = tegra18x_phy_xusb_clear_id_override,
	.set_vbus_override = tegra18x_phy_xusb_set_vbus_override,
	.clear_vbus_override = tegra18x_phy_xusb_clear_vbus_override,
	.has_otg_cap = tegra18x_phy_xusb_has_otg_cap,
	.set_reverse_id = tegra18x_phy_xusb_set_reverse_id,
	.clear_reverse_id = tegra18x_phy_xusb_clear_reverse_id,
	.generate_srp = tegra18x_phy_xusb_generate_srp,
	.enable_srp = tegra18x_phy_xusb_enable_srp_detect,
	.disable_srp = tegra18x_phy_xusb_disable_srp_detect,
	.srp_detected = tegra18x_phy_xusb_srp_detected,
	.enable_otg_int = tegra18x_phy_xusb_enable_otg_int,
	.disable_otg_int = tegra18x_phy_xusb_disable_otg_int,
	.ack_otg_int = tegra18x_phy_xusb_ack_otg_int,
	.get_otg_vbus_id = tegra18x_phy_xusb_get_otg_vbus_id,
};

static const struct of_device_id tegra_xotg_of_match[] = {
	{ .compatible = "nvidia,tegra186-xotg", .data = &tegra186_soc_config },
	{}
};
MODULE_DEVICE_TABLE(of, tegra_xotg_of_match);

static int tegra_xotg_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct tegra_xotg *xotg;
	int err;
	int irq;

	xotg = devm_kzalloc(&pdev->dev, sizeof(*xotg), GFP_KERNEL);
	if (!xotg)
		return -ENOMEM;
	xotg->dev = &pdev->dev;
	platform_set_drvdata(pdev, xotg);

	match = of_match_device(tegra_xotg_of_match, &pdev->dev);
	if (!match) {
		dev_warn(xotg->dev, "of doesn't match\n");
		return -ENODEV;
	}
	xotg->soc_config = match->data;

	spin_lock_init(&xotg->lock);

	/* shared padctl IRQ */
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(xotg->dev, "failed to get irq resource 0: %d\n", irq);
		return irq;
	}
	err = devm_request_threaded_irq(&pdev->dev, irq, NULL,
			tegra_xotg_irq,
			IRQF_ONESHOT | IRQF_TRIGGER_HIGH | IRQF_SHARED,
			dev_name(&pdev->dev), xotg);
	if (err < 0) {
		dev_err(xotg->dev, "failed to claim irq %d\n", err);
		return err;
	}

	/* OTG port phy */
	xotg->usb2_phy = devm_phy_get(&pdev->dev, "otg-usb2");
	if (IS_ERR(xotg->usb2_phy)) {
		err = PTR_ERR(xotg->usb2_phy);
		if (err == -EPROBE_DEFER)
			dev_info(xotg->dev, "usb2 phy is not available yet\n");
		else
			dev_err(xotg->dev, "failed to get usb2 phy %d\n", err);
		return err;
	}
	if (xotg->soc_config->has_otg_cap &&
			!xotg->soc_config->has_otg_cap(xotg->usb2_phy)) {
		err = -EINVAL;
		dev_err(xotg->dev, "usb2 phy doesn't have OTG capability\n");
		return err;
	}
	xotg->usb3_phy = devm_phy_optional_get(&pdev->dev, "otg-usb3");
	if (IS_ERR(xotg->usb3_phy)) {
		err = PTR_ERR(xotg->usb3_phy);
		if (err == -EPROBE_DEFER)
			dev_info(xotg->dev, "usb3 phy is not available yet\n");
		else
			dev_err(xotg->dev, "failed to get usb3 phy %d\n", err);
		return err;
	} else if (xotg->usb3_phy && xotg->soc_config->has_otg_cap &&
			!xotg->soc_config->has_otg_cap(xotg->usb3_phy)) {
		err = -EINVAL;
		dev_err(xotg->dev, "usb3 phy doesn't have OTG capability\n");
		return err;
	}

	/* ID extcon initialization */
	xotg->id_extcon = extcon_get_extcon_dev_by_cable(&pdev->dev, "id");
	if (!IS_ERR(xotg->id_extcon)) {
		xotg->id_extcon_nb.notifier_call =
			tegra_xotg_id_extcon_notifier;
		extcon_register_notifier(xotg->id_extcon, EXTCON_USB_HOST,
					 &xotg->id_extcon_nb);
	} else if (PTR_ERR(xotg->id_extcon) == -EPROBE_DEFER) {
		err = -EPROBE_DEFER;
		goto exit;
	} else
		dev_warn(&pdev->dev, "no USB ID extcon found\n");

	/* vbus extcon initialization */
	xotg->vbus_extcon = extcon_get_extcon_dev_by_cable(&pdev->dev, "vbus");
	if (!IS_ERR(xotg->vbus_extcon)) {
		xotg->vbus_extcon_nb.notifier_call =
			tegra_xotg_vbus_extcon_notifier;
		extcon_register_notifier(xotg->vbus_extcon, EXTCON_USB,
					 &xotg->vbus_extcon_nb);
	} else if (PTR_ERR(xotg->vbus_extcon) == -EPROBE_DEFER) {
		err = -EPROBE_DEFER;
		goto unregister_id_extcon;
	} else
		dev_warn(&pdev->dev, "no USB VBUS extcon found\n");

	dev_info(&pdev->dev, "OTG rev:%04x, ADP:%d, SRP:%d, HNP:%d, RSP:%d\n",
			tegra_xotg_otg_config.otg_caps.otg_rev,
			tegra_xotg_otg_config.otg_caps.adp_support,
			tegra_xotg_otg_config.otg_caps.srp_support,
			tegra_xotg_otg_config.otg_caps.hnp_support,
			tegra_xotg_otg_config.otg_caps.rsp_support);

	/* register to OTG Core */
	xotg->otg = usb_otg_register(xotg->dev, &tegra_xotg_otg_config);
	if (IS_ERR(xotg->otg)) {
		err = PTR_ERR(xotg->otg);
		if (err == -ENOTSUPP)
			dev_err(xotg->dev, "CONFIG_USB_OTG needed for XOTG\n");
		else
			dev_err(xotg->dev,
				"failed to register to OTG core: %d\n", err);
		goto unregister_vbus_extcon;
	}

	/* init otg timers */
	tegra_xotg_init_timers(xotg);

	/* register OTG event handler */
	xotg->otg_nb.notifier_call = tegra_xotg_event_handler;
	usb_otg_register_notifier(xotg->otg, &xotg->otg_nb);

	/* sync FSM initial state from extcon cable state */
	tegra_xotg_update_data_role(xotg);
	dev_dbg(xotg->dev, "initial state: id=%d, b_sess_vld=%d\n",
			xotg->otg->fsm.id, xotg->otg->fsm.b_sess_vld);

	/* allocate fsm.host_req_flag so HNP polling will start */
	xotg->otg->fsm.host_req_flag = devm_kzalloc(&pdev->dev, sizeof(u8),
						    GFP_KERNEL);
	/* set initial FSM variables */
	xotg->otg->fsm.a_bus_drop = 0;
	xotg->otg->fsm.a_bus_req = 1;
	usb_otg_sync_inputs(xotg->otg);

	/* initialize OTG FSM state transition operation */
	otg_init_state_op(xotg->otg);

	/* for SRP: only enable SRP detection in A_IDLE state */
	otg_set_state_op(xotg->otg,
			OTG_STATE_UNDEFINED, OTG_STATE_A_IDLE,
			tegra_xotg_enable_srp);
	otg_set_state_op(xotg->otg,
			OTG_STATE_A_IDLE, OTG_STATE_UNDEFINED,
			tegra_xotg_disable_srp);
	/* for A-device HNP */
	otg_set_state_op(xotg->otg,
			OTG_STATE_A_SUSPEND, OTG_STATE_A_PERIPHERAL,
			tegra_xotg_a_hnp_enter);
	otg_set_state_op(xotg->otg,
			OTG_STATE_A_PERIPHERAL, OTG_STATE_UNDEFINED,
			tegra_xotg_a_hnp_exit);
	/* for B-device HNP */
	otg_set_state_op(xotg->otg,
			OTG_STATE_B_PERIPHERAL, OTG_STATE_B_WAIT_ACON,
			tegra_xotg_b_hnp_enter);
	otg_set_state_op(xotg->otg,
			OTG_STATE_B_WAIT_ACON, OTG_STATE_B_PERIPHERAL,
			tegra_xotg_b_hnp_exit);
	otg_set_state_op(xotg->otg,
			OTG_STATE_B_WAIT_ACON, OTG_STATE_B_IDLE,
			tegra_xotg_b_hnp_exit);
	otg_set_state_op(xotg->otg,
			OTG_STATE_B_HOST, OTG_STATE_B_PERIPHERAL,
			tegra_xotg_b_hnp_exit);
	otg_set_state_op(xotg->otg,
			OTG_STATE_B_HOST, OTG_STATE_B_IDLE,
			tegra_xotg_b_hnp_exit);

	/* for session support */
	otg_set_state_op(xotg->otg,
			OTG_STATE_A_WAIT_VFALL, OTG_STATE_A_IDLE,
			tegra_xotg_vbus_session);
	otg_set_state_op(xotg->otg,
			OTG_STATE_UNDEFINED, OTG_STATE_A_HOST,
			tegra_xotg_mark_a_hnp_end);


	/* enable OTG interrupts */
	if (xotg->soc_config->enable_otg_int) {
		err = xotg->soc_config->enable_otg_int(xotg->usb2_phy);
		if (err) {
			dev_err(xotg->dev, "failed to enable OTG interrupt: %d\n",
				err);
			goto unregister_otg;
		}
	}

	/* sysfs nodes */
	err = sysfs_create_group(&pdev->dev.kobj, &tegra_xotg_attr_group);
	if (err) {
		dev_err(xotg->dev, "cannot create sysfs group: %d\n", err);
		goto disable_otg_int;
	}

	dev_info(xotg->dev, "Nvidia XUSB OTG Controller\n");

	return 0;

disable_otg_int:
	if (xotg->soc_config->disable_otg_int)
		xotg->soc_config->disable_otg_int(xotg->usb2_phy);
unregister_otg:
	usb_otg_unregister_notifier(xotg->otg, &xotg->otg_nb);
	usb_otg_unregister(xotg->dev);
unregister_vbus_extcon:
	extcon_unregister_notifier(xotg->vbus_extcon, EXTCON_USB,
				   &xotg->vbus_extcon_nb);
unregister_id_extcon:
	extcon_unregister_notifier(xotg->id_extcon, EXTCON_USB_HOST,
				   &xotg->id_extcon_nb);
exit:
	return err;
}

static int tegra_xotg_remove(struct platform_device *pdev)
{
	struct tegra_xotg *xotg = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &tegra_xotg_attr_group);
	if (xotg->soc_config->disable_srp)
		xotg->soc_config->disable_srp(xotg->usb2_phy);
	if (xotg->soc_config->disable_otg_int)
		xotg->soc_config->disable_otg_int(xotg->usb2_phy);
	usb_otg_unregister_notifier(xotg->otg, &xotg->otg_nb);
	usb_otg_unregister(xotg->dev);
	extcon_unregister_notifier(xotg->vbus_extcon, EXTCON_USB,
				   &xotg->vbus_extcon_nb);
	extcon_unregister_notifier(xotg->id_extcon, EXTCON_USB_HOST,
				   &xotg->id_extcon_nb);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int tegra_xotg_resume(struct device *dev)
{
	struct tegra_xotg *xotg = dev_get_drvdata(dev);
	struct otg_fsm *fsm = &xotg->otg->fsm;

	/* update FSM according to cable state (extcon) */
	tegra_xotg_update_data_role(xotg);

	/* re-start host or gadget based on current role */
	if (fsm->protocol == PROTO_HOST)
		otg_start_host(xotg->otg, 1);
	else if (fsm->protocol == PROTO_GADGET)
		otg_start_gadget(xotg->otg, 1);

	/*
	 * if we resume and find ourselves still in a_peripheral state,
	 * force a FSM reset to a_wait_bcon state
	 */
	if (xotg->otg->state == OTG_STATE_A_PERIPHERAL) {
		xotg->otg->fsm.a_bidl_adis_tmout = 1;
		usb_otg_sync_inputs(xotg->otg);
	}

	return 0;
}

static int tegra_xotg_suspend(struct device *dev)
{
	return 0;
}
#endif

static const struct dev_pm_ops tegra_xotg_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(tegra_xotg_suspend, tegra_xotg_resume)
};

static struct platform_driver tegra_xotg_driver = {
	.probe = tegra_xotg_probe,
	.remove = tegra_xotg_remove,
	.driver = {
		.name = "tegra-xotg",
		.pm = &tegra_xotg_pm_ops,
		.of_match_table = tegra_xotg_of_match,
	},
};
module_platform_driver(tegra_xotg_driver);

MODULE_DESCRIPTION("Nvidia XUSB OTG Controller Driver");
MODULE_AUTHOR("Mark Kuo (mkuo@nvidia.com)");
MODULE_LICENSE("GPL");
