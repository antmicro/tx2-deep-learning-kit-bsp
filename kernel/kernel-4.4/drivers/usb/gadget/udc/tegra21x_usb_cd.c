/*
 * Copyright (c) 2016-2017, NVIDIA Corporation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/device.h>
#include <soc/tegra/xusb.h>
#include "tegra_usb_cd.h"

#define	VON_DIV2P0_DET	BIT(0)
#define	VON_DIV2P7_DET	BIT(1)
#define	VOP_DIV2P0_DET	BIT(2)
#define	VOP_DIV2P7_DET	BIT(3)

#define VREG_CUR_LEVEL_0	500
#define VREG_CUR_LEVEL_1	900
#define VREG_CUR_LEVEL_2	1500
#define VREG_CUR_LEVEL_3	2000

#define IS_CUR_IN_RANGE(ma, low, high)	\
	((ma >= VREG_CUR_LEVEL_##low) && (ma <= (VREG_CUR_LEVEL_##high - 1)))
#define VREG_LVL(ma, level)	IS_CUR_IN_RANGE(ma, level, level + 1)

static bool tegra21x_usb_dcp_charger_detect(struct tegra_usb_cd *ucd)
{
	bool status;

	status = tegra21x_phy_xusb_utmi_pad_dcd(ucd->phy);
	dev_dbg(ucd->dev, "DCD: %d\n", status);

	/* Primary Detection step */
	status = tegra21x_phy_xusb_utmi_pad_primary_charger_detect(ucd->phy);
	if (status)
		dev_dbg(ucd->dev, "Voltage from D+ to D-, CDP/DCP detected\n");

	dev_dbg(ucd->dev, "Primary detection: %d\n", status);

	return status;
}

static bool tegra21x_usb_cdp_charger_detect(struct tegra_usb_cd *ucd)
{
	bool status;

	/* Secondary Detection step */
	status = tegra21x_phy_xusb_utmi_pad_secondary_charger_detect(ucd->phy);
	if (status)
		dev_dbg(ucd->dev, "No Voltage from D- to D+, CDP detected\n");

	dev_dbg(ucd->dev, "Secondary detection: %d\n", status);

	return status;
}

static int tegra21x_usb_apple_charger_detect(struct tegra_usb_cd *ucd)
{
	u32 val;
	val = tegra21x_phy_xusb_noncompliant_div_detect(ucd->phy);
	if ((val & VOP_DIV2P0_DET) && (val & VON_DIV2P0_DET))
		return APPLE_500MA;
	if ((val & VOP_DIV2P0_DET) && (val & VON_DIV2P7_DET))
		return APPLE_1000MA;
	if (((val & VOP_DIV2P7_DET) && (val & VON_DIV2P0_DET))
		|| ((val & VOP_DIV2P7_DET) && (val & VON_DIV2P7_DET)))
		return APPLE_2000MA;
	return -1;
}

static int tegra21x_pad_power_on(struct tegra_usb_cd *ucd)
{
	tegra21x_phy_xusb_utmi_pad_charger_detect_on(ucd->phy);
	tegra21x_phy_xusb_set_dcd_debounce_time(ucd->phy, 0xa);
	tegra21x_phy_xusb_utmi_pad_enable_detect_filters(ucd->phy);

	return 0;
}

static int tegra21x_pad_power_off(struct tegra_usb_cd *ucd)
{
	tegra21x_phy_xusb_utmi_pad_disable_detect_filters(ucd->phy);
	tegra21x_phy_xusb_utmi_pad_charger_detect_off(ucd->phy);

	return 0;
}

static void tegra21x_usb_vbus_pad_protection(struct tegra_usb_cd *ucd,
			bool enable)
{
	int current_limit;

	current_limit = ucd->current_limit_ma;

	if (!enable) {
		tegra21x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, -1,
				TEGRA_VBUS_DEFAULT);
		return;
	}

	if (VREG_LVL(current_limit, 0))
		tegra21x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 0,
				TEGRA_VBUS_SINK);
	else if (VREG_LVL(current_limit, 1))
		tegra21x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 1,
				TEGRA_VBUS_SINK);
	else if (VREG_LVL(current_limit, 2))
		tegra21x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 2,
				TEGRA_VBUS_SINK);
	else if (current_limit >= VREG_CUR_LEVEL_3)
		tegra21x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 3,
				TEGRA_VBUS_SINK);
}

static int no_op(struct tegra_usb_cd *ucd)
{
	return 0;
}

static struct tegra_usb_cd_ops tegra21_ucd_ops = {
	.open = no_op,
	.close = no_op,
	.power_on = tegra21x_pad_power_on,
	.power_off = tegra21x_pad_power_off,
	.dcp_cd = tegra21x_usb_dcp_charger_detect,
	.cdp_cd = tegra21x_usb_cdp_charger_detect,
	.apple_cd = tegra21x_usb_apple_charger_detect,
	.vbus_pad_protection = tegra21x_usb_vbus_pad_protection,
};

int tegra21x_usb_cd_init_ops(struct tegra_usb_cd *ucd)
{
	ucd->hw_ops = &tegra21_ucd_ops;
	return 0;
}
