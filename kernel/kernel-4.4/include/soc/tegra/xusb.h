/*
 * Copyright (C) 2014-2017, NVIDIA Corporation.  All rights reserved.
 * Copyright (C) 2014 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#ifndef __SOC_TEGRA_XUSB_H__
#define __SOC_TEGRA_XUSB_H__

#include <linux/usb/ch9.h>

/* Command requests from the firmware */
enum tegra_xusb_mbox_cmd {
	MBOX_CMD_MSG_ENABLED = 1,
	MBOX_CMD_INC_FALC_CLOCK,
	MBOX_CMD_DEC_FALC_CLOCK,
	MBOX_CMD_INC_SSPI_CLOCK,
	MBOX_CMD_DEC_SSPI_CLOCK,
	MBOX_CMD_SET_BW, /* no ACK/NAK required */
	MBOX_CMD_SET_SS_PWR_GATING,
	MBOX_CMD_SET_SS_PWR_UNGATING,
	MBOX_CMD_SAVE_DFE_CTLE_CTX,
	MBOX_CMD_AIRPLANE_MODE_ENABLED, /* unused */
	MBOX_CMD_AIRPLANE_MODE_DISABLED, /* unused */
	MBOX_CMD_START_HSIC_IDLE,
	MBOX_CMD_STOP_HSIC_IDLE,
	MBOX_CMD_DBC_WAKE_STACK, /* unused */
	MBOX_CMD_HSIC_PRETEND_CONNECT,
	MBOX_CMD_RESET_SSPI,
	MBOX_CMD_DISABLE_SS_LFPS_DETECTION,
	MBOX_CMD_ENABLE_SS_LFPS_DETECTION,
	MBOX_CMD_SS_PORT_IN_POLLING,
	MBOX_CMD_MAX,

	/* Response message to above commands */
	MBOX_CMD_ACK = 128,
	MBOX_CMD_NAK,
	MBOX_CMD_COMPL /* mailbox completed without sending ACK/NAK */
};

struct tegra_xusb_mbox_msg {
	u32 cmd;
	u32 data;
};

/*
 * Tegra OTG port VBUS direction:
 * default (based on port capability) or
 * as source or sink
 */
enum tegra_vbus_dir {
	TEGRA_VBUS_DEFAULT,
	TEGRA_VBUS_SOURCE,
	TEGRA_VBUS_SINK
};

/* RID status from IDDIG bits in XUSB_PADCTL_USB2_VBUS_ID_0 register */
enum tegra_xusb_vbus_rid {
	VBUS_ID_RID_GND,
	VBUS_ID_RID_FLOAT,
	VBUS_ID_RID_A,
	VBUS_ID_RID_B,
	VBUS_ID_RID_C,
	VBUS_ID_RID_UNDEFINED
};

/* OTG status change details from XUSB_PADCTL_USB2_VBUS_ID_0 register */
struct tegra_xusb_otg_vbus_id {
	bool vbus_sess_vld_chg; /* bit 1 */
	unsigned vbus_sess_vld:1; /* bit 0 */

	bool vbus_vld_chg; /* bit 4 */
	unsigned vbus_vld:1; /* bit 3 */

	bool iddig_chg; /* bit 10 */
	enum tegra_xusb_vbus_rid iddig; /* from bit [9:6] */

	bool vbus_wakeup_chg; /* bit 23 */
	unsigned vbus_wakeup:1; /* bit 22 */

	unsigned vbus_override:1; /* bit 14 */
	unsigned id_override:4; /* bit [21:18] */
};

struct phy;

#ifdef CONFIG_PINCTRL_TEGRA21x_PADCTL_UPHY
int tegra21x_phy_xusb_set_vbus_override(struct phy *phy);
int tegra21x_phy_xusb_clear_vbus_override(struct phy *phy);
void tegra21x_phy_xusb_utmi_pad_power_on(struct phy *phy);
void tegra21x_phy_xusb_utmi_pad_power_down(struct phy *phy);
void tegra21x_phy_xusb_set_dcd_debounce_time(struct phy *phy, u32 val);
void tegra21x_phy_xusb_utmi_pad_charger_detect_on(struct phy *phy);
void tegra21x_phy_xusb_utmi_pad_charger_detect_off(struct phy *phy);
void tegra21x_phy_xusb_utmi_pad_enable_detect_filters(struct phy *phy);
void tegra21x_phy_xusb_utmi_pad_disable_detect_filters(struct phy *phy);
bool tegra21x_phy_xusb_utmi_pad_primary_charger_detect(struct phy *phy);
bool tegra21x_phy_xusb_utmi_pad_secondary_charger_detect(struct phy *phy);
/* data contact detection */
bool tegra21x_phy_xusb_utmi_pad_dcd(struct phy *phy);
/* level < 0: disable protection */
void tegra21x_phy_xusb_utmi_pad_set_protection_level(struct phy *phy, int level,
						  enum tegra_vbus_dir dir);
