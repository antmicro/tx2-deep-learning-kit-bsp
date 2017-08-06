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

#define VON_DIV2P0_DET BIT(0)
#define VON_DIV2P7_DET BIT(1)
#define VOP_DIV2P0_DET BIT(2)
#define VOP_DIV2P7_DET BIT(3)

static bool tegra18x_usb_dcp_charger_detect(struct tegra_usb_cd *ucd)
{
	bool status;

	status = tegra18x_phy_xusb_utmi_pad_dcd(ucd->phy);
	dev_dbg(ucd->dev, "DCD: %d\n", status);

	/* Primary Detection step */
	status = tegra18x_phy_xusb_utmi_pad_primary_charger_detect(ucd->phy);
	if (status)
		dev_dbg(ucd->dev, "Voltage from D+ to D-, CDP/DCP detected\n");

	dev_dbg(ucd->dev, "Primary detection: %d\n", status);

	return status;
}

static bool tegra18x_usb_cdp_charger_detect(struct tegra_usb_cd *ucd)
{
	bool status;

	/* Secondary Detection step */
	status = tegra18x_phy_xusb_utmi_pad_secondary_charger_detect(ucd->phy);
	if (status)
		dev_dbg(ucd->dev, "No Voltage from D- to D+, CDP detected\n");

	dev_dbg(ucd->dev, "Secondary detection: %d\n", status);

	return status;
}

static int tegra18x_usb_apple_charger_detect(struct tegra_usb_cd *ucd)
{
	u32 val;

	val = tegra18x_phy_xusb_noncompliant_div_detect(ucd->phy);
	if ((val & VOP_DIV2P0_DET) && (val & VON_DIV2P0_DET))
		return APPLE_500MA;
	if ((val & VOP_DIV2P0_DET) && (val & VON_DIV2P7_DET))
		return APPLE_1000MA;
	if (((val & VOP_DIV2P7_DET) && (val & VON_DIV2P0_DET))
		|| ((val & VOP_DIV2P7_DET) && (val & VON_DIV2P7_DET)))
		return APPLE_2000MA;
	return -1;
}

static int tegra18x_pad_power_on(struct tegra_usb_cd *ucd)
{
	tegra18x_phy_xusb_utmi_pad_charger_detect_on(ucd->phy);
	tegra18x_phy_xusb_set_dcd_debounce_time(ucd->phy, 0xa);
	tegra18x_phy_xusb_utmi_pad_enable_detect_filters(ucd->phy);

	return 0;
}

static int tegra18x_pad_power_off(struct tegra_usb_cd *ucd)
{
	tegra18x_phy_xusb_utmi_pad_disable_detect_filters(ucd->phy);
	tegra18x_phy_xusb_utmi_pad_charger_detect_off(ucd->phy);

	return 0;
}

static void tegra18x_usb_vbus_pad_protection(struct tegra_usb_cd *ucd,
			bool enable)
{
	if (!enable) {
		tegra18x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, -1,
				TEGRA_VBUS_DEFAULT);
		return;
	}

	if (ucd->current_limit_ma >= 500 &&
			ucd->current_limit_ma <= 899)
		tegra18x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 0,
				TEGRA_VBUS_SINK);
	else if (ucd->current_limit_ma >= 900 &&
			ucd->current_limit_ma <= 1499)
		tegra18x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 1,
				TEGRA_VBUS_SINK);
	else if (ucd->current_limit_ma >= 1500 &&
			ucd->current_limit_ma <= 1999)
		tegra18x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 2,
				TEGRA_VBUS_SINK);
	else if (ucd->current_limit_ma >= 2000)
		tegra18x_phy_xusb_utmi_pad_set_protection_level(ucd->phy, 3,
				TEGRA_VBUS_SINK);
}

static int no_op(struct tegra_usb_cd *ucd)
{
	return 0;
}

static struct tegra_usb_cd_ops tegra18_ucd_ops = {
	.open = no_op,
	.close = no_op,
	.power_on = tegra18x_pad_power_on,
	.power_off = tegra18x_pad_power_off,
	.dcp_cd = tegra18x_usb_dcp_charger_detect,
	.cdp_cd = tegra18x_usb_cdp_charger_detect,
	.apple_cd = tegra18x_usb_apple_charger_detect,
	.vbus_pad_protection = tegra18x_usb_vbus_pad_protection,
};

int tegra18x_usb_cd_init_ops(struct tegra_usb_cd *ucd)
{
	ucd->hw_ops = &tegra18_ucd_ops;

	return 0;
}