u32 tegra21x_phy_xusb_noncompliant_div_detect(struct phy *phy);
int tegra21x_phy_xusb_utmi_vbus_power_on(struct phy *phy);
int tegra21x_phy_xusb_utmi_vbus_power_off(struct phy *phy);
int tegra21x_utmi_vbus_enable(struct phy *phy);
int tegra21x_phy_xusb_overcurrent_detected(struct phy *phy);
void tegra21x_phy_xusb_handle_overcurrent(struct phy *phy);
int tegra21x_phy_xusb_set_id_override(struct phy *phy);
int tegra21x_phy_xusb_clear_id_override(struct phy *phy);
bool tegra21x_phy_xusb_has_otg_cap(struct phy *phy);
int tegra21x_phy_xusb_enable_sleepwalk(struct phy *phy,
				    enum usb_device_speed speed);
int tegra21x_phy_xusb_disable_sleepwalk(struct phy *phy);
int tegra21x_phy_xusb_enable_wake(struct phy *phy);
int tegra21x_phy_xusb_disable_wake(struct phy *phy);
int tegra21x_phy_xusb_pretend_connected(struct phy *phy);

/* tegra_phy_xusb_remote_wake_detected()
 *   check if a XUSB phy has detected remote wake.
 *   return values:
 *         0: no
 *       > 0: yes
 *       < 0: error
 */
int tegra21x_phy_xusb_remote_wake_detected(struct phy *phy);
int tegra21x_phy_xusb_set_reverse_id(struct phy *phy);
int tegra21x_phy_xusb_clear_reverse_id(struct phy *phy);
int tegra21x_phy_xusb_generate_srp(struct phy *phy);
int tegra21x_phy_xusb_enable_srp_detect(struct phy *phy);
int tegra21x_phy_xusb_disable_srp_detect(struct phy *phy);
bool tegra21x_phy_xusb_srp_detected(struct phy *phy);
int tegra21x_phy_xusb_enable_otg_int(struct phy *phy);
int tegra21x_phy_xusb_disable_otg_int(struct phy *phy);
int tegra21x_phy_xusb_ack_otg_int(struct phy *phy);
int tegra21x_phy_xusb_get_otg_vbus_id(struct phy *phy,
		struct tegra_xusb_otg_vbus_id *change);
void t210_clamp_en_early(struct phy *phy, bool on);
void t210_receiver_detector(struct phy *phy, bool on);
#else
static inline int tegra21x_phy_xusb_set_vbus_override(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_clear_vbus_override(struct phy *phy)
{
	return 0;
}

static inline void tegra21x_phy_xusb_utmi_pad_power_on(struct phy *phy)
{
}

static inline void tegra21x_phy_xusb_utmi_pad_power_down(struct phy *phy)
{
}
static inline void tegra21x_phy_xusb_set_dcd_debounce_time(struct phy *phy,
								u32 val)
{
}

static inline void tegra21x_phy_xusb_utmi_pad_charger_detect_on(struct phy *phy)
{
}

static inline void tegra21x_phy_xusb_utmi_pad_charger_detect_off
							(struct phy *phy)
{
}

static inline void tegra21x_phy_xusb_utmi_pad_enable_detect_filters
							(struct phy *phy)
{
}

static inline void tegra21x_phy_xusb_utmi_pad_disable_detect_filters
							(struct phy *phy)
{
}

static inline bool tegra21x_phy_xusb_utmi_pad_primary_charger_detect
							(struct phy *phy)
{
	return 0;
}

static inline bool tegra21x_phy_xusb_utmi_pad_secondary_charger_detect
							(struct phy *phy)
{
	return 0;
}

/* data contact detection */
static inline bool tegra21x_phy_xusb_utmi_pad_dcd(struct phy *phy)
{
	return 0;
}

/* level < 0: disable protection */
static inline void tegra21x_phy_xusb_utmi_pad_set_protection_level
						(struct phy *phy, int level,
						enum tegra_vbus_dir dir)
{
}

static inline u32 tegra21x_phy_xusb_noncompliant_div_detect(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_utmi_vbus_power_on(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_utmi_vbus_power_off(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_utmi_vbus_enable(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_overcurrent_detected(struct phy *phy)
{
	return 0;
}

static inline void tegra21x_phy_xusb_handle_overcurrent(struct phy *phy)
{
}

static inline int tegra21x_phy_xusb_set_id_override(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_clear_id_override(struct phy *phy)
{
	return 0;
}

static inline bool tegra21x_phy_xusb_has_otg_cap(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_enable_sleepwalk(struct phy *phy,
				    enum usb_device_speed speed)
{
	return 0;
}

static inline int tegra21x_phy_xusb_disable_sleepwalk(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_enable_wake(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_disable_wake(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_pretend_connected(struct phy *phy)
{
	return 0;
}

/* tegra_phy_xusb_remote_wake_detected()
 *   check if a XUSB phy has detected remote wake.
 *   return values:
 *         0: no
 *       > 0: yes
 *       < 0: error
 */
static inline int tegra21x_phy_xusb_remote_wake_detected(struct phy *phy)
{
	return 0;
}
static inline int tegra21x_phy_xusb_set_reverse_id(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_clear_reverse_id(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_generate_srp(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_enable_srp_detect(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_disable_srp_detect(struct phy *phy)
{
	return 0;
}

static inline bool tegra21x_phy_xusb_srp_detected(struct phy *phy)
{
	return false;
}

static inline int tegra21x_phy_xusb_enable_otg_int(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_disable_otg_int(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_ack_otg_int(struct phy *phy)
{
	return 0;
}

static inline int tegra21x_phy_xusb_get_otg_vbus_id(struct phy *phy,
		struct tegra_xusb_otg_vbus_id *change)
{
	return 0;
}

static inline void t210_clamp_en_early(struct phy *phy, bool on)
{
}

static inline void t210_receiver_detector(struct phy *phy, bool on)
{
}
#endif

#ifdef CONFIG_PINCTRL_TEGRA186_PADCTL
int tegra18x_phy_xusb_set_vbus_override(struct phy *phy);
int tegra18x_phy_xusb_clear_vbus_override(struct phy *phy);
void tegra18x_phy_xusb_utmi_pad_power_on(struct phy *phy);
void tegra18x_phy_xusb_utmi_pad_power_down(struct phy *phy);
void tegra18x_phy_xusb_set_dcd_debounce_time(struct phy *phy, u32 val);
void tegra18x_phy_xusb_utmi_pad_charger_detect_on(struct phy *phy);
void tegra18x_phy_xusb_utmi_pad_charger_detect_off(struct phy *phy);
void tegra18x_phy_xusb_utmi_pad_enable_detect_filters(struct phy *phy);
void tegra18x_phy_xusb_utmi_pad_disable_detect_filters(struct phy *phy);
void tegra18x_phy_xusb_utmi_pad_set_protection_level(struct phy *phy, int level,
					enum tegra_vbus_dir dir);
bool tegra18x_phy_xusb_utmi_pad_dcd(struct phy *phy);
u32 tegra18x_phy_xusb_noncompliant_div_detect(struct phy *phy);
bool tegra18x_phy_xusb_utmi_pad_primary_charger_detect(struct phy *phy);
bool tegra18x_phy_xusb_utmi_pad_secondary_charger_detect(struct phy *phy);
int tegra18x_phy_xusb_utmi_vbus_power_on(struct phy *phy);
int tegra18x_phy_xusb_utmi_vbus_power_off(struct phy *phy);
int tegra18x_utmi_vbus_enable(struct phy *phy);
int tegra18x_phy_xusb_overcurrent_detected(struct phy *phy);
void tegra18x_phy_xusb_handle_overcurrent(struct phy *phy);
int tegra18x_phy_xusb_set_id_override(struct phy *phy);
int tegra18x_phy_xusb_clear_id_override(struct phy *phy);
bool tegra18x_phy_xusb_has_otg_cap(struct phy *phy);

int tegra18x_phy_xusb_enable_sleepwalk(struct phy *phy,
				    enum usb_device_speed speed);
int tegra18x_phy_xusb_disable_sleepwalk(struct phy *phy);
int tegra18x_phy_xusb_enable_wake(struct phy *phy);
int tegra18x_phy_xusb_disable_wake(struct phy *phy);
int tegra18x_phy_xusb_pretend_connected(struct phy *phy);

/* tegra_phy_xusb_remote_wake_detected()
 *   check if a XUSB phy has detected remote wake.
 *   return values:
 *         0: no
 *       > 0: yes
 *       < 0: error
 */
int tegra18x_phy_xusb_remote_wake_detected(struct phy *phy);
int tegra18x_phy_xusb_set_reverse_id(struct phy *phy);
int tegra18x_phy_xusb_clear_reverse_id(struct phy *phy);
int tegra18x_phy_xusb_generate_srp(struct phy *phy);
int tegra18x_phy_xusb_enable_srp_detect(struct phy *phy);
int tegra18x_phy_xusb_disable_srp_detect(struct phy *phy);
bool tegra18x_phy_xusb_srp_detected(struct phy *phy);
int tegra18x_phy_xusb_enable_otg_int(struct phy *phy);
int tegra18x_phy_xusb_disable_otg_int(struct phy *phy);
int tegra18x_phy_xusb_ack_otg_int(struct phy *phy);
int tegra18x_phy_xusb_get_otg_vbus_id(struct phy *phy,
		struct tegra_xusb_otg_vbus_id *change);
#else
static inline int tegra18x_phy_xusb_set_vbus_override(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_clear_vbus_override(struct phy *phy)
{
	return 0;
}

static inline void tegra18x_phy_xusb_utmi_pad_power_on(struct phy *phy)
{
}

static inline void tegra18x_phy_xusb_utmi_pad_power_down(struct phy *phy)
{
}

static inline void tegra18x_phy_xusb_set_dcd_debounce_time(struct phy *phy,
								u32 val)
{
}

static inline void tegra18x_phy_xusb_utmi_pad_charger_detect_on(struct phy *phy)
{
}

static inline void tegra18x_phy_xusb_utmi_pad_charger_detect_off
							(struct phy *phy)
{
}

static inline void tegra18x_phy_xusb_utmi_pad_enable_detect_filters
							(struct phy *phy)
{
}

static inline void tegra18x_phy_xusb_utmi_pad_disable_detect_filters
							(struct phy *phy)
{
}

static inline void tegra18x_phy_xusb_utmi_pad_set_protection_level
						(struct phy *phy, int level,
						enum tegra_vbus_dir dir)
{
}

static inline bool tegra18x_phy_xusb_utmi_pad_dcd(struct phy *phy)
{
	return 0;
}

static inline u32 tegra18x_phy_xusb_noncompliant_div_detect(struct phy *phy)
{
	return 0;
}

static inline bool tegra18x_phy_xusb_utmi_pad_primary_charger_detect
							(struct phy *phy)
{
	return 0;
}

static inline bool tegra18x_phy_xusb_utmi_pad_secondary_charger_detect
							(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_utmi_vbus_power_on(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_utmi_vbus_power_off(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_utmi_vbus_enable(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_overcurrent_detected(struct phy *phy)
{
	return 0;
}

static inline void tegra18x_phy_xusb_handle_overcurrent(struct phy *phy)
{
}

static inline int tegra18x_phy_xusb_set_id_override(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_clear_id_override(struct phy *phy)
{
	return 0;
}

static inline bool tegra18x_phy_xusb_has_otg_cap(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_enable_sleepwalk(struct phy *phy,
				    enum usb_device_speed speed)
{
	return 0;
}

static inline int tegra18x_phy_xusb_disable_sleepwalk(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_enable_wake(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_disable_wake(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_pretend_connected(struct phy *phy)
{
	return 0;
}

/* tegra_phy_xusb_remote_wake_detected()
 *   check if a XUSB phy has detected remote wake.
 *   return values:
 *         0: no
 *       > 0: yes
 *       < 0: error
 */
static inline int tegra18x_phy_xusb_remote_wake_detected(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_set_reverse_id(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_clear_reverse_id(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_generate_srp(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_enable_srp_detect(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_disable_srp_detect(struct phy *phy)
{
	return 0;
}

static inline bool tegra18x_phy_xusb_srp_detected(struct phy *phy)
{
	return false;
}

static inline int tegra18x_phy_xusb_enable_otg_int(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_disable_otg_int(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_ack_otg_int(struct phy *phy)
{
	return 0;
}

static inline int tegra18x_phy_xusb_get_otg_vbus_id(struct phy *phy,
		struct tegra_xusb_otg_vbus_id *change)
{
	return 0;
}
#endif
#endif /* __SOC_TEGRA_XUSB_H__ */
