/*
 * Copyright (c) 2016-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/tegra_pm_domains.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/tegra_prod.h>
#include <linux/tegra-powergate.h>
#include <linux/string.h>

#include <soc/tegra/fuse.h>
#include <soc/tegra/pmc.h>
#include <soc/tegra/xusb.h>

#include <dt-bindings/pinctrl/pinctrl-tegra21x-padctl-uphy.h>

#define VERBOSE_DEBUG
#ifdef TRACE
#undef TRACE
#endif
#ifdef VERBOSE_DEBUG
#define TRACE(dev, fmt, args...)					\
	dev_dbg(dev, "%s(%d) " fmt, __func__, __LINE__, ## args)
#else
#define TRACE(dev, fmt, args...)					\
	do {								\
		if (0)							\
			dev_dbg(dev, "%s(%d) " fmt,		\
				__func__, __LINE__, ## args);		\
	} while (0)
#endif

#include "core.h"
#include "pinctrl-utils.h"

#define TEGRA_PCIE_PHYS		(2)
#define TEGRA_SATA_PHYS		(1)
#define TEGRA_USB3_PHYS		(4)
#define TEGRA_UTMI_PHYS		(4)
#define TEGRA_HSIC_PHYS		(1)
#define T21x_UPHY_PLLS		(2)
#define T21x_UPHY_LANES		(8)
#define SATA_LANE_MASK		BIT(7)
#define TEGRA_CDP_PHYS		(4)

#define TDCD_TIMEOUT_MS		400


/* XUSB PADCTL registers */
#define XUSB_PADCTL_USB2_PAD_MUX_0		(0x4)
#define   USB2_OTG_PAD_PORTx(_port, _val)	(((_val) & 0x3)		\
						<< ((_port) * 2))
#define   USB2_OTG_PAD_PORTx_XUSB(_port)	USB2_OTG_PAD_PORTx(_port, 1)
#define   USB2_HSIC_PAD_PORTx_XUSB(_port)	BIT((_port) + 14)
#define   HSIC_PAD_TRK_XUSB			BIT(16)
#define   USB2_BIAS_PAD_XUSB			BIT(18)
#define   HSIC_PORTx_CONFIG_HSIC(_port)		BIT((_port) + 20)

#define XUSB_PADCTL_USB3_PAD_MUX_0		(0x28)
#define   SEL(_lane, _val)			(((_val) & 0x3)		\
						<< (((_lane) * 2) + 12))
#define   SEL_PCIE_X1(_lane)			SEL(_lane, 0)
#define   SEL_USB3(_lane)			SEL(_lane, 1)
#define   SEL_SATA(_lane)			SEL(_lane, 2)
#define   SEL_PCIE_X4(_lane)			SEL(_lane, 3)
#define   FORCE_PAD_IDDQ_DISABLE(_lane)		BIT((_lane) + 1)

#define XUSB_PADCTL_SS_PORT_MAP_0		(0x014)
#define   SS_PORT_MAP(_ss, _usb2)		(((_usb2) & 0x7)	\
						<< ((_ss) * 5))
#define   SS_PORT_MAP_PORT_DISABLED		(0x7)

#define XUSB_PADCTL_USB2_PORT_CAP_0		(0x8)
#define   PORTX_CAP_SHIFT(x)			((x) * 4)
#define   PORT_CAP_MASK				(0x3)
#define     PORT_CAP_DISABLED			(0x0)
#define     PORT_CAP_HOST			(0x1)
#define     PORT_CAP_DEVICE			(0x2)
#define     PORT_CAP_OTG			(0x3)

#define XUSB_PADCTL_ELPG_PROGRAM_0		(0x20)
#define   USB2_PORT_WAKE_INTERRUPT_ENABLE(x)	BIT((x))
#define   USB2_PORT_WAKEUP_EVENT(x)		BIT((x) + 7)
#define   SS_PORT_WAKE_INTERRUPT_ENABLE(x)	BIT((x) + 14)
#define   SS_PORT_WAKEUP_EVENT(x)		BIT((x) + 21)
#define   USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(x)			\
						BIT((x) + 28)
#define   USB2_HSIC_PORT_WAKEUP_EVENT(x)	BIT((x) + 30)
#define   ALL_WAKE_EVENTS			(			\
	USB2_PORT_WAKEUP_EVENT(0) | USB2_PORT_WAKEUP_EVENT(1) |		\
	USB2_PORT_WAKEUP_EVENT(2) | USB2_PORT_WAKEUP_EVENT(3) |		\
	SS_PORT_WAKEUP_EVENT(0) | SS_PORT_WAKEUP_EVENT(1) |		\
	SS_PORT_WAKEUP_EVENT(2) | SS_PORT_WAKEUP_EVENT(3) |		\
	USB2_HSIC_PORT_WAKEUP_EVENT(0))

#define XUSB_PADCTL_ELPG_PROGRAM_1		(0x24)
#define   SSPX_ELPG_CLAMP_EN(x)			BIT(0 + (x) * 3)
#define   SSPX_ELPG_CLAMP_EN_EARLY(x)		BIT(1 + (x) * 3)
#define   SSPX_ELPG_VCORE_DOWN(x)		BIT(2 + (x) * 3)
#define   AUX_MUX_LP0_CLAMP_EN			BIT(29)
#define   AUX_MUX_LP0_CLAMP_EN_EARLY		BIT(30)
#define   AUX_MUX_LP0_VCORE_DOWN		BIT(31)

#define XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(x)	(0x80 + (x) * 0x40)
#define   PD_CHG                                (1 << 0)
#define   VDCD_DET_FILTER_EN                    (1 << 4)
#define   VDAT_DET                              (1 << 5)
#define   VDAT_DET_FILTER_EN                    (1 << 8)
#define   OP_SINK_EN                            (1 << 9)
#define   OP_SRC_EN                             (1 << 10)
#define   ON_SINK_EN                            (1 << 11)
#define   ON_SRC_EN                             (1 << 12)
#define   OP_I_SRC_EN                           (1 << 13)
#define   ZIP_FILTER_EN                         (1 << 21)
#define   ZIN_FILTER_EN                         (1 << 25)
#define   DCD_DETECTED                          (1 << 26)
#define   SRP_DETECT_EN                         (1 << 28)
#define   SRP_DETECTED                          (1 << 29)
#define   SRP_INTR_EN                           (1 << 30)
#define   GENERATE_SRP                          (1 << 31)

#define XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(x)	(0x84 + (x) * 0x40)
#define   DIV_DET_EN				BIT(4)
#define   VREG_FIX18				BIT(6)
#define   VREG_LEV(x)                           (((x) & 0x3) << 7)
#define   USBOP_RPD_OVRD                        (1 << 16)
#define   USBOP_RPD_OVRD_VAL                    (1 << 17)
#define   USBOP_RPU_OVRD                        (1 << 18)
#define   USBOP_RPU_OVRD_VAL                    (1 << 19)
#define   USBON_RPD_OVRD                        (1 << 20)
#define   USBON_RPD_OVRD_VAL                    (1 << 21)
#define   USBON_RPU_OVRD                        (1 << 22)
#define   USBON_RPU_OVRD_VAL                    (1 << 23)

#define XUSB_PADCTL_USB2_OTG_PADX_CTL_0(x)	(0x88 + (x) * 0x40)
#define   HS_CURR_LEVEL(x)			((x) & 0x3f)
#define   TERM_SEL				BIT(25)
#define   USB2_OTG_PD				BIT(26)
#define   USB2_OTG_PD2				BIT(27)
#define   USB2_OTG_PD2_OVRD_EN			BIT(28)
#define   USB2_OTG_PD_ZI			BIT(29)

#define XUSB_PADCTL_USB2_OTG_PADX_CTL_1(x)	(0x8c + (x) * 0x40)
#define   USB2_OTG_PD_DR			BIT(2)
#define   TERM_RANGE_ADJ(x)			(((x) & 0xf) << 3)
#define   RPD_CTRL(x)				(((x) & 0x1f) << 26)
#define   RPD_CTRL_VALUE(x)			(((x) << 26) & 0x1f)

#define XUSB_PADCTL_USB2_BIAS_PAD_CTL_0		(0x284)
#define   BIAS_PAD_PD				BIT(11)
#define   HS_SQUELCH_LEVEL(x)			(((x) & 0x7) << 0)

#define XUSB_PADCTL_USB2_BIAS_PAD_CTL_1		(0x288)
#define   TCTRL_VALUE(x)			(((x) & 0x3f) >> 0)
#define   PCTRL_VALUE(x)			(((x) >> 6) & 0x3f)
#define   USB2_TRK_START_TIMER(x)		(((x) & 0x7f) << 12)
#define   USB2_TRK_DONE_RESET_TIMER(x)		(((x) & 0x7f) << 19)
#define   USB2_PD_TRK				BIT(26)

#define XUSB_PADCTL_USB2_VBUS_ID		(0xc60)
#define   VBUS_VALID_ST_CHNG			BIT(4)
#define   IDDIG_ST_CHNG				BIT(10)
#define   VBUS_SOURCE_SELECT(x)			(((x) & 0x3) << 12)
#define   VBUS_SOURCE_SELECT_VBUS		VBUS_SOURCE_SELECT(1)
#define   VBUS_OVERRIDE_VBUS_ON			BIT(14)
#define   ID_SOURCE_SELECT(x)			(((x) & 0x3) << 16)
#define   ID_SOURCE_SELECT_ID			ID_SOURCE_SELECT(1)
#define   ID_OVERRIDE(x)			(((x) & 0xf) << 18)
#define   ID_OVERRIDE_GROUNDED			ID_OVERRIDE(0)
#define   ID_OVERRIDE_FLOATING			ID_OVERRIDE(8)

#define XUSB_PADCTL_HSIC_PADX_CTL_0(x)		(0x300 + (x) * 0x20)
#define   HSIC_PD_TX_DATA0			BIT(1)
#define   HSIC_PD_TX_STROBE			BIT(3)
#define   HSIC_PD_RX_DATA0			BIT(4)
#define   HSIC_PD_RX_STROBE			BIT(6)
#define   HSIC_PD_ZI_DATA0			BIT(7)
#define   HSIC_PD_ZI_STROBE			BIT(9)
#define   HSIC_RPD_DATA0			BIT(13)
#define   HSIC_RPD_STROBE			BIT(15)
#define   HSIC_RPU_DATA0			BIT(16)
#define   HSIC_RPU_STROBE			BIT(18)

#define XUSB_PADCTL_HSIC_PAD_TRK_CTL_0		(0x340)
#define   HSIC_TRK_START_TIMER(x)		(((x) & 0x7f) << 5)
#define   HSIC_TRK_DONE_RESET_TIMER(x)		(((x) & 0x7f) << 12)
#define   HSIC_PD_TRK				BIT(19)

/* XUSB PADCTL UPHY PLL P0/S0 registers */
#define UPHY_PLL_CTL_1				(0x0)
#define   PLL_IDDQ				BIT(0)
#define   PLL_SLEEP(x)				(((x) & 0x3) << 1)
#define   PLL_ENABLE				BIT(3)
#define   PWR_OVRD				BIT(4)
#define   LOCKDET_STATUS			BIT(15)
#define   FREQ_MDIV(x)				(((x) & 0x3) << 16)
#define   FREQ_NDIV(x)				(((x) & 0xff) << 20)
#define   FREQ_NDIV_USB_VAL			(0x19)
#define   FREQ_NDIV_SATA_VAL			(0x1e)

#define UPHY_PLL_CTL_2				(0x4)
#define   CAL_EN				BIT(0)
#define   CAL_DONE				BIT(1)
#define   CAL_OVRD				BIT(2)
#define   CAL_RESET				BIT(3)
#define   CAL_CTRL(x)				(((x) & 0xffffff) << 4)
#define   CAL_CTRL_VAL				(0x136)

#define UPHY_PLL_CTL_4				(0xc)
#define   REFCLK_SEL(x)				(((x) & 0xf) << 4)
#define   TXCLKREF_SEL(x)			(((x) & 0x3) << 12)
#define   TXCLKREF_SEL_SATA_VAL			(0x0)
#define   TXCLKREF_SEL_USB_VAL			(0x2)
#define   TXCLKREF_EN				BIT(15)

#define UPHY_PLL_CTL_5				(0x10)
#define   DCO_CTRL(x)				(((x) & 0xff) << 16)
#define   DCO_CTRL_VAL				(0x2a)

#define UPHY_PLL_CTL_8				(0x1c)
#define   RCAL_EN				BIT(12)
#define   RCAL_CLK_EN				BIT(13)
#define   RCAL_OVRD				BIT(15)
#define   RCAL_DONE				BIT(31)

/* XUSB PADCTL UPHY Lane registers */
#define UPHY_MISC_PAD_CTL_1			(0x0)
#define   AUX_TX_IDDQ				BIT(0)
#define   AUX_TX_IDDQ_OVRD			BIT(1)
#define   AUX_TX_TERM_EN			BIT(2)
#define   AUX_TX_RDET_EN			BIT(4)
#define   AUX_TX_RDET_BYP			BIT(5)
#define   AUX_TX_RDET_CLK_EN			BIT(6)
#define   AUX_TX_RDET_STATUS			BIT(7)
#define   AUX_TX_MODE_OVRD			BIT(12)
#define   AUX_RX_MODE_OVRD			BIT(13)
#define   AUX_RX_IDDQ				BIT(16)
#define   AUX_RX_IDDQ_OVRD			BIT(17)
#define   AUX_RX_TERM_EN			BIT(18)
#define   AUX_RX_IDLE_MODE(x)			(((x) & 0x3) << 20)
#define   AUX_RX_IDLE_EN			BIT(22)
#define   AUX_RX_IDLE_TH(x)			(((x) & 0x3) << 24)

#define UPHY_MISC_PAD_CTL_2			(0x4)
#define   RX_IDDQ_OVRD				BIT(9)
#define   RX_IDDQ				BIT(8)
#define   TX_IDDQ_OVRD				BIT(1)
#define   TX_IDDQ				BIT(0)

#define UPHY_MISC_PAD_CTL_4			(0xc)
#define   RX_TERM_EN				BIT(21)
#define   RX_TERM_OVRD				BIT(23)

/* FUSE USB_CALIB registers */
/* FUSE_USB_CALIB_0 */
#define HS_CURR_LEVEL_PADX_SHIFT(x)		((x) ? (11 + (x - 1) * 6) : 0)
#define HS_CURR_LEVEL_PAD_MASK			(0x3f)
/* TODO: HS_TERM_RANGE_ADJ has bits overlap, check with hardware team */
#define HS_TERM_RANGE_ADJ_SHIFT			(7)
#define HS_TERM_RANGE_ADJ_MASK			(0xf)
#define HS_SQUELCH_SHIFT			(29)
#define HS_SQUELCH_MASK				(0x7)
/* FUSE_USB_CALIB_EXT_0 */
#define RPD_CTRL_SHIFT				(0)
#define RPD_CTRL_MASK				(0x1f)

#define XUSB_PADCTL_USB2_BATTERY_CHRG_TDCD_DBNC_TIMER_0 (0x280)
#define   TDCD_DBNC(x)                          (((x) & 0x7ff) << 0)

static struct of_device_id tegra_xusbb_pd[] = {
		{ .compatible = "nvidia,tegra210-xusbb-pd", },
		{},
};

static struct of_device_id tegra_xusbc_pd[] = {
		{ .compatible = "nvidia,tegra210-xusbc-pd", },
		{},
};

enum tegra21x_function {
	TEGRA21x_FUNC_HSIC,
	TEGRA21x_FUNC_XUSB,
	TEGRA21x_FUNC_PCIE,
	TEGRA21x_FUNC_USB3,
	TEGRA21x_FUNC_SATA,
	TEGRA21x_FUNC_INVALID,
};

enum tegra21xb01_function {
	TEGRA21xB01_FUNC_XUSB,
	TEGRA21xB01_FUNC_PCIE,
	TEGRA21xB01_FUNC_USB3,
	TEGRA21xB01_FUNC_INVALID,
};

struct tegra_padctl_uphy_function {
	const char *name;
	const char * const *groups;
	unsigned int num_groups;
};

struct tegra_padctl_uphy_group {
	const unsigned int *funcs;
	unsigned int num_funcs;
};

struct tegra_padctl_uphy;

struct tegra_padctl_uphy_soc {
	const struct pinctrl_pin_desc *pins;
	unsigned int num_pins;

	const struct tegra_padctl_uphy_function *functions;
	unsigned int num_functions;

	const struct tegra_padctl_uphy_lane *lanes;
	unsigned int num_lanes;

	unsigned int hsic_port_offset;

	const char * const *supply_names;
	unsigned int num_supplies;

	void (*usb3_phy_set_lfps_detector)(struct tegra_padctl_uphy *uphy,
				    unsigned int port, bool enable);

	bool disable_u0_ts1_detect;
};

struct tegra_padctl_uphy_lane {
	const char *name;

	unsigned int offset;
	unsigned int shift;
	unsigned int mask;
	unsigned int iddq;

	const unsigned int *funcs;
	unsigned int num_funcs;
};

struct tegra_xusb_fuse_calibration {
	u32 hs_curr_level[TEGRA_UTMI_PHYS];
	u32 hs_squelch;
	u32 hs_term_range_adj;
	u32 rpd_ctrl;
	int hs_curr_level_offset; /* deal with platform design deviation */
};

enum xusb_port_cap {
	CAP_DISABLED = TEGRA_PADCTL_PORT_DISABLED,
	HOST_ONLY = TEGRA_PADCTL_PORT_HOST_ONLY,
	DEVICE_ONLY = TEGRA_PADCTL_PORT_DEVICE_ONLY,
	OTG = TEGRA_PADCTL_PORT_OTG_CAP,
};

struct tegra_xusb_usb3_port {
	enum xusb_port_cap port_cap;
	unsigned int uphy_lane;
	unsigned int usb2_map;
	bool clamp_en_early_enabled;
	bool receiver_detector_disabled;
};

enum tegra_pcie_lane_select {
	PCIE_LANE_X4 = TEGRA_PADCTL_PCIE_LANE_X4,
	PCIE_LANE_X1 = TEGRA_PADCTL_PCIE_LANE_X1,
};

struct tegra_pcie_controller {
	unsigned int uphy_lane_bitmap;
	enum tegra_pcie_lane_select pcie_lane_select;
};

struct tegra_xusb_utmi_port {
	enum xusb_port_cap port_cap;
	int usb3_port_fake;
};

struct tegra_xusb_hsic_port {
	bool pretend_connected;
};

enum uphy_pll_state {
	UPHY_PLL_POWER_DOWN = 0,
	UPHY_PLL_POWER_UP_SW_CTL,
	UPHY_PLL_POWER_UP_HW_SEQ,
};

static const char * const uphy_pll_states[] = {
	"UPHY_PLL_POWER_DOWN",
	"UPHY_PLL_POWER_UP_SW_CTL",
	"UPHY_PLL_POWER_UP_HW_SEQ"
};

enum source_pll_state {
	PLL_POWER_DOWN = 0,
	PLL_POWER_UP_SW_CTL,
	PLL_POWER_UP_HW_SEQ, /* only valid for plle */
};

static const char * const source_pll_states[] = {
	"PLL_POWER_DOWN",
	"PLL_POWER_UP_SW_CTL",
	"PLL_POWER_UP_HW_SEQ",
};

struct padctl_context {
	u32 vbus_id;
	u32 usb2_pad_mux;
	u32 usb2_port_cap;
};

struct tegra_padctl_uphy {
	struct device *dev;
	void __iomem *padctl_regs;
	void __iomem *uphy_pll_regs[T21x_UPHY_PLLS];
	void __iomem *uphy_lane_regs[T21x_UPHY_LANES];

	struct reset_control *padctl_rst;

	struct clk *plle; /* plle in software control state */
	struct clk *usb2_trk_clk; /* utmi tracking circuit clock */
	struct clk *hsic_trk_clk; /* hsic tracking circuit clock */

	struct mutex lock;
	spinlock_t	spinlock;

	const struct tegra_padctl_uphy_soc *soc;
	struct tegra_xusb_fuse_calibration calib;
	struct tegra_prod *prod_list;
	struct pinctrl_dev *pinctrl;
	struct pinctrl_desc desc;

	struct phy_provider *provider;
	struct phy *usb3_phys[TEGRA_USB3_PHYS];
	struct phy *utmi_phys[TEGRA_UTMI_PHYS];
	struct phy *hsic_phys[TEGRA_HSIC_PHYS];
	struct phy *cdp_phys[TEGRA_CDP_PHYS];
	struct phy *pcie_phys[TEGRA_PCIE_PHYS];
	struct phy *sata_phys[TEGRA_SATA_PHYS];
	struct tegra_xusb_hsic_port hsic_ports[TEGRA_HSIC_PHYS];
	struct tegra_xusb_utmi_port utmi_ports[TEGRA_UTMI_PHYS];
	int utmi_otg_port_base_1; /* one based utmi port number */
	struct tegra_xusb_usb3_port usb3_ports[TEGRA_USB3_PHYS];
	int usb3_otg_port_base_1; /* one based usb3 port number */
	unsigned long usb3_lanes;
	struct tegra_pcie_controller pcie_controllers[TEGRA_PCIE_PHYS];
	unsigned long pcie_lanes;
	unsigned long sata_lanes;
	bool sata_bypass_fuse;

	struct work_struct mbox_req_work;
	struct tegra_xusb_mbox_msg mbox_req;
	struct mbox_client mbox_client;
	struct mbox_chan *mbox_chan;

	bool host_mode_phy_disabled; /* set true if mailbox is not available */
	unsigned int utmi_enable; /* track if USB2 tracking circuits
				   * could be powered down
				   */
	unsigned int hsic_enable; /* track if HSIC tracking circuits
				   * could be powered down
				   */
	unsigned int utmipll_use_count; /* track if UTMIPLL used by UTMI and
					 * HSIC could be put into IDDQ
					 */
	/* TODO: should move to host controller driver? */
	struct regulator *vbus[TEGRA_UTMI_PHYS];
	struct regulator *vddio_hsic;

	enum uphy_pll_state uphy_pll_state[T21x_UPHY_PLLS];
	enum source_pll_state plle_state;

	struct reset_control *uphy_pll_rst[T21x_UPHY_PLLS];

	/* vbus/id based OTG */
	struct work_struct otg_vbus_work;
	bool otg_vbus_on;
	bool otg_vbus_alwayson;
	bool cdp_used;

	struct regulator_bulk_data *supplies;
	struct padctl_context padctl_context;

	struct tegra_utmi_pad_config utmi_pad_cfg;

	/* ref count for bias pad */
	int bias_pad_enable;
};

#ifdef VERBOSE_DEBUG
#define padctl_writel(_padctl, _value, _offset)				\
{									\
	unsigned long v = _value, o = _offset;				\
	pr_debug("%s padctl_write %s(@0x%lx) with 0x%lx\n", __func__,	\
		#_offset, o, v);					\
	writel(v, _padctl->padctl_regs + o);				\
}

#define padctl_readl(_padctl, _offset)					\
({									\
	unsigned long v, o = _offset;					\
	v = readl(_padctl->padctl_regs + o);				\
	pr_debug("%s padctl_read %s(@0x%lx) = 0x%lx\n", __func__,	\
		#_offset, o, v);					\
	v;								\
})
#else
static inline void padctl_writel(struct tegra_padctl_uphy *padctl, u32 value,
				 unsigned long offset)
{
	writel(value, padctl->padctl_regs + offset);
}

static inline u32 padctl_readl(struct tegra_padctl_uphy *padctl,
			       unsigned long offset)
{
	return readl(padctl->padctl_regs + offset);
}
#endif

#ifdef VERBOSE_DEBUG
#define uphy_pll_writel(_uphy, _pll, _value, _offset)			\
{									\
	unsigned long v = _value, o = _offset;				\
	pr_debug("%s uphy_pll_writel pll %d %s(@0x%lx) with 0x%lx\n",	\
		__func__, _pll, #_offset, o, v);			\
	writel(v, _uphy->uphy_pll_regs[_pll] + o);			\
}

#define uphy_pll_readl(_uphy, _pll, _offset)				\
({									\
	unsigned long v, o = _offset;					\
	v = readl(_uphy->uphy_pll_regs[_pll] + o);			\
	pr_debug("%s uphy_pll_readl pll %d %s(@0x%lx) = 0x%lx\n",	\
		__func__, _pll, #_offset, o, v);			\
	v;								\
})
#else
static inline void uphy_pll_writel(struct tegra_padctl_uphy *uphy, int pll,
				   u32 value, unsigned long offset)
{
	writel(value, uphy->uphy_pll_regs[pll] + offset);
}

static inline u32 uphy_pll_readl(struct tegra_padctl_uphy *uphy, int pll,
				 unsigned long offset)
{
	return readl(uphy->uphy_pll_regs[pll] + offset);
}
#endif

#ifdef VERBOSE_DEBUG
#define uphy_lane_writel(_uphy, _lane, _value, _offset)			\
{									\
	unsigned long v = _value, o = _offset;				\
	pr_debug("%s uphy_lane_writel lane %d %s(@0x%lx) with 0x%lx\n",	\
		__func__, _lane, #_offset, o, v);			\
	writel(v, _uphy->uphy_lane_regs[_lane] + o);			\
}

#define uphy_lane_readl(_uphy, _lane, _offset)				\
({									\
	unsigned long v, o = _offset;					\
	v = readl(_uphy->uphy_lane_regs[_lane] + o);			\
	pr_debug("%s uphy_lane_readl lane %d %s(@0x%lx) = 0x%lx\n",	\
		__func__, _lane, #_offset, o, v);			\
	v;								\
})
#else
static inline void uphy_lane_writel(struct tegra_padctl_uphy *uphy, int lane,
				   u32 value, unsigned long offset)
{
	writel(value, uphy->uphy_lane_regs[lane] + offset);
}

static inline u32 uphy_lane_readl(struct tegra_padctl_uphy *uphy, int lane,
				 unsigned long offset)
{
	return readl(uphy->uphy_lane_regs[lane] + offset);
}
#endif

static inline int is_chip_t210b01(struct tegra_padctl_uphy *uphy)
{
	struct device_node *np;
	const char *compatible;

	np = uphy->dev->of_node;
	compatible = of_get_property(np, "compatible", NULL);

	if (!compatible) {
		dev_err(uphy->dev, "Unable to find compatible property\n");
		return 0;
	}

	if (strstr(compatible, "tegra21xb01") != NULL)
		return 1;
	return 0;
}

static int str_to_func(char *func_name)
{
	if (!func_name)
		return -EINVAL;

	if (strstr(func_name, "hsic"))
		return TEGRA21x_FUNC_HSIC;
	else if (strstr(func_name, "usb3"))
		return TEGRA21x_FUNC_USB3;
	else if (strstr(func_name, "xusb"))
		return TEGRA21x_FUNC_XUSB;
	else if (strstr(func_name, "pcie"))
		return TEGRA21x_FUNC_PCIE;
	else
		return TEGRA21x_FUNC_SATA;
}

static int get_function(struct tegra_padctl_uphy *uphy, char *func)
{
	int ret;

	ret = is_chip_t210b01(uphy);

	switch (str_to_func(func)) {
	case TEGRA21x_FUNC_HSIC:
		return ret ? TEGRA21xB01_FUNC_INVALID : TEGRA21x_FUNC_HSIC;
	case TEGRA21x_FUNC_XUSB:
		return ret ? TEGRA21xB01_FUNC_XUSB : TEGRA21x_FUNC_XUSB;
	case TEGRA21x_FUNC_PCIE:
		return ret ? TEGRA21xB01_FUNC_PCIE : TEGRA21x_FUNC_PCIE;
	case TEGRA21x_FUNC_SATA:
		return ret ? TEGRA21xB01_FUNC_INVALID : TEGRA21x_FUNC_SATA;
	case TEGRA21x_FUNC_USB3:
		return ret ? TEGRA21xB01_FUNC_USB3 : TEGRA21x_FUNC_USB3;
	default:
		dev_err(uphy->dev, "Invalid function\n");
	}
	return ret ? TEGRA21xB01_FUNC_INVALID : TEGRA21x_FUNC_INVALID;
}

static int tegra21x_padctl_uphy_regulators_init(struct tegra_padctl_uphy *uphy)
{
	struct device *dev = uphy->dev;
	size_t size;
	int err;
	int i;

	size = uphy->soc->num_supplies * sizeof(struct regulator_bulk_data);
	uphy->supplies = devm_kzalloc(dev, size, GFP_ATOMIC);
	if (!uphy->supplies) {
		dev_err(dev, "failed to alloc memory for regulators\n");
		return -ENOMEM;
	}

	for (i = 0; i < uphy->soc->num_supplies; i++)
		uphy->supplies[i].supply = uphy->soc->supply_names[i];

	err = devm_regulator_bulk_get(dev, uphy->soc->num_supplies,
					uphy->supplies);
	if (err) {
		dev_err(dev, "failed to request regulators %d\n", err);
		return err;
	}

	return 0;
}

/* caller must hold uphy->lock */
static void uphy_pll_sw_overrides(struct tegra_padctl_uphy *uphy, int pll,
				enum tegra21x_function func, bool set)
{
	struct device *dev = uphy->dev;
	u32 value;

	TRACE(dev, "%s PLL%d overrides\n", set ? "set" : "clear", pll);

	if (set) {
		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_1);
		value |= PWR_OVRD;
		uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_1);

		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_2);
		value |= CAL_OVRD;
		uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_2);

		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
		value |= RCAL_OVRD;
		uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_8);
	} else {
		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_1);
		value &= ~PWR_OVRD;
		uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_1);

		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_2);
		value &= ~CAL_OVRD;
		uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_2);

		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
		value &= ~RCAL_OVRD;
		uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_8);
	}
}

#define uphy_pll_clear_sw_overrides(u, p, f)		\
	uphy_pll_sw_overrides(u, p, f, false)

#define uphy_pll_set_sw_overrides(u, p, f)		\
	uphy_pll_sw_overrides(u, p, f, true)

/* caller must hold uphy->lock */
static int uphy_pll_source_clk_state_check(struct tegra_padctl_uphy *uphy)
{
	struct device *dev = uphy->dev;

	if ((uphy->plle_state < PLL_POWER_DOWN) ||
		(uphy->plle_state > PLL_POWER_UP_HW_SEQ)) {
		dev_err(dev, "invalid PLLE state %d\n", uphy->plle_state);
		return -EINVAL;
	}
	TRACE(dev, "PLLE state %s\n", source_pll_states[uphy->plle_state]);

	return 0;
}

/* caller must hold uphy->lock */
static int uphy_pll_source_clk_enable(struct tegra_padctl_uphy *uphy)
{
	struct device *dev = uphy->dev;
	int rc = 0;

	rc = uphy_pll_source_clk_state_check(uphy);
	if (rc)
		return rc;

	if (tegra210_plle_hw_sequence_is_enabled()) {
		TRACE(dev,
		"PLLE HW was already enabled before UPHY PLL init!\n");
		uphy->plle_state = PLL_POWER_UP_HW_SEQ;
	}

	/* power up PLLE if it has not been enabled */
	if (uphy->plle_state == PLL_POWER_DOWN) {
		rc = clk_prepare_enable(uphy->plle);
		if (rc) {
			dev_err(dev, "failed to enable PLLE clock %d\n",
				rc);
			return rc;
		}
		uphy->plle_state = PLL_POWER_UP_SW_CTL;
		TRACE(dev, "done enable PLLE in SW\n");
	}

	return 0;
}

/* caller must hold uphy->lock */
static int uphy_pll_resistor_calibration(struct tegra_padctl_uphy *uphy,
					 int pll)
{
	struct device *dev = uphy->dev;
	u32 value;

	TRACE(dev, "PLL%d resistor calibration\n", pll);

	/* perform resistor calibration */
	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
	value |= RCAL_EN;
	uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_8);

	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
	value |= RCAL_CLK_EN;
	uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_8);

	usleep_range(5, 10);

	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
	if (!(value & RCAL_DONE))
		dev_err(dev, "PLL%d start resistor calibration timeout\n", pll);

	/* stop resistor calibration */
	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
	value &= ~RCAL_EN;
	uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_8);

	usleep_range(5, 10);

	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
	if (value & RCAL_DONE)
		dev_err(dev, "PLL%d stop resistor calibration timeout\n", pll);

	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_8);
	value &= ~RCAL_CLK_EN;
	uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_8);

	return 0;
}

/* caller must hold uphy->lock */
static int uphy_pll_hw_sequencer_enable(struct tegra_padctl_uphy *uphy, int pll,
					enum tegra21x_function func)
{
	struct device *dev = uphy->dev;
	u32 value;
	unsigned int uphy_lane;

	TRACE(dev, "enable PLL%d HW power sequencer by function %s\n",
					pll, uphy->soc->functions[func].name);

	if (func == get_function(uphy, "sata")) {
		for_each_set_bit(uphy_lane, &uphy->sata_lanes,
					T21x_UPHY_LANES) {
			value = uphy_lane_readl(uphy, uphy_lane,
						UPHY_MISC_PAD_CTL_1);
			value &= ~AUX_RX_IDLE_TH(~0);
			value |= (AUX_RX_IDLE_TH(1) | AUX_RX_MODE_OVRD |
					AUX_RX_IDLE_EN);
			uphy_lane_writel(uphy, uphy_lane, value,
						UPHY_MISC_PAD_CTL_1);

			udelay(200);

			value = uphy_lane_readl(uphy, uphy_lane,
						UPHY_MISC_PAD_CTL_4);
			value |= (RX_TERM_EN | RX_TERM_OVRD);
			uphy_lane_writel(uphy, uphy_lane, value,
					UPHY_MISC_PAD_CTL_4);
		}
	}

	if (pll)
		tegra210_sata_pll_hw_control_enable();
	else
		tegra210_xusb_pll_hw_control_enable();

	/* remove SW overrides to allow HW sequencer to run */
	uphy_pll_clear_sw_overrides(uphy, pll, func);

	usleep_range(10, 20);

	if (pll)
		tegra210_sata_pll_hw_sequence_start();
	else
		tegra210_xusb_pll_hw_sequence_start();

	uphy->uphy_pll_state[pll] = UPHY_PLL_POWER_UP_HW_SEQ;
	TRACE(dev, "done enable PLL%d in HW by function %s\n",
				pll, uphy->soc->functions[func].name);

	/* enable PLLE Power sequencer */
	if ((uphy->uphy_pll_state[0] == UPHY_PLL_POWER_UP_HW_SEQ) &&
		(uphy->uphy_pll_state[1] == UPHY_PLL_POWER_UP_HW_SEQ)) {
		tegra210_plle_hw_sequence_start();
		uphy->plle_state = PLL_POWER_UP_HW_SEQ;
		TRACE(dev, "done enable PLLE in HW");
	}

	return 0;
}

/* caller must hold uphy->lock */
static int uphy_pll_calibration(struct tegra_padctl_uphy *uphy, int pll)
{
	struct device *dev = uphy->dev;
	u32 value;
	int i;

	TRACE(dev, "PLL%d calibration\n", pll);

	/* perform PLL calibration */
	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_2);
	value |= CAL_EN;
	uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_2);

	for (i = 0; i < 50; i++) {
		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_2);
		if (value & CAL_DONE)
			break;
		usleep_range(10, 15);
	}
	if (!(value & CAL_DONE))
		dev_err(dev, "start PLL%d calibration timeout\n", pll);

	/* stop PLL calibration */
	value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_2);
	value &= ~CAL_EN;
	uphy_pll_writel(uphy, pll, value, UPHY_PLL_CTL_2);

	for (i = 0; i < 50; i++) {
		value = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_2);
		if (!(value & CAL_DONE))
			break;
		usleep_range(10, 15);
	}
	if (value & CAL_DONE)
		dev_err(dev, "stop PLL%d calibration timeout\n", pll);

	return 0;
}

/* caller must hold uphy->lock */
static int uphy_pll_init_full(struct tegra_padctl_uphy *uphy, int pll,
			      enum tegra21x_function func)
{
	struct device *dev = uphy->dev;
	u32 reg;

	if (pll < 0 || pll >= T21x_UPHY_PLLS)
		return -EINVAL;

	if ((uphy->uphy_pll_state[pll] < UPHY_PLL_POWER_DOWN) ||
		(uphy->uphy_pll_state[pll] > UPHY_PLL_POWER_UP_HW_SEQ)) {
		dev_err(dev, "invalid PLL%d state %d\n", pll,
					uphy->uphy_pll_state[pll]);
		return -EINVAL;
	}

	if ((pll == 0 && tegra210_xusb_pll_hw_sequence_is_enabled()) ||
		(pll == 1 && tegra210_sata_pll_hw_sequence_is_enabled()))
		uphy->uphy_pll_state[pll] = UPHY_PLL_POWER_UP_HW_SEQ;

	TRACE(dev, "PLL%d state %s\n", pll,
			uphy_pll_states[uphy->uphy_pll_state[pll]]);

	if (uphy->uphy_pll_state[pll] >= UPHY_PLL_POWER_UP_SW_CTL) {
		TRACE(dev,
		"PLL%d was already enabled in SW/HW, current function is %s",
		pll, uphy->soc->functions[func].name);
		return 0; /* already done */
	}

	TRACE(dev, "PLL%d full init by function %s\n",
			pll, uphy->soc->functions[func].name);

	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_2);
	reg &= ~CAL_CTRL(~0);
	reg |= CAL_CTRL(CAL_CTRL_VAL);
	uphy_pll_writel(uphy, pll, reg, UPHY_PLL_CTL_2);

	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_5);
	reg &= ~DCO_CTRL(~0);
	reg |= DCO_CTRL(DCO_CTRL_VAL);
	uphy_pll_writel(uphy, pll, reg, UPHY_PLL_CTL_5);

	if (uphy->uphy_pll_state[pll] == UPHY_PLL_POWER_DOWN)
		uphy_pll_set_sw_overrides(uphy, pll, func);

	/* power up PLL */
	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_4);
	reg &= ~(TXCLKREF_SEL(~0) | REFCLK_SEL(~0));
	if (func == get_function(uphy, "sata"))
		reg |= (TXCLKREF_SEL(TXCLKREF_SEL_SATA_VAL) | TXCLKREF_EN);
	else
		reg |= (TXCLKREF_SEL(TXCLKREF_SEL_USB_VAL) | TXCLKREF_EN);
	uphy_pll_writel(uphy, pll, reg, UPHY_PLL_CTL_4);

	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_1);
	reg &= ~(FREQ_MDIV(~0) | FREQ_NDIV(~0));
	if (pll == 1) {
		if (func == get_function(uphy, "sata"))
			reg |= FREQ_NDIV(FREQ_NDIV_SATA_VAL);
		else
			reg |= FREQ_NDIV(FREQ_NDIV_USB_VAL);
	} else
		reg |= FREQ_NDIV(25);
	uphy_pll_writel(uphy, pll, reg, UPHY_PLL_CTL_1);

	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_1);
	reg &= ~PLL_IDDQ;
	uphy_pll_writel(uphy, pll, reg, UPHY_PLL_CTL_1);

	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_1);
	reg &= ~PLL_SLEEP(~0);
	uphy_pll_writel(uphy, pll, reg, UPHY_PLL_CTL_1);

	ndelay(100);

	uphy_pll_calibration(uphy, pll);

	/* enable PLL and wait for lock */
	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_1);
	reg |= PLL_ENABLE;
	uphy_pll_writel(uphy, pll, reg, UPHY_PLL_CTL_1);

	usleep_range(65, 70);

	reg = uphy_pll_readl(uphy, pll, UPHY_PLL_CTL_1);
	if (!(reg & LOCKDET_STATUS))
		dev_err(dev, "enable PLL%d timeout\n", pll);

	if (uphy->uphy_pll_state[pll] == UPHY_PLL_POWER_DOWN)
		uphy_pll_resistor_calibration(uphy, pll);

	uphy->uphy_pll_state[pll] = UPHY_PLL_POWER_UP_SW_CTL;
	TRACE(dev, "done enable PLL%d in SW by function %s\n",
			pll, uphy->soc->functions[func].name);

	return uphy_pll_hw_sequencer_enable(uphy, pll, func);
}

/* caller must hold uphy->lock */
static int uphy_pll_init(struct tegra_padctl_uphy *uphy,
			 enum tegra21x_function func)
{
	struct device *dev = uphy->dev;
	int rc = 0;

	TRACE(dev, "PLL init by function %s\n",
		uphy->soc->functions[func].name);

	rc = uphy_pll_source_clk_enable(uphy);
	if (rc)
		return rc;

	if (is_chip_t210b01(uphy)) {
		switch (func) {
		case TEGRA21xB01_FUNC_PCIE:
			rc = uphy_pll_init_full(uphy, 0, func);
			break;
		case TEGRA21xB01_FUNC_USB3:
			rc = uphy_pll_init_full(uphy, 0, func);
			if (rc)
				return rc;
			break;
		default:
		rc = -EINVAL;
		break;
		}
	} else {
		switch (func) {
		case TEGRA21x_FUNC_PCIE:
			rc = uphy_pll_init_full(uphy, 0, func);
			break;
		case TEGRA21x_FUNC_SATA:
			rc = uphy_pll_init_full(uphy, 1, func);
			break;
		case TEGRA21x_FUNC_USB3:
			rc = uphy_pll_init_full(uphy, 0, func);
			if (rc)
				return rc;
			if (uphy->usb3_lanes & SATA_LANE_MASK)
				rc = uphy_pll_init_full(uphy, 1, func);
			break;
		default:
		rc = -EINVAL;
		break;
		}
	}

	return rc;
}

/* caller must hold uphy->lock */
static int uphy_pll_reset_deassert(struct tegra_padctl_uphy *uphy, int pll)
{
	struct device *dev = uphy->dev;
	int rc = 0;

	if (pll < 0 || pll >= T21x_UPHY_PLLS)
		return -EINVAL;

	TRACE(dev, "deassert reset to PLL%d\n", pll);
	rc = reset_control_deassert(uphy->uphy_pll_rst[pll]);
	if (rc) {
		dev_err(dev, "failed to deassert reset PLL%d %d\n", pll, rc);
	}

	return rc;
}

/* caller must hold uphy->lock */
static int uphy_pll_reset_assert(struct tegra_padctl_uphy *uphy, int pll)
{
	struct device *dev = uphy->dev;
	int rc = 0;

	if (pll < 0 || pll >= T21x_UPHY_PLLS)
		return -EINVAL;

	TRACE(dev, "assert reset to PLL%d\n", pll);
	rc = reset_control_assert(uphy->uphy_pll_rst[pll]);
	if (rc) {
		dev_err(dev, "failed to assert reset for PLL%d %d\n", pll, rc);
	}

	return rc;
}

static inline
struct tegra_padctl_uphy *mbox_work_to_uphy(struct work_struct *work)
{
	return container_of(work, struct tegra_padctl_uphy, mbox_req_work);
}

#define PIN_OTG_0	0
#define PIN_OTG_1	1
#define PIN_OTG_2	2
#define PIN_OTG_3	3
#define PIN_HSIC_0	4
#define PIN_UPHY_0	5
#define PIN_UPHY_1	6
#define PIN_UPHY_2	7
#define PIN_UPHY_3	8
#define PIN_UPHY_4	9
#define PIN_UPHY_5	10
#define PIN_UPHY_6	11
#define PIN_SATA_0	12
#define PIN_CDP_0	13
#define PIN_CDP_1	14
#define PIN_CDP_2	15
#define PIN_CDP_3	16

#define T21xB01_PIN_OTG_0	0
#define T21xB01_PIN_OTG_1	1
#define T21xB01_PIN_OTG_2	2
#define T21xB01_PIN_OTG_3	3
#define T21xB01_PIN_UPHY_0	4
#define T21xB01_PIN_UPHY_1	5
#define T21xB01_PIN_UPHY_2	6
#define T21xB01_PIN_UPHY_3	7
#define T21xB01_PIN_UPHY_4	8
#define T21xB01_PIN_UPHY_5	9
#define T21xB01_PIN_CDP_0	10
#define T21xB01_PIN_CDP_1	11
#define T21xB01_PIN_CDP_2	12
#define T21xB01_PIN_CDP_3	13

static inline bool lane_is_otg(unsigned int lane)
{
	return lane >= PIN_OTG_0 && lane <= PIN_OTG_3;
}

static inline bool lane_is_hsic(struct tegra_padctl_uphy *uphy,
				unsigned int lane)
{
	if (is_chip_t210b01(uphy))
		return false;
	return lane == PIN_HSIC_0;
}

static inline bool pad_is_cdp(unsigned int pad)
{
	return pad >= PIN_CDP_0 && pad <= PIN_CDP_3;
}

static inline int get_uphy_pin_zero(struct tegra_padctl_uphy *uphy)
{
	if (is_chip_t210b01(uphy))
		return T21xB01_PIN_UPHY_0;
	return PIN_UPHY_0;
}

static inline bool lane_is_uphy(struct tegra_padctl_uphy *uphy,
					unsigned int lane)
{
	if (is_chip_t210b01(uphy))
		return lane >= T21xB01_PIN_UPHY_0 && lane <= T21xB01_PIN_UPHY_5;
	return lane >= PIN_UPHY_0 && lane <= PIN_SATA_0;
}

static int lane_to_usb3_port(struct tegra_padctl_uphy *uphy,
			     unsigned int uphy_lane)
{
	unsigned int i;

	for (i = 0; i < TEGRA_USB3_PHYS; i++) {
		if (uphy->usb3_ports[i].uphy_lane == uphy_lane)
			return i;
	}

	return -EINVAL;
}

static int lane_to_pcie_controller(struct tegra_padctl_uphy *uphy,
			     unsigned int uphy_lane)
{
	unsigned int i;

	for (i = 0; i < TEGRA_PCIE_PHYS; i++) {
		if (uphy->pcie_controllers[i].uphy_lane_bitmap & BIT(uphy_lane))
			return i;
	}

	return -EINVAL;
}

static int tegra_padctl_uphy_get_groups_count(struct pinctrl_dev *pinctrl)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);

	TRACE(uphy->dev, "num_pins %u\n", uphy->soc->num_pins);
	return uphy->soc->num_pins;
}

static const char *tegra_padctl_uphy_get_group_name(struct pinctrl_dev *pinctrl,
						    unsigned int group)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);

	TRACE(uphy->dev, "group %u name %s\n", group,
						uphy->soc->pins[group].name);
	return uphy->soc->pins[group].name;
}

static int tegra_padctl_uphy_get_group_pins(struct pinctrl_dev *pinctrl,
				 unsigned group,
				 const unsigned **pins,
				 unsigned *num_pins)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);

	*pins = &uphy->soc->pins[group].number;
	*num_pins = 1; /* one pin per group */

	TRACE(uphy->dev, "group %u num_pins %u pins[0] %u\n",
						group, *num_pins, *pins[0]);

	return 0;
}

enum tegra_xusb_padctl_param {
	TEGRA_PADCTL_UPHY_USB3_PORT,
	TEGRA_PADCTL_UPHY_PORT_CAP,
	TEGRA_PADCTL_UPHY_PCIE_CONTROLLER_NUM,
	TEGRA_PADCTL_UPHY_HSIC_PRETEND_CONNECTED,
	TEGRA_PADCTL_UPHY_USB2_MAP,
	TEGRA_PADCTL_UPHY_PCIE_LANE_SELECT,
	TEGRA_PADCTL_UPHY_USB3_PORT_FAKE,
};

static const struct tegra_padctl_uphy_property {
	const char *name;
	enum tegra_xusb_padctl_param param;
} properties[] = {
	{"nvidia,usb3-port", TEGRA_PADCTL_UPHY_USB3_PORT},
	{"nvidia,port-cap", TEGRA_PADCTL_UPHY_PORT_CAP},
	{"nvidia,pcie-controller", TEGRA_PADCTL_UPHY_PCIE_CONTROLLER_NUM},
	{"nvidia,pretend-connected", TEGRA_PADCTL_UPHY_HSIC_PRETEND_CONNECTED},
	{"nvidia,usb2-map", TEGRA_PADCTL_UPHY_USB2_MAP},
	{"nvidia,pcie-lane-select", TEGRA_PADCTL_UPHY_PCIE_LANE_SELECT},
	{"nvidia,usb3-port-fake", TEGRA_PADCTL_UPHY_USB3_PORT_FAKE},
};

#define TEGRA_XUSB_PADCTL_PACK(param, value) ((param) << 16 | (value))
#define TEGRA_XUSB_PADCTL_UNPACK_PARAM(config) ((config) >> 16)
#define TEGRA_XUSB_PADCTL_UNPACK_VALUE(config) ((config) & 0xffff)

static int tegra21x_padctl_uphy_parse_subnode(struct tegra_padctl_uphy *uphy,
					   struct device_node *np,
					   struct pinctrl_map **maps,
					   unsigned int *reserved_maps,
					   unsigned int *num_maps)
{
	unsigned int i, reserve = 0, num_configs = 0;
	unsigned long config, *configs = NULL;
	const char *function, *group;
	struct property *prop;
	int err = 0;
	u32 value;

	err = of_property_read_string(np, "nvidia,function", &function);
	if (err < 0) {
		if (err != -EINVAL)
			goto out;

		function = NULL;
	}

	for (i = 0; i < ARRAY_SIZE(properties); i++) {
		err = of_property_read_u32(np, properties[i].name, &value);
		if (err < 0) {
			if (err == -EINVAL)
				continue;

			goto out;
		}

		config = TEGRA_XUSB_PADCTL_PACK(properties[i].param, value);

		err = pinctrl_utils_add_config(uphy->pinctrl, &configs,
					       &num_configs, config);
		if (err < 0)
			goto out;
	}

	if (function)
		reserve++;

	if (num_configs)
		reserve++;

	err = of_property_count_strings(np, "nvidia,lanes");
	if (err < 0)
		goto out;

	reserve *= err;

	err = pinctrl_utils_reserve_map(uphy->pinctrl, maps, reserved_maps,
					num_maps, reserve);
	if (err < 0)
		goto out;

	of_property_for_each_string(np, "nvidia,lanes", prop, group) {
		if (function) {
			err = pinctrl_utils_add_map_mux(uphy->pinctrl, maps,
					reserved_maps, num_maps, group,
					function);
			if (err < 0)
				goto out;
		}

		if (num_configs) {
			err = pinctrl_utils_add_map_configs(uphy->pinctrl,
					maps, reserved_maps, num_maps, group,
					configs, num_configs,
					PIN_MAP_TYPE_CONFIGS_GROUP);
			if (err < 0)
				goto out;
		}
	}

	err = 0;

out:
	kfree(configs);
	return err;
}

static int tegra_padctl_uphy_dt_node_to_map(struct pinctrl_dev *pinctrl,
					    struct device_node *parent,
					    struct pinctrl_map **maps,
					    unsigned int *num_maps)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);
	unsigned int reserved_maps = 0;
	struct device_node *np;
	int err;

	*num_maps = 0;
	*maps = NULL;

	for_each_child_of_node(parent, np) {
		/* If node status is disabled then ignore the node */
		if (!of_device_is_available(np))
			continue;

		err = tegra21x_padctl_uphy_parse_subnode(uphy, np, maps,
						      &reserved_maps,
						      num_maps);
		if (err < 0) {
			pr_info("%s %d err %d\n", __func__, __LINE__, err);
			return err;
		}
	}

	return 0;
}

static const struct pinctrl_ops tegra_xusb_padctl_pinctrl_ops = {
	.get_groups_count = tegra_padctl_uphy_get_groups_count,
	.get_group_name = tegra_padctl_uphy_get_group_name,
	.get_group_pins = tegra_padctl_uphy_get_group_pins,
	.dt_node_to_map = tegra_padctl_uphy_dt_node_to_map,
	.dt_free_map = pinctrl_utils_dt_free_map,
};

static int tegra21x_padctl_uphy_get_functions_count(struct pinctrl_dev *pinctrl)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);

	TRACE(uphy->dev, "num_functions %u\n", uphy->soc->num_functions);
	return uphy->soc->num_functions;
}

static const char *
tegra21x_padctl_uphy_get_function_name(struct pinctrl_dev *pinctrl,
				    unsigned int function)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);

	TRACE(uphy->dev, "function %u name %s\n", function,
					uphy->soc->functions[function].name);

	return uphy->soc->functions[function].name;
}

static int tegra21x_padctl_uphy_get_function_groups(struct pinctrl_dev *pinctrl,
						 unsigned int function,
						 const char * const **groups,
						 unsigned * const num_groups)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);

	*num_groups = uphy->soc->functions[function].num_groups;
	*groups = uphy->soc->functions[function].groups;

	TRACE(uphy->dev, "function %u *num_groups %u groups %s\n",
				function, *num_groups, *groups[0]);
	return 0;
}

static int tegra21x_padctl_uphy_pinmux_set(struct pinctrl_dev *pinctrl,
					   unsigned int function,
					   unsigned int group)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);
	const struct tegra_padctl_uphy_lane *lane;
	unsigned int i;
	u32 value;

	lane = &uphy->soc->lanes[group];

	TRACE(uphy->dev, "group %u (%s) function %u num_funcs %d\n",
			group, lane->name, function, lane->num_funcs);

	for (i = 0; i < lane->num_funcs; i++) {
		if (lane->funcs[i] == function)
			break;
	}

	if (i >= lane->num_funcs)
		return -EINVAL;

	TRACE(uphy->dev, "group %s set to function %s\n",
			lane->name, uphy->soc->functions[function].name);

	/* set XUSB_PADCTL_USB2_PAD_MUX_0 for USB2 and HSIC ports */
	if (lane_is_otg(group)) {
		int port = group - PIN_OTG_0;

		value = padctl_readl(uphy, XUSB_PADCTL_USB2_PAD_MUX_0);
		value |= ((USB2_OTG_PAD_PORTx_XUSB(port)) |
				USB2_BIAS_PAD_XUSB);
		dev_info(uphy->dev, "UTMI-%d is used by XUSB\n", port);
		padctl_writel(uphy, value, XUSB_PADCTL_USB2_PAD_MUX_0);
	} else if (lane_is_hsic(uphy, group)) {
		value = padctl_readl(uphy, XUSB_PADCTL_USB2_PAD_MUX_0);
		value |= (USB2_HSIC_PAD_PORTx_XUSB(0) | HSIC_PAD_TRK_XUSB |
				HSIC_PORTx_CONFIG_HSIC(0));
		padctl_writel(uphy, value, XUSB_PADCTL_USB2_PAD_MUX_0);
	} else if (lane_is_uphy(uphy, group)) {
		int uphy_lane = group - get_uphy_pin_zero(uphy);

		if (function == get_function(uphy, "usb3")) {
			set_bit(uphy_lane, &uphy->usb3_lanes);
			dev_info(uphy->dev, "uphy_lane = %d, set usb3_lanes = 0x%lx\n",
					uphy_lane, uphy->usb3_lanes);
		} else if (function == get_function(uphy, "pcie")) {
			set_bit(uphy_lane, &uphy->pcie_lanes);
			dev_info(uphy->dev, "uphy_lane = %d, set pcie_lanes = 0x%lx\n",
					uphy_lane, uphy->pcie_lanes);
		} else if (function == get_function(uphy, "sata")) {
			set_bit(uphy_lane, &uphy->sata_lanes);
			dev_info(uphy->dev, "uphy_lane = %d, set sata_lanes = 0x%lx\n",
					uphy_lane, uphy->sata_lanes);
		}
	} else if (pad_is_cdp(group)) {
		if (function != get_function(uphy, "xusb"))
			dev_warn(uphy->dev, "group %s isn't for xusb!",
				 lane->name);
	} else
		return -EINVAL;

	return 0;
}

static const struct pinmux_ops tegra21x_padctl_uphy_pinmux_ops = {
	.get_functions_count = tegra21x_padctl_uphy_get_functions_count,
	.get_function_name = tegra21x_padctl_uphy_get_function_name,
	.get_function_groups = tegra21x_padctl_uphy_get_function_groups,
	.set_mux = tegra21x_padctl_uphy_pinmux_set,
};

static int tegra_padctl_uphy_pinconf_group_get(struct pinctrl_dev *pinctrl,
					       unsigned int group,
					       unsigned long *config)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);
	struct device *dev = uphy->dev;
	enum tegra_xusb_padctl_param param;
	unsigned uphy_lane;
	int value = 0;

	param = TEGRA_XUSB_PADCTL_UNPACK_PARAM(*config);

	TRACE(uphy->dev, "group %u param 0x%x\n", group, param);

	switch (param) {
	case TEGRA_PADCTL_UPHY_USB3_PORT:
		uphy_lane = group - get_uphy_pin_zero(uphy);
		value = lane_to_usb3_port(uphy, uphy_lane);
		if (value < 0) {
			dev_err(dev, "Pin %d not mapped to USB3 port\n", group);
			return -EINVAL;
		}
		break;

	case TEGRA_PADCTL_UPHY_PCIE_CONTROLLER_NUM:
		uphy_lane = group - get_uphy_pin_zero(uphy);
		value = lane_to_pcie_controller(uphy, uphy_lane);
		if (value < 0) {
			dev_err(dev, "Pin %d not mapped to PCIE controller\n",
				group);
			return -EINVAL;
		}

		break;
	default:
		dev_err(uphy->dev, "invalid configuration parameter: %04x\n",
			param);
		return -ENOTSUPP;
	}

	*config = TEGRA_XUSB_PADCTL_PACK(param, value);
	return 0;
}

static int tegra_padctl_uphy_pinconf_group_set(struct pinctrl_dev *pinctrl,
					       unsigned int group,
					       unsigned long *configs,
					       unsigned num_configs)
{
	struct tegra_padctl_uphy *uphy = pinctrl_dev_get_drvdata(pinctrl);
	struct device *dev = uphy->dev;
	enum tegra_xusb_padctl_param param;
	unsigned long value;
	unsigned uphy_lane;
	int i;

	for (i = 0; i < num_configs; i++) {
		param = TEGRA_XUSB_PADCTL_UNPACK_PARAM(configs[i]);
		value = TEGRA_XUSB_PADCTL_UNPACK_VALUE(configs[i]);

		TRACE(dev, "group %u config 0x%lx param 0x%x value 0x%lx\n",
			group, configs[i], param, value);

		switch (param) {
		case TEGRA_PADCTL_UPHY_USB3_PORT:
			if (value >= TEGRA_USB3_PHYS) {
				dev_err(dev, "Invalid USB3 port: %lu\n", value);
				return -EINVAL;
			}
			if (!lane_is_uphy(uphy, group)) {
				dev_err(dev, "USB3 port not applicable for pin %d\n",
					group);
				return -EINVAL;
			}

			/* TODO: make sure lane configuration is valid */
			uphy_lane = group - get_uphy_pin_zero(uphy);
			TRACE(dev, "USB3 port %lu uses uphy-lane-%u\n",
			      value, uphy_lane);
			uphy->usb3_ports[value].uphy_lane = uphy_lane;
			uphy->usb3_ports[value].usb2_map =
						SS_PORT_MAP_PORT_DISABLED;
			break;

		case TEGRA_PADCTL_UPHY_USB2_MAP:
			if (lane_is_uphy(uphy, group)) {
				unsigned long port;

				if (value >= TEGRA_UTMI_PHYS) {
					dev_err(dev, "Invalid USB2 port: %lu\n",
						value);
					return -EINVAL;
				}

				uphy_lane = group - get_uphy_pin_zero(uphy);
				port = lane_to_usb3_port(uphy, uphy_lane);
				dev_info(dev, "USB3 port %lu maps to USB2 port %lu\n",
					  port, value);
				uphy->usb3_ports[port].usb2_map = value;
			} else {
				dev_err(dev, "usb2-map not applicable for pin %d\n",
					group);
				return -EINVAL;
			}
			break;

		case TEGRA_PADCTL_UPHY_PORT_CAP:
			if (value > TEGRA_PADCTL_PORT_OTG_CAP) {
				dev_err(dev, "Invalid port-cap: %lu\n", value);
				return -EINVAL;
			}
			if (lane_is_uphy(uphy, group)) {
				int port;

				uphy_lane = group - get_uphy_pin_zero(uphy);
				port = lane_to_usb3_port(uphy, uphy_lane);
				if (port < 0) {
					dev_err(dev, "Pin %d not mapped to USB3 port\n",
						group);
					return -EINVAL;
				}
				uphy->usb3_ports[port].port_cap = value;
				TRACE(dev, "USB3 port %d cap %lu\n",
				      port, value);
				if (value == OTG) {
					if (uphy->usb3_otg_port_base_1)
						dev_warn(dev, "enabling OTG on multiple USB3 ports\n");


					dev_info(dev, "using USB3 port %d for otg\n",
						 port);
					uphy->usb3_otg_port_base_1 =
								port + 1;
				}
			} else if (lane_is_otg(group)) {
				int port = group - PIN_OTG_0;

				uphy->utmi_ports[port].port_cap = value;
				TRACE(dev, "UTMI port %d cap %lu\n",
					  port, value);
				if (value == OTG) {
					if (uphy->utmi_otg_port_base_1)
						dev_warn(dev,
				"enabling OTG on multiple UTMI ports\n");

					dev_info(dev,
					"using UTMI port %d for otg\n", port);
					uphy->utmi_otg_port_base_1 = port + 1;
				}
			} else {
				dev_err(dev,
				"port-cap not applicable for pin %d\n", group);
				return -EINVAL;
			}

			break;

		case TEGRA_PADCTL_UPHY_HSIC_PRETEND_CONNECTED:
			if (lane_is_hsic(uphy, group)) {
				int port = group - PIN_HSIC_0;

				uphy->hsic_ports[port].pretend_connected =
									  value;
				TRACE(dev,
					"HSIC port %d pretend-connected %ld\n",
					port, value);
			} else {
				dev_err(dev,
			"pretend-connected is not applicable for pin %d\n",
				group);
				return -EINVAL;
			}

			break;

		case TEGRA_PADCTL_UPHY_PCIE_CONTROLLER_NUM:
			if (value >= TEGRA_PCIE_PHYS) {
				dev_err(dev, "Invalid PCIE controller: %lu\n",
					value);
				return -EINVAL;
			}
			if (!lane_is_uphy(uphy, group)) {
				dev_err(dev,
					"PCIE controller not applicable for pin %d\n",
					group);
				return -EINVAL;
			}

			/* TODO: make sure lane configuration is valid */
			uphy_lane = group - get_uphy_pin_zero(uphy);
			TRACE(dev, "PCIE controller %lu uses uphy-lane-%u\n",
			      value, uphy_lane);
			uphy->pcie_controllers[value].uphy_lane_bitmap |=
								BIT(uphy_lane);

			break;

		case TEGRA_PADCTL_UPHY_PCIE_LANE_SELECT:
			if (value > PCIE_LANE_X1) {
				dev_err(dev, "Invalid PCIE lane select: %lu\n",
					value);
				return -EINVAL;
			}
			if (lane_is_uphy(uphy, group)) {
				int controller;

				uphy_lane = group - get_uphy_pin_zero(uphy);
				controller =
				lane_to_pcie_controller(uphy, uphy_lane);
				TRACE(dev,
			"PCIE lane select %lu on controller %u uphy-lane-%u\n",
				      value, controller, uphy_lane);
			uphy->pcie_controllers[controller].pcie_lane_select =
									value;
			} else {
				dev_err(dev,
			"PCIE lane select is not applicable for pin %d\n",
					group);
				return -EINVAL;
			}

			break;

		case TEGRA_PADCTL_UPHY_USB3_PORT_FAKE:
			if (value >= TEGRA_USB3_PHYS) {
				dev_err(dev, "Invalid USB3 port: %lu\n", value);
				return -EINVAL;
			}

			if (lane_is_otg(group)) {
				int port = group - PIN_OTG_0;

				uphy->utmi_ports[port].usb3_port_fake = value;
				TRACE(dev,
			"UTMI port %d faked maping to USB3 port %lu\n",
					port, value);
			} else {
				dev_err(dev, "nvidia,usb3-port-fake is only valid in OTG ports\n");
				return -EINVAL;
			}

			break;

		default:
			dev_err(dev, "invalid configuration parameter: %04x\n",
				param);
			return -ENOTSUPP;
		}
	}
	return 0;
}

#ifdef CONFIG_DEBUG_FS
static const char *strip_prefix(const char *s)
{
	const char *comma = strchr(s, ',');

	if (!comma)
		return s;

	return comma + 1;
}

static void
tegra_padctl_uphy_pinconf_group_dbg_show(struct pinctrl_dev *pinctrl,
					 struct seq_file *s,
					 unsigned int group)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(properties); i++) {
		unsigned long config, value;
		int err;

		config = TEGRA_XUSB_PADCTL_PACK(properties[i].param, 0);

		err = tegra_padctl_uphy_pinconf_group_get(pinctrl, group,
							  &config);
		if (err < 0)
			continue;

		value = TEGRA_XUSB_PADCTL_UNPACK_VALUE(config);

		seq_printf(s, "\n\t%s=%lu\n", strip_prefix(properties[i].name),
			   value);
	}
}

static void
tegra_padctl_uphy_pinconf_config_dbg_show(struct pinctrl_dev *pinctrl,
					  struct seq_file *s,
					  unsigned long config)
{
	enum tegra_xusb_padctl_param param;
	const char *name = "unknown";
	unsigned long value;
	unsigned int i;

	param = TEGRA_XUSB_PADCTL_UNPACK_PARAM(config);
	value = TEGRA_XUSB_PADCTL_UNPACK_VALUE(config);

	for (i = 0; i < ARRAY_SIZE(properties); i++) {
		if (properties[i].param == param) {
			name = properties[i].name;
			break;
		}
	}

	seq_printf(s, "%s=%lu", strip_prefix(name), value);
}
#endif

static const struct pinconf_ops tegra_padctl_uphy_pinconf_ops = {
	.pin_config_group_get = tegra_padctl_uphy_pinconf_group_get,
	.pin_config_group_set = tegra_padctl_uphy_pinconf_group_set,
#ifdef CONFIG_DEBUG_FS
	.pin_config_group_dbg_show = tegra_padctl_uphy_pinconf_group_dbg_show,
	.pin_config_config_dbg_show = tegra_padctl_uphy_pinconf_config_dbg_show,
#endif
};

/* caller must hold uphy->lock */
static inline void uphy_lanes_clamp(struct tegra_padctl_uphy *uphy,
				    unsigned long uphy_lane_bitmap,
				    bool enable)
{
	unsigned int lane;
	u32 reg;

	if (enable) {
		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= AUX_MUX_LP0_CLAMP_EN;
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= AUX_MUX_LP0_CLAMP_EN_EARLY;
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= AUX_MUX_LP0_VCORE_DOWN;
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(5, 10);

		/* Enable force IDDQ on lanes */
		for_each_set_bit(lane, &uphy_lane_bitmap, T21x_UPHY_LANES) {
			reg = padctl_readl(uphy, XUSB_PADCTL_USB3_PAD_MUX_0);
			reg &= ~FORCE_PAD_IDDQ_DISABLE(lane);
			padctl_writel(uphy, reg, XUSB_PADCTL_USB3_PAD_MUX_0);
		}
	} else {
		/* Remove IDDQ and clamp */
		for_each_set_bit(lane, &uphy_lane_bitmap, T21x_UPHY_LANES) {
			reg = padctl_readl(uphy, XUSB_PADCTL_USB3_PAD_MUX_0);
			reg |= FORCE_PAD_IDDQ_DISABLE(lane);
			padctl_writel(uphy, reg, XUSB_PADCTL_USB3_PAD_MUX_0);
		}

		usleep_range(5, 10);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~AUX_MUX_LP0_CLAMP_EN;
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~AUX_MUX_LP0_CLAMP_EN_EARLY;
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~AUX_MUX_LP0_VCORE_DOWN;
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);
	}
}

#define uphy_lanes_clamp_enable(u, m) uphy_lanes_clamp(u, m, true)
#define uphy_lanes_clamp_disable(u, m) uphy_lanes_clamp(u, m, false)

static int pcie_phy_to_controller(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	unsigned int i;

	for (i = 0; i < TEGRA_PCIE_PHYS; i++) {
		if (phy == uphy->pcie_phys[i])
			return i;
	}
	WARN_ON(1);

	return -EINVAL;
}

static int tegra21x_pcie_uphy_pll_init(struct tegra_padctl_uphy *uphy)
{
	unsigned long uphy_lane_bitmap;
	unsigned int uphy_lane;
	u32 reg;
	int rc;
	int controller;

	mutex_lock(&uphy->lock);

	uphy_lane_bitmap = uphy->pcie_lanes;
	TRACE(uphy->dev, "%s PCIE controller uphy-lanes 0x%lx\n",
		__func__, uphy_lane_bitmap);

	for_each_set_bit(uphy_lane, &uphy_lane_bitmap, T21x_UPHY_LANES) {
		/* Program lane ownership by selecting mux to PCIE */
		controller = lane_to_pcie_controller(uphy, uphy_lane);

		reg = padctl_readl(uphy, XUSB_PADCTL_USB3_PAD_MUX_0);
		reg &= ~SEL(uphy_lane, ~0);
		if (uphy->pcie_controllers[controller].pcie_lane_select ==
			TEGRA_PADCTL_PCIE_LANE_X4)
			reg |= SEL_PCIE_X4(uphy_lane);
		else
			reg |= SEL_PCIE_X1(uphy_lane);
		padctl_writel(uphy, reg, XUSB_PADCTL_USB3_PAD_MUX_0);

		/* reduce idle detect threshold for compliance purpose */
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_1);
		reg &= ~AUX_RX_IDLE_TH(~0);
		reg |= AUX_RX_IDLE_TH(1);
		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_1);
	}

	/* Reset release of lanes and PLL0 */
	rc = uphy_pll_reset_deassert(uphy, 0);
	if (rc)
		goto unlock_out;

	rc = uphy_pll_init(uphy, get_function(uphy, "pcie"));
	if (rc)
		goto assert_pll0_reset;

	mutex_unlock(&uphy->lock);
	return 0;

assert_pll0_reset:
	uphy_pll_reset_assert(uphy, 0);
unlock_out:
	mutex_unlock(&uphy->lock);
	return rc;
}

static int tegra21x_pcie_phy_init(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int controller = pcie_phy_to_controller(phy);
	unsigned long uphy_lane_bitmap;

	if (controller < 0)
		return controller;

	mutex_lock(&uphy->lock);

	uphy_lane_bitmap = uphy->pcie_controllers[controller].uphy_lane_bitmap;
	TRACE(uphy->dev, "phy init PCIE controller %d uphy-lanes 0x%lx\n",
		controller, uphy_lane_bitmap);

	mutex_unlock(&uphy->lock);
	return 0;
}

static int tegra21x_pcie_phy_exit(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int controller = pcie_phy_to_controller(phy);
	unsigned long uphy_lane_bitmap;

	if (controller < 0)
		return controller;

	mutex_lock(&uphy->lock);

	uphy_lane_bitmap = uphy->pcie_controllers[controller].uphy_lane_bitmap;
	TRACE(uphy->dev, "phy exit PCIE controller %d uphy-lanes 0x%lx\n",
		controller, uphy_lane_bitmap);

	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_pcie_phy_power_on(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int controller = pcie_phy_to_controller(phy);
	unsigned long uphy_lane_bitmap;

	if (controller < 0)
		return controller;

	mutex_lock(&uphy->lock);

	uphy_lane_bitmap = uphy->pcie_controllers[controller].uphy_lane_bitmap;
	TRACE(uphy->dev, "power on PCIE controller %d uphy-lanes 0x%lx\n",
		controller, uphy_lane_bitmap);

	uphy_lanes_clamp_disable(uphy, uphy_lane_bitmap);

	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_pcie_phy_power_off(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;
	int controller = pcie_phy_to_controller(phy);
	unsigned long uphy_lane_bitmap;

	if (controller < 0)
		return controller;

	mutex_lock(&uphy->lock);

	uphy_lane_bitmap = uphy->pcie_controllers[controller].uphy_lane_bitmap;
	TRACE(dev, "power off PCIE controller %d uphy-lanes 0x%lx\n",
		controller, uphy_lane_bitmap);

	uphy_lanes_clamp_enable(uphy, uphy_lane_bitmap);

	mutex_unlock(&uphy->lock);

	return 0;
}

static const struct phy_ops pcie_phy_ops = {
	.init = tegra21x_pcie_phy_init,
	.exit = tegra21x_pcie_phy_exit,
	.power_on = tegra21x_pcie_phy_power_on,
	.power_off = tegra21x_pcie_phy_power_off,
	.owner = THIS_MODULE,
};

/* caller must hold uphy->lock */
static inline void tegra21x_sata_phy_idle_detector(
				struct tegra_padctl_uphy *uphy, bool enable)
{
	unsigned int uphy_lane;
	u32 reg;

	TRACE(uphy->dev, "%s SATA idle detector\n",
			enable ? "enable" : "disable");

	for_each_set_bit(uphy_lane, &uphy->sata_lanes, T21x_UPHY_LANES) {
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_1);

		if (enable) {
			reg &= ~(AUX_RX_TERM_EN | AUX_RX_MODE_OVRD |
				AUX_TX_IDDQ | AUX_TX_IDDQ_OVRD);
			reg |= AUX_RX_IDLE_EN;
		} else {
			reg &= ~AUX_RX_IDLE_EN;
			reg |= (AUX_RX_TERM_EN | AUX_RX_MODE_OVRD |
				AUX_TX_IDDQ | AUX_TX_IDDQ_OVRD);
		}

		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_1);
	}
}
#define tegra21x_sata_phy_idle_detector_enable(uphy)		\
		tegra21x_sata_phy_idle_detector(uphy, true)
#define tegra21x_sata_phy_idle_detector_disable(uphy)		\
		tegra21x_sata_phy_idle_detector(uphy, false)

static int tegra21x_sata_uphy_pll_init(struct tegra_padctl_uphy *uphy)
{
	unsigned int uphy_lane;
	u32 reg;
	int rc;

	TRACE(uphy->dev, "%s SATA uphy-lanes 0x%lx\n", __func__,
						uphy->sata_lanes);

	mutex_lock(&uphy->lock);

	/* Program lane ownership by selecting mux to SATA */
	for_each_set_bit(uphy_lane, &uphy->sata_lanes, T21x_UPHY_LANES) {
		reg = padctl_readl(uphy, XUSB_PADCTL_USB3_PAD_MUX_0);
		reg &= ~SEL(uphy_lane, ~0);
		reg |= SEL_SATA(uphy_lane);
		padctl_writel(uphy, reg, XUSB_PADCTL_USB3_PAD_MUX_0);
	}

	/* Reset release of lanes and PLL1 */
	rc = uphy_pll_reset_deassert(uphy, 1);
	if (rc)
		goto unlock_out;

	rc = uphy_pll_init(uphy, get_function(uphy, "sata"));
	if (rc)
		goto assert_pll1_reset;

	tegra21x_sata_phy_idle_detector_disable(uphy);
	tegra210_set_sata_pll_seq_sw(true);

	mutex_unlock(&uphy->lock);

	return 0;

assert_pll1_reset:
	uphy_pll_reset_assert(uphy, 1);
unlock_out:
	mutex_unlock(&uphy->lock);

	return rc;
}

static int tegra21x_sata_phy_init(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;

	TRACE(dev, "phy init SATA uphy-lanes 0x%lx\n", uphy->sata_lanes);

	return 0;
}

static int tegra21x_sata_phy_exit(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;

	TRACE(dev, "phy exit SATA uphy-lanes 0x%lx\n", uphy->sata_lanes);

	return 0;
}

static int tegra21x_sata_phy_power_on(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;

	mutex_lock(&uphy->lock);

	TRACE(dev, "power on SATA uphy-lanes 0x%lx\n", uphy->sata_lanes);

	uphy_lanes_clamp_disable(uphy, uphy->sata_lanes);
	tegra210_set_sata_pll_seq_sw(false);
	tegra21x_sata_phy_idle_detector_enable(uphy);

	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_sata_phy_power_off(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;

	mutex_lock(&uphy->lock);

	TRACE(dev, "power off SATA uphy-lanes 0x%lx\n", uphy->sata_lanes);

	tegra21x_sata_phy_idle_detector_disable(uphy);
	tegra210_set_sata_pll_seq_sw(true);
	uphy_lanes_clamp_enable(uphy, uphy->sata_lanes);

	mutex_unlock(&uphy->lock);

	return 0;
}

static const struct phy_ops sata_phy_ops = {
	.init = tegra21x_sata_phy_init,
	.exit = tegra21x_sata_phy_exit,
	.power_on = tegra21x_sata_phy_power_on,
	.power_off = tegra21x_sata_phy_power_off,
	.owner = THIS_MODULE,
};

static int usb3_phy_to_port(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	unsigned int i;

	for (i = 0; i < TEGRA_USB3_PHYS; i++) {
		if (phy == uphy->usb3_phys[i])
			return i;
	}
	WARN_ON(1);

	return -EINVAL;
}

static void tegra210_usb3_phy_set_lfps_detector(struct tegra_padctl_uphy *uphy,
				    unsigned int port, bool enable)
{
	unsigned uphy_lane = uphy->usb3_ports[port].uphy_lane;
	u32 reg;

	reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_1);

	reg &= ~(AUX_RX_IDLE_MODE(~0) | AUX_RX_TERM_EN | AUX_RX_MODE_OVRD);
	if (!enable)
		reg |= (AUX_RX_IDLE_MODE(0x1) | AUX_RX_TERM_EN |
			AUX_RX_MODE_OVRD);

	uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_1);
}

static int tegra21x_usb3_phy_enable_wakelogic(struct tegra_padctl_uphy *uphy,
					      int port)
{
	struct device *dev = uphy->dev;
	unsigned int uphy_lane;
	u32 reg;

	uphy_lane = uphy->usb3_ports[port].uphy_lane;
	TRACE(dev, "enable wakelogic USB3 port %d uphy-lane-%u\n",
		port, uphy_lane);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg |= SSPX_ELPG_CLAMP_EN_EARLY(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg |= SSPX_ELPG_CLAMP_EN(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(250, 350);

	return 0;
}

static int tegra21x_usb3_phy_disable_wakelogic(struct tegra_padctl_uphy *uphy,
					      int port)
{
	struct device *dev = uphy->dev;
	unsigned int uphy_lane;
	u32 reg;

	uphy_lane = uphy->usb3_ports[port].uphy_lane;
	TRACE(dev, "disable wakelogic USB3 port %d uphy-lane-%u\n",
		port, uphy_lane);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg &= ~SSPX_ELPG_CLAMP_EN_EARLY(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg &= ~SSPX_ELPG_CLAMP_EN(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	return 0;
}

static int tegra21x_usb3_uphy_pll_init(struct tegra_padctl_uphy *uphy)
{
	unsigned long uphy_lane_bitmap;
	unsigned uphy_lane;
	u32 reg;
	int rc;

	uphy_lane_bitmap = uphy->usb3_lanes;
	TRACE(uphy->dev, "XUSB controller uphy-lanes 0x%lx\n",
			uphy_lane_bitmap);

	mutex_lock(&uphy->lock);

	/* Program lane ownership by selecting mux to USB3 */
	for_each_set_bit(uphy_lane, &uphy_lane_bitmap, T21x_UPHY_LANES) {
		reg = padctl_readl(uphy, XUSB_PADCTL_USB3_PAD_MUX_0);
		reg &= ~SEL(uphy_lane, ~0);
		reg |= SEL_USB3(uphy_lane);
		padctl_writel(uphy, reg, XUSB_PADCTL_USB3_PAD_MUX_0);
	}

	/* Reset release of lanes and PLL0, PLL1 */
	rc = uphy_pll_reset_deassert(uphy, 0);
	if (rc)
		goto unlock_out;

	rc = uphy_pll_reset_deassert(uphy, 1);
	if (rc)
		goto assert_pll0_reset;

	rc = uphy_pll_init(uphy, get_function(uphy, "usb3"));
	if (rc)
		goto assert_pll1_reset;

	mutex_unlock(&uphy->lock);
	return 0;

assert_pll1_reset:
	uphy_pll_reset_assert(uphy, 1);
assert_pll0_reset:
	uphy_pll_reset_assert(uphy, 0);
unlock_out:
	mutex_unlock(&uphy->lock);
	return rc;
}

static int tegra21x_usb3_phy_init(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = usb3_phy_to_port(phy);
	unsigned uphy_lane;

	if (port < 0)
		return port;

	mutex_lock(&uphy->lock);

	uphy_lane = uphy->usb3_ports[port].uphy_lane;
	TRACE(uphy->dev, "phy init USB3 port %d uphy-lane-%u\n",
		port, uphy_lane);

	mutex_unlock(&uphy->lock);
	return 0;
}

static int tegra21x_usb3_phy_exit(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;
	int port = usb3_phy_to_port(phy);
	unsigned uphy_lane;

	if (port < 0)
		return port;

	mutex_lock(&uphy->lock);

	uphy_lane = uphy->usb3_ports[port].uphy_lane;
	TRACE(dev, "phy exit USB3 port %d uphy-lane-%u\n",
		port, uphy_lane);

	mutex_unlock(&uphy->lock);
	return 0;
}

static int tegra21x_usb3_phy_power_on(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;
	int port = usb3_phy_to_port(phy);
	char prod_name[] = "prod_c_ssX";
	unsigned int uphy_lane;
	u32 reg;
	int err;
	int port_map = uphy->usb3_ports[port].usb2_map;

	if (port < 0)
		return port;

	mutex_lock(&uphy->lock);

	uphy_lane = uphy->usb3_ports[port].uphy_lane;
	TRACE(uphy->dev, "power on USB3 port %d uphy-lane-%u\n",
		port, uphy_lane);

	sprintf(prod_name, "prod_c_ss%d", port);
	err = tegra_prod_set_by_name(&uphy->padctl_regs, prod_name,
				     uphy->prod_list);
	if (err) {
		dev_info(dev, "failed to apply prod for ss pad%d (%d)\n",
			port, err);
	}

	reg = padctl_readl(uphy, XUSB_PADCTL_SS_PORT_MAP_0);
	reg &= ~SS_PORT_MAP(port, SS_PORT_MAP_PORT_DISABLED);
	reg |= SS_PORT_MAP(port, uphy->usb3_ports[port].usb2_map);
	padctl_writel(uphy, reg, XUSB_PADCTL_SS_PORT_MAP_0);

	/* No XUSB_PADCTL_SS_PORT_CAP on T210, use USB2_PORT_CAP */
	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_PORT_CAP_0);
	reg &= ~(PORT_CAP_MASK << PORTX_CAP_SHIFT(port_map));
	if (uphy->usb3_ports[port].port_cap == CAP_DISABLED)
		reg |= (PORT_CAP_DISABLED << PORTX_CAP_SHIFT(port_map));
	else if (uphy->usb3_ports[port].port_cap == DEVICE_ONLY)
		reg |= (PORT_CAP_DEVICE << PORTX_CAP_SHIFT(port_map));
	else if (uphy->usb3_ports[port].port_cap == HOST_ONLY)
		reg |= (PORT_CAP_HOST << PORTX_CAP_SHIFT(port_map));
	else if (uphy->usb3_ports[port].port_cap == OTG)
		reg |= (PORT_CAP_OTG << PORTX_CAP_SHIFT(port_map));
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_PORT_CAP_0);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg &= ~SSPX_ELPG_VCORE_DOWN(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg &= ~SSPX_ELPG_CLAMP_EN_EARLY(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg &= ~SSPX_ELPG_CLAMP_EN(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	uphy_lanes_clamp_disable(uphy, BIT(uphy_lane));

	mutex_unlock(&uphy->lock);
	return 0;
}

static int tegra21x_usb3_phy_power_off(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;
	int port = usb3_phy_to_port(phy);
	unsigned int uphy_lane;
	u32 reg;

	if (port < 0)
		return port;

	mutex_lock(&uphy->lock);

	uphy_lane = uphy->usb3_ports[port].uphy_lane;
	TRACE(dev, "power off USB3 port %d uphy-lane-%u\n",
		port, uphy_lane);

	uphy_lanes_clamp_enable(uphy, BIT(uphy_lane));

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg |= SSPX_ELPG_CLAMP_EN_EARLY(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg |= SSPX_ELPG_CLAMP_EN(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	usleep_range(100, 200);

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
	reg |= SSPX_ELPG_VCORE_DOWN(port);
	padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

	reg = padctl_readl(uphy, XUSB_PADCTL_SS_PORT_MAP_0);
	reg |= SS_PORT_MAP(port, SS_PORT_MAP_PORT_DISABLED);
	padctl_writel(uphy, reg, XUSB_PADCTL_SS_PORT_MAP_0);

	mutex_unlock(&uphy->lock);
	return 0;
}

static const struct phy_ops usb3_phy_ops = {
	.init = tegra21x_usb3_phy_init,
	.exit = tegra21x_usb3_phy_exit,
	.power_on = tegra21x_usb3_phy_power_on,
	.power_off = tegra21x_usb3_phy_power_off,
	.owner = THIS_MODULE,
};

static inline bool is_usb3_phy(struct phy *phy)
{
	return phy->ops == &usb3_phy_ops;
}

static int utmi_phy_to_port(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	unsigned int i;

	for (i = 0; i < TEGRA_UTMI_PHYS; i++) {
		if (phy == uphy->utmi_phys[i])
			return i;
	}
	WARN_ON(1);

	return -EINVAL;
}

static void tegra21x_utmi_phy_get_pad_config(struct tegra_padctl_uphy *uphy,
				int port, struct tegra_utmi_pad_config *config)
{
	u32 reg;

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
	config->tctrl = TCTRL_VALUE(reg);
	config->pctrl = PCTRL_VALUE(reg);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));
	config->rpd_ctrl = RPD_CTRL_VALUE(reg);
}

static bool tegra21x_utmi_phy_xusb_partitions_powergated(void)
{
	int partition_id_xusbb, partition_id_xusbc;

	partition_id_xusbb = tegra_pd_get_powergate_id(tegra_xusbb_pd);
	if (partition_id_xusbb < 0)
		return -EINVAL;

	partition_id_xusbc = tegra_pd_get_powergate_id(tegra_xusbc_pd);
	if (partition_id_xusbc < 0)
		return -EINVAL;

	if (!tegra_powergate_is_powered(partition_id_xusbb)
			&& !tegra_powergate_is_powered(partition_id_xusbc))
		return true;

	return false;
}

/* caller must hold uphy->lock */
static inline void tegra21x_utmi_phy_put_utmipll_iddq(
				struct tegra_padctl_uphy *uphy, bool in_iddq)
{
	TRACE(uphy->dev, "put UTMIPLL %s IDDQ\n",
			in_iddq ? "in" : "out of");

	if (in_iddq) {
		if (!tegra21x_utmi_phy_xusb_partitions_powergated()) {
			dev_info(uphy->dev,
			"trying to assert IDDQ while XUSB partitions are on\n");
			return;
		}

		tegra210_put_utmipll_in_iddq();
	} else
		tegra210_put_utmipll_out_iddq();
}
#define tegra21x_utmi_phy_put_utmipll_in_iddq(uphy)		\
		tegra21x_utmi_phy_put_utmipll_iddq(uphy, true)
#define tegra21x_utmi_phy_put_utmipll_out_iddq(uphy)		\
		tegra21x_utmi_phy_put_utmipll_iddq(uphy, false)

/* caller must hold uphy->lock */
static inline void tegra21x_usb2_trk(struct tegra_padctl_uphy *uphy, bool on)
{
	int err;
	u32 reg;

	TRACE(uphy->dev, "%s USB2 tracking circuits\n",
			on ? "enable" : "disable");

	err = clk_prepare_enable(uphy->usb2_trk_clk);
	if (err) {
		dev_err(uphy->dev, "failed to enable USB2 tracking clock %d\n",
			err);
	}

	/* BIAS PAD */
	if (on) {
		if (uphy->bias_pad_enable++ > 0)
			return;
		reg = padctl_readl(uphy, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
		reg &= ~USB2_TRK_START_TIMER(~0);
		reg |= USB2_TRK_START_TIMER(0x1e);
		reg &= ~USB2_TRK_DONE_RESET_TIMER(~0);
		reg |= USB2_TRK_DONE_RESET_TIMER(0xa);
		padctl_writel(uphy, reg, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);

		reg = padctl_readl(uphy, XUSB_PADCTL_USB2_BIAS_PAD_CTL_0);
		reg &= ~BIAS_PAD_PD;
		reg &= ~HS_SQUELCH_LEVEL(~0);
		reg |= HS_SQUELCH_LEVEL(uphy->calib.hs_squelch);
		padctl_writel(uphy, reg, XUSB_PADCTL_USB2_BIAS_PAD_CTL_0);

		udelay(1);

		reg = padctl_readl(uphy, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
		reg &= ~USB2_PD_TRK;
		padctl_writel(uphy, reg, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);

		usleep_range(50, 60);
	} else {
		if (WARN_ON(uphy->bias_pad_enable == 0))
			return;
		if (--uphy->bias_pad_enable > 0)
			return;
		if (!uphy->cdp_used) {
			/* only turn BIAS pad off when host CDP isn't enabled */
			reg = padctl_readl(uphy,
					XUSB_PADCTL_USB2_BIAS_PAD_CTL_0);
			reg |= BIAS_PAD_PD;
			padctl_writel(uphy, reg,
					XUSB_PADCTL_USB2_BIAS_PAD_CTL_0);
		}

		reg = padctl_readl(uphy, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
		reg |= USB2_PD_TRK;
		padctl_writel(uphy, reg, XUSB_PADCTL_USB2_BIAS_PAD_CTL_1);
	}

	clk_disable_unprepare(uphy->usb2_trk_clk);
}
#define tegra21x_usb2_trk_on(uphy) tegra21x_usb2_trk(uphy, true)
#define tegra21x_usb2_trk_off(uphy) tegra21x_usb2_trk(uphy, false)

/* caller must hold uphy->lock */
static inline void tegra21x_hsic_trk(struct tegra_padctl_uphy *uphy, bool on)
{
	int err;
	u32 reg;

	TRACE(uphy->dev, "%s HSIC tracking circuits\n",
			on ? "enable" : "disable");

	err = clk_prepare_enable(uphy->hsic_trk_clk);
	if (err) {
		dev_err(uphy->dev, "failed to enable HSIC tracking clock %d\n",
				err);
	}

	if (on) {
		reg = padctl_readl(uphy, XUSB_PADCTL_HSIC_PAD_TRK_CTL_0);
		reg &= ~HSIC_TRK_START_TIMER(~0);
		reg |= HSIC_TRK_START_TIMER(0x1e);
		reg &= ~HSIC_TRK_DONE_RESET_TIMER(~0);
		reg |= HSIC_TRK_DONE_RESET_TIMER(0xa);
		padctl_writel(uphy, reg, XUSB_PADCTL_HSIC_PAD_TRK_CTL_0);

		reg = padctl_readl(uphy, XUSB_PADCTL_HSIC_PADX_CTL_0(0));
		reg &= ~(HSIC_PD_TX_DATA0 | HSIC_PD_TX_STROBE |
			HSIC_PD_RX_DATA0 | HSIC_PD_RX_STROBE |
			HSIC_PD_ZI_DATA0 | HSIC_PD_ZI_STROBE);
		padctl_writel(uphy, reg, XUSB_PADCTL_HSIC_PADX_CTL_0(0));

		udelay(1);

		reg = padctl_readl(uphy, XUSB_PADCTL_HSIC_PAD_TRK_CTL_0);
		reg &= ~HSIC_PD_TRK;
		padctl_writel(uphy, reg, XUSB_PADCTL_HSIC_PAD_TRK_CTL_0);

		usleep_range(50, 60);
	} else {
		reg = padctl_readl(uphy, XUSB_PADCTL_HSIC_PAD_TRK_CTL_0);
		reg |= HSIC_PD_TRK;
		padctl_writel(uphy, reg, XUSB_PADCTL_HSIC_PAD_TRK_CTL_0);
	}

	clk_disable_unprepare(uphy->hsic_trk_clk);
}
#define tegra21x_hsic_trk_on(uphy) tegra21x_hsic_trk(uphy, true)
#define tegra21x_hsic_trk_off(uphy) tegra21x_hsic_trk(uphy, false)

int tegra21x_utmi_vbus_enable(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = utmi_phy_to_port(phy);
	int rc;

	if (port < 0)
		return port;

	TRACE(uphy->dev, "enable vbus-%d\n", port);

	mutex_lock(&uphy->lock);

	if (uphy->vbus[port] && uphy->utmi_ports[port].port_cap == HOST_ONLY) {
		rc = regulator_enable(uphy->vbus[port]);
		if (rc) {
			dev_err(uphy->dev, "enable port %d vbus failed %d\n",
				port, rc);
			mutex_unlock(&uphy->lock);
			return rc;
		}
	}

	mutex_unlock(&uphy->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_utmi_vbus_enable);

static int tegra21x_utmi_phy_init(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = utmi_phy_to_port(phy);

	if (port < 0)
		return port;

	TRACE(uphy->dev, "phy init UTMI port %d\n", port);

	return 0;
}

static int tegra21x_utmi_phy_exit(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = utmi_phy_to_port(phy);
	int rc;

	if (port < 0)
		return port;

	TRACE(uphy->dev, "phy exit UTMI port %d\n",  port);

	mutex_lock(&uphy->lock);

	if (uphy->vbus[port] && uphy->utmi_ports[port].port_cap == HOST_ONLY) {
		rc = regulator_disable(uphy->vbus[port]);
		if (rc) {
			dev_err(uphy->dev, "disable port %d vbus failed %d\n",
				port, rc);
			mutex_unlock(&uphy->lock);
			return rc;
		}
	}

	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_utmi_phy_power_on(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	struct device *dev = uphy->dev;
	int port = utmi_phy_to_port(phy);
	char prod_name[] = "prod_c_utmiX";
	int err;
	u32 reg;
	int usb3_port_fake;

	if (port < 0)
		return port;

	TRACE(uphy->dev, "power on UTMI port %d\n",  port);

	usb3_port_fake = uphy->utmi_ports[port].usb3_port_fake;

	if (usb3_port_fake != -1) {
		TRACE(uphy->dev, "Faked map SSP%d to HSP%d for device mode\n",
			usb3_port_fake, port);
		reg = padctl_readl(uphy, XUSB_PADCTL_SS_PORT_MAP_0);
		reg &= ~SS_PORT_MAP(usb3_port_fake, SS_PORT_MAP_PORT_DISABLED);
		reg |= SS_PORT_MAP(usb3_port_fake, port);
		padctl_writel(uphy, reg, XUSB_PADCTL_SS_PORT_MAP_0);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~SSPX_ELPG_VCORE_DOWN(usb3_port_fake);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~SSPX_ELPG_CLAMP_EN_EARLY(usb3_port_fake);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~SSPX_ELPG_CLAMP_EN(usb3_port_fake);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);
	}

	sprintf(prod_name, "prod_c_utmi%d", port);
	err = tegra_prod_set_by_name(&uphy->padctl_regs, prod_name,
		uphy->prod_list);
	if (err) {
		dev_info(dev, "failed to apply prod for utmi pad%d (%d)\n",
			port, err);
	}

	sprintf(prod_name, "prod_c_bias");
	err = tegra_prod_set_by_name(&uphy->padctl_regs, prod_name,
		uphy->prod_list);
	if (err)
		dev_info(dev, "failed to apply prod for bias pad (%d)\n", err);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_PORT_CAP_0);
	reg &= ~(PORT_CAP_MASK << PORTX_CAP_SHIFT(port));
	if (uphy->utmi_ports[port].port_cap == CAP_DISABLED)
		reg |= (PORT_CAP_DISABLED << PORTX_CAP_SHIFT(port));
	else if (uphy->utmi_ports[port].port_cap == DEVICE_ONLY)
		reg |= (PORT_CAP_DEVICE << PORTX_CAP_SHIFT(port));
	else if (uphy->utmi_ports[port].port_cap == HOST_ONLY)
		reg |= (PORT_CAP_HOST << PORTX_CAP_SHIFT(port));
	else if (uphy->utmi_ports[port].port_cap == OTG)
		reg |= (PORT_CAP_OTG << PORTX_CAP_SHIFT(port));
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_PORT_CAP_0);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
	reg &= ~USB2_OTG_PD_ZI;
	reg &= ~HS_CURR_LEVEL(~0);
	if (uphy->calib.hs_curr_level_offset) {
		int hs_current_level;

		TRACE(uphy->dev, "UTMI port %d apply hs_curr_level_offset %d\n",
			port, uphy->calib.hs_curr_level_offset);

		hs_current_level = (int) uphy->calib.hs_curr_level[port] +
			uphy->calib.hs_curr_level_offset;

		if (hs_current_level < 0)
			hs_current_level = 0;
		if (hs_current_level > 0x3f)
			hs_current_level = 0x3f;

		reg |= HS_CURR_LEVEL(hs_current_level);
	} else
		reg |= HS_CURR_LEVEL(uphy->calib.hs_curr_level[port]);
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));
	reg &= ~TERM_RANGE_ADJ(~0);
	reg |= TERM_RANGE_ADJ(uphy->calib.hs_term_range_adj);
	reg &= ~RPD_CTRL(~0);
	reg |= RPD_CTRL(uphy->calib.rpd_ctrl);
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));

	reg = padctl_readl(uphy,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	if (uphy->utmi_ports[port].port_cap == HOST_ONLY)
		reg |= VREG_FIX18;
	else
		reg |= VREG_LEV(0x1);
	padctl_writel(uphy, reg,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	mutex_lock(&uphy->lock);

	if (uphy->utmi_enable++ > 0) {
		uphy->utmipll_use_count++;
		goto out;
	}

	uphy->utmipll_use_count++;
	tegra21x_utmi_phy_put_utmipll_out_iddq(uphy);
	tegra21x_usb2_trk_on(uphy);

out:
	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_utmi_phy_power_off(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = utmi_phy_to_port(phy);
	u32 reg;
	int usb3_port_fake;

	if (port < 0)
		return port;

	TRACE(uphy->dev, "power off UTMI port %d\n", port);

	usb3_port_fake = uphy->utmi_ports[port].usb3_port_fake;

	if (usb3_port_fake != -1) {
		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= SSPX_ELPG_CLAMP_EN_EARLY(usb3_port_fake);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= SSPX_ELPG_CLAMP_EN(usb3_port_fake);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		usleep_range(100, 200);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= SSPX_ELPG_VCORE_DOWN(usb3_port_fake);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);

		reg = padctl_readl(uphy, XUSB_PADCTL_SS_PORT_MAP_0);
		reg |= SS_PORT_MAP(usb3_port_fake,
					SS_PORT_MAP_PORT_DISABLED);
		padctl_writel(uphy, reg, XUSB_PADCTL_SS_PORT_MAP_0);
	}

	mutex_lock(&uphy->lock);

	if (--uphy->utmi_enable == 0)
		tegra21x_usb2_trk_off(uphy);
	if (--uphy->utmipll_use_count == 0)
		tegra21x_utmi_phy_put_utmipll_in_iddq(uphy);

	mutex_unlock(&uphy->lock);
	return 0;
}

int tegra21x_phy_xusb_utmi_vbus_power_on(struct phy *phy)
{
	struct tegra_padctl_uphy *padctl;
	int port;
	int rc;
	int status;

	if (!phy)
		return -EINVAL;

	padctl = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	status = regulator_is_enabled(padctl->vbus[port]);
	mutex_lock(&padctl->lock);
	if (padctl->vbus[port]) {
		rc = regulator_enable(padctl->vbus[port]);
		if (rc) {
			dev_err(padctl->dev, "enable port %d vbus failed %d\n",
				port, rc);
			mutex_unlock(&padctl->lock);
			return rc;
		}
	}
	mutex_unlock(&padctl->lock);
	dev_info(padctl->dev, "%s: port %d regulator status: %d->%d\n",
		 __func__, port, status,
		 regulator_is_enabled(padctl->vbus[port]));

	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_vbus_power_on);

int tegra21x_phy_xusb_utmi_vbus_power_off(struct phy *phy)
{
	struct tegra_padctl_uphy *padctl;
	int port;
	int rc;
	int status;

	if (!phy)
		return -EINVAL;

	padctl = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	status = regulator_is_enabled(padctl->vbus[port]);
	mutex_lock(&padctl->lock);
	if (padctl->vbus[port]) {
		rc = regulator_disable(padctl->vbus[port]);
		if (rc) {
			dev_err(padctl->dev, "disable port %d vbus failed %d\n",
				port, rc);
			mutex_unlock(&padctl->lock);
			return rc;
		}
	}
	mutex_unlock(&padctl->lock);
	dev_info(padctl->dev, "%s: port %d regulator status: %d->%d\n",
		 __func__, port, status,
		 regulator_is_enabled(padctl->vbus[port]));

	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_vbus_power_off);

int tegra21x_phy_xusb_overcurrent_detected(struct phy *phy)
{
	return 0;
}

void tegra21x_phy_xusb_handle_overcurrent(struct phy *phy)
{
}

static const struct phy_ops utmi_phy_ops = {
	.init = tegra21x_utmi_phy_init,
	.exit = tegra21x_utmi_phy_exit,
	.power_on = tegra21x_utmi_phy_power_on,
	.power_off = tegra21x_utmi_phy_power_off,
	.owner = THIS_MODULE,
};

static int cdp_phy_to_port(struct phy *phy)
{
	struct tegra_padctl_uphy *padctl = phy_get_drvdata(phy);
	unsigned int i;

	for (i = 0; i < TEGRA_CDP_PHYS; i++) {
		if (phy == padctl->cdp_phys[i])
			return i;
	}

	dev_warn(padctl->dev, "failed to get valid port number for cdp phy\n");

	return -EINVAL;
}

static int tegra210_cdp_phy_set_cdp(struct phy *phy, bool enable)
{
	struct tegra_padctl_uphy *padctl = phy_get_drvdata(phy);
	int port = cdp_phy_to_port(phy);
	u32 reg;

	dev_info(padctl->dev, "%sable UTMI port %d Tegra CDP\n",
		 enable ? "en" : "dis", port);
	if (enable) {
		reg = padctl_readl(padctl,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
		reg &= ~PD_CHG;
		padctl_writel(padctl, reg,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

		reg = padctl_readl(padctl,
				   XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
		reg |= (USB2_OTG_PD2 | USB2_OTG_PD2_OVRD_EN);
		padctl_writel(padctl, reg,
			      XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

		reg = padctl_readl(padctl,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
		reg |= ON_SRC_EN;
		padctl_writel(padctl, reg,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	} else {
		reg = padctl_readl(padctl,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
		reg |= PD_CHG;
		padctl_writel(padctl, reg,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

		reg = padctl_readl(padctl,
				   XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
		reg &= ~(USB2_OTG_PD2 | USB2_OTG_PD2_OVRD_EN);
		padctl_writel(padctl, reg,
			      XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

		reg = padctl_readl(padctl,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
		reg &= ~ON_SRC_EN;
		padctl_writel(padctl, reg,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	}

	return 0;
}

static int tegra210_cdp_phy_power_on(struct phy *phy)
{
	return tegra210_cdp_phy_set_cdp(phy, true);
}

static int tegra210_cdp_phy_power_off(struct phy *phy)
{
	return tegra210_cdp_phy_set_cdp(phy, false);
}

static const struct phy_ops cdp_phy_ops = {
	.power_on = tegra210_cdp_phy_power_on,
	.power_off = tegra210_cdp_phy_power_off,
	.owner = THIS_MODULE,
};

static inline bool is_utmi_phy(struct phy *phy)
{
	return phy->ops == &utmi_phy_ops;
}

static int hsic_phy_to_port(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	unsigned int i;

	for (i = 0; i < TEGRA_HSIC_PHYS; i++) {
		if (phy == uphy->hsic_phys[i])
			return i;
	}
	WARN_ON(1);

	return -EINVAL;
}

enum hsic_pad_pupd {
	PUPD_DISABLE = 0,
	PUPD_IDLE,
	PUPD_RESET
};

static int tegra21x_hsic_phy_pupd_set(struct tegra_padctl_uphy *uphy, int pad,
				      enum hsic_pad_pupd pupd)
{
	struct device *dev = uphy->dev;
	u32 reg;

	if (pad >= 1) {
		dev_err(dev, "%s invalid HSIC pad number %u\n", __func__, pad);
		return -EINVAL;
	}

	TRACE(dev, "pad %u pupd %d\n", pad, pupd);

	reg = padctl_readl(uphy, XUSB_PADCTL_HSIC_PADX_CTL_0(pad));
	reg &= ~(HSIC_RPD_DATA0 | HSIC_RPU_DATA0);
	reg &= ~(HSIC_RPU_STROBE | HSIC_RPD_STROBE);
	if (pupd == PUPD_IDLE) {
		reg |= (HSIC_RPD_DATA0 | HSIC_RPU_STROBE);
	} else if (pupd == PUPD_RESET) {
		reg |= (HSIC_RPD_DATA0 | HSIC_RPD_STROBE);
	} else if (pupd != PUPD_DISABLE) {
		dev_err(dev, "%s invalid pupd %d\n", __func__, pupd);
		return -EINVAL;
	}
	padctl_writel(uphy, reg, XUSB_PADCTL_HSIC_PADX_CTL_0(pad));

	return 0;
}

static ssize_t hsic_power_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_padctl_uphy *uphy = platform_get_drvdata(pdev);
	int pad = 0;
	u32 reg;
	int on;

	reg = padctl_readl(uphy, XUSB_PADCTL_HSIC_PADX_CTL_0(pad));

	if (reg & (HSIC_RPD_DATA0 | HSIC_RPD_STROBE))
		on = 0; /* bus in reset */
	else
		on = 1;

	return sprintf(buf, "%d\n", on);
}

static ssize_t hsic_power_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_padctl_uphy *uphy = platform_get_drvdata(pdev);
	struct tegra_xusb_mbox_msg msg;
	unsigned int on;
	int port;
	int rc;

	if (kstrtouint(buf, 10, &on))
		return -EINVAL;

	if (uphy->host_mode_phy_disabled) {
		dev_err(dev, "doesn't support HSIC PHY because mailbox is not available\n");
		return -EINVAL;
	}

	if (on)
		msg.cmd = MBOX_CMD_AIRPLANE_MODE_DISABLED;
	else
		msg.cmd = MBOX_CMD_AIRPLANE_MODE_ENABLED;

	port = uphy->soc->hsic_port_offset;
	msg.data = BIT(port + 1);
	rc = mbox_send_message(uphy->mbox_chan, &msg);
	if (rc < 0)
		dev_err(dev, "failed to send message to firmware %d\n", rc);

	if (on)
		rc = tegra21x_hsic_phy_pupd_set(uphy, 0, PUPD_IDLE);
	else
		rc = tegra21x_hsic_phy_pupd_set(uphy, 0, PUPD_RESET);

	return n;
}
static DEVICE_ATTR(hsic_power, S_IRUGO | S_IWUSR,
		   hsic_power_show, hsic_power_store);

static ssize_t otg_vbus_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_padctl_uphy *uphy = platform_get_drvdata(pdev);
	int port = uphy->utmi_otg_port_base_1 - 1;

	if (!uphy->utmi_otg_port_base_1)
		return sprintf(buf, "No UTMI OTG port\n");

	return sprintf(buf, "OTG port %d vbus always-on: %s\n",
			port, uphy->otg_vbus_alwayson ? "yes" : "no");
}

static ssize_t otg_vbus_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t n)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct tegra_padctl_uphy *uphy = platform_get_drvdata(pdev);
	int port = uphy->utmi_otg_port_base_1 - 1;
	unsigned int on;
	int err = 0;

	if (kstrtouint(buf, 10, &on))
		return -EINVAL;

	if (!uphy->utmi_otg_port_base_1) {
		dev_err(dev, "No UTMI OTG port\n");
		return -EINVAL;
	}

	if (!uphy->vbus[port]) {
		dev_err(dev, "UTMI OTG port %d has no vbus regulator\n", port);
		return -EINVAL;
	}

	if (on && !uphy->otg_vbus_alwayson) {
		err = regulator_enable(uphy->vbus[port]);
		if (!err)
			uphy->otg_vbus_alwayson = true;
	} else if (!on && uphy->otg_vbus_alwayson) {
		err = regulator_disable(uphy->vbus[port]);
		if (!err)
			uphy->otg_vbus_alwayson = false;
	}

	if (err)
		dev_err(dev, "failed to %s OTG port %d vbus always-on: %d\n",
				on ? "enable" : "disable", port, err);

	return n;
}

static DEVICE_ATTR(otg_vbus, S_IRUGO | S_IWUSR, otg_vbus_show, otg_vbus_store);

static struct attribute *padctl_uphy_attrs[] = {
	&dev_attr_hsic_power.attr,
	&dev_attr_otg_vbus.attr,
	NULL,
};
static struct attribute_group padctl_uphy_attr_group = {
	.attrs = padctl_uphy_attrs,
};

static int tegra21x_hsic_phy_pretend_connected(struct tegra_padctl_uphy *uphy
					, int port)
{
	struct device *dev = uphy->dev;
	struct tegra_xusb_mbox_msg msg;
	int rc;

	if (!uphy->hsic_ports[port].pretend_connected)
		return 0; /* pretend-connected is not enabled */

	msg.cmd = MBOX_CMD_HSIC_PRETEND_CONNECT;
	msg.data = BIT(uphy->soc->hsic_port_offset + port + 1);
	rc = mbox_send_message(uphy->mbox_chan, &msg);
	if (rc < 0)
		dev_err(dev, "failed to send message to firmware %d\n", rc);

	return rc;
}

static int tegra21x_hsic_phy_init(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = hsic_phy_to_port(phy);

	TRACE(uphy->dev, "phy init HSIC port %d\n", port);

	return 0;
}

static int tegra21x_hsic_phy_exit(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = hsic_phy_to_port(phy);

	TRACE(uphy->dev, "phy exit HSIC port %d\n", port);

	return 0;
}

static int tegra21x_hsic_phy_power_on(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = hsic_phy_to_port(phy);
	char prod_name[] = "prod_c_hsicX";
	int rc;

	mutex_lock(&uphy->lock);

	TRACE(uphy->dev, "power on HSIC port %d\n", port);
	if (port < 0)
		return port;

	sprintf(prod_name, "prod_c_hsic%d", port);
	rc = tegra_prod_set_by_name(&uphy->padctl_regs, prod_name,
				    uphy->prod_list);
	if (rc) {
		dev_info(uphy->dev, "failed to apply prod for hsic pad%d (%d)\n",
			port, rc);
	}

	rc = regulator_enable(uphy->vddio_hsic);
	if (rc) {
		dev_err(uphy->dev, "enable hsic %d power failed %d\n",
			port, rc);
		return rc;
	}

	if (uphy->hsic_enable++ > 0) {
		uphy->utmipll_use_count++;
		goto out;
	}

	uphy->utmipll_use_count++;
	tegra21x_utmi_phy_put_utmipll_out_iddq(uphy);
	tegra21x_hsic_trk_on(uphy);

out:
	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_hsic_phy_power_off(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port = utmi_phy_to_port(phy);
	int rc;

	mutex_lock(&uphy->lock);

	TRACE(uphy->dev, "power off HSIC port %d\n", port);
	if (port < 0)
		return port;

	if (WARN_ON(uphy->hsic_enable == 0))
		goto out;

	if (--uphy->hsic_enable == 0) {
		rc = regulator_disable(uphy->vddio_hsic);
		if (rc) {
			dev_err(uphy->dev, "disable hsic %d power failed %d\n",
					port, rc);
		}

		tegra21x_hsic_trk_off(uphy);
	}

	if (--uphy->utmipll_use_count == 0)
		tegra21x_utmi_phy_put_utmipll_in_iddq(uphy);

out:
	mutex_unlock(&uphy->lock);

	return 0;
}

static const struct phy_ops hsic_phy_ops = {
	.init = tegra21x_hsic_phy_init,
	.exit = tegra21x_hsic_phy_exit,
	.power_on = tegra21x_hsic_phy_power_on,
	.power_off = tegra21x_hsic_phy_power_off,
	.owner = THIS_MODULE,
};

static inline bool is_hsic_phy(struct phy *phy)
{
	return phy->ops == &hsic_phy_ops;
}

static void tegra_xusb_phy_mbox_work(struct work_struct *work)
{
	struct tegra_padctl_uphy *uphy = mbox_work_to_uphy(work);
	struct tegra_xusb_mbox_msg *msg = &uphy->mbox_req;
	struct tegra_xusb_mbox_msg resp;
	u32 ports;
	unsigned int i;
	unsigned long flags;

	TRACE(uphy->dev, "mailbox command %d\n", msg->cmd);
	resp.cmd = 0;
	switch (msg->cmd) {
	case MBOX_CMD_START_HSIC_IDLE:
	case MBOX_CMD_STOP_HSIC_IDLE:
		ports = msg->data >> (uphy->soc->hsic_port_offset + 1);
		resp.data = msg->data;
		resp.cmd = MBOX_CMD_ACK;
		if (msg->cmd == MBOX_CMD_START_HSIC_IDLE)
			tegra21x_hsic_phy_pupd_set(uphy, 0, PUPD_IDLE);
		else
			tegra21x_hsic_phy_pupd_set(uphy, 0, PUPD_DISABLE);
		break;
	case MBOX_CMD_DISABLE_SS_LFPS_DETECTION:
	case MBOX_CMD_ENABLE_SS_LFPS_DETECTION:

		if (!uphy->soc->usb3_phy_set_lfps_detector)
			break;

		ports = msg->data >> 1;
		resp.data = msg->data;
		resp.cmd = MBOX_CMD_ACK;
		for (i = 0; i < TEGRA_USB3_PHYS; i++) {
			if (!(ports & BIT(i)))
				continue;
			if (msg->cmd == MBOX_CMD_ENABLE_SS_LFPS_DETECTION) {
				spin_lock_irqsave(&uphy->spinlock, flags);
				uphy->soc->usb3_phy_set_lfps_detector(uphy, i,
									true);
				if (uphy->soc->disable_u0_ts1_detect)
					t210_receiver_detector(
						uphy->usb3_phys[i], true);
				spin_unlock_irqrestore(&uphy->spinlock, flags);
			} else {
				uphy->soc->usb3_phy_set_lfps_detector(uphy, i,
									false);
				/*
				 * Add this delay to increase stability of
				 * directing U3.
				 */
				usleep_range(500, 1000);
			}
		}
		break;
	default:
		break;
	}

	if (resp.cmd)
		mbox_send_message(uphy->mbox_chan, &resp);
}

static bool is_phy_mbox_message(u32 cmd)
{
	switch (cmd) {
	case MBOX_CMD_START_HSIC_IDLE:
	case MBOX_CMD_STOP_HSIC_IDLE:
	case MBOX_CMD_DISABLE_SS_LFPS_DETECTION:
	case MBOX_CMD_ENABLE_SS_LFPS_DETECTION:
		return true;
	default:
		return false;
	}
}

static void tegra_xusb_phy_mbox_rx(struct mbox_client *cl, void *data)
{
	struct tegra_padctl_uphy *uphy = dev_get_drvdata(cl->dev);
	struct tegra_xusb_mbox_msg *msg = data;

	if (is_phy_mbox_message(msg->cmd)) {
		uphy->mbox_req = *msg;
		schedule_work(&uphy->mbox_req_work);
	}
}

static struct phy *tegra21x_padctl_uphy_xlate(struct device *dev,
					   struct of_phandle_args *args)
{
	struct tegra_padctl_uphy *uphy = dev_get_drvdata(dev);
	unsigned int index = args->args[0];
	unsigned int phy_index;
	struct phy *phy = NULL;

	if (args->args_count <= 0)
		return ERR_PTR(-EINVAL);

	TRACE(dev, "index %d\n", index);

	if ((index >= TEGRA_PADCTL_UPHY_USB3_BASE) &&
		(index < TEGRA_PADCTL_UPHY_USB3_BASE + 16)) {

		phy_index = index - TEGRA_PADCTL_UPHY_USB3_BASE;
		if (phy_index < TEGRA_USB3_PHYS) {
			phy = uphy->usb3_phys[phy_index];
			TRACE(dev, "returning usb3_phys[%d]\n", phy_index);
		}
	} else if ((index >= TEGRA_PADCTL_UPHY_UTMI_BASE) &&
		(index < TEGRA_PADCTL_UPHY_UTMI_BASE + 16)) {

		phy_index = index - TEGRA_PADCTL_UPHY_UTMI_BASE;
		if (phy_index < TEGRA_UTMI_PHYS) {
			phy = uphy->utmi_phys[phy_index];
			TRACE(dev, "returning utmi_phys[%d]\n", phy_index);
		}
	} else if ((index >= TEGRA_PADCTL_UPHY_HSIC_BASE) &&
		(index < TEGRA_PADCTL_UPHY_HSIC_BASE + 16)) {

		phy_index = index - TEGRA_PADCTL_UPHY_HSIC_BASE;
		if (phy_index < TEGRA_HSIC_PHYS) {
			phy = uphy->hsic_phys[phy_index];
			TRACE(dev, "returning hsic_phys[%d]\n", phy_index);
		}
	} else if ((index >= TEGRA_PADCTL_UPHY_PCIE_BASE) &&
		(index < TEGRA_PADCTL_UPHY_PCIE_BASE + 16)) {

		phy_index = index - TEGRA_PADCTL_UPHY_PCIE_BASE;
		if (phy_index < TEGRA_PCIE_PHYS) {
			phy = uphy->pcie_phys[phy_index];
			TRACE(dev, "returning pcie_phys[%d]\n", phy_index);
		}
	} else if ((index >= TEGRA_PADCTL_UPHY_SATA_BASE) &&
		(index < TEGRA_PADCTL_UPHY_SATA_BASE + 16)) {

		phy_index = index - TEGRA_PADCTL_UPHY_SATA_BASE;
		if (phy_index < TEGRA_SATA_PHYS) {
			phy = uphy->sata_phys[phy_index];
			TRACE(dev, "returning sata_phys[%d]\n", phy_index);
		}
	} else if ((index >= TEGRA_PADCTL_PHY_CDP_BASE) &&
		(index < TEGRA_PADCTL_PHY_CDP_BASE + 16)) {

		phy_index = index - TEGRA_PADCTL_PHY_CDP_BASE;
		if (phy_index < TEGRA_CDP_PHYS)
			phy = uphy->cdp_phys[phy_index];
		uphy->cdp_used = true;
	}

	return (phy) ? phy : ERR_PTR(-EINVAL);
}

static const struct pinctrl_pin_desc tegra21x_pins[] = {
	PINCTRL_PIN(PIN_OTG_0,  "otg-0"),
	PINCTRL_PIN(PIN_OTG_1,  "otg-1"),
	PINCTRL_PIN(PIN_OTG_2,  "otg-2"),
	PINCTRL_PIN(PIN_OTG_3,  "otg-3"),
	PINCTRL_PIN(PIN_HSIC_0, "hsic-0"),
	PINCTRL_PIN(PIN_UPHY_0, "uphy-lane-0"),
	PINCTRL_PIN(PIN_UPHY_1, "uphy-lane-1"),
	PINCTRL_PIN(PIN_UPHY_2, "uphy-lane-2"),
	PINCTRL_PIN(PIN_UPHY_3, "uphy-lane-3"),
	PINCTRL_PIN(PIN_UPHY_4, "uphy-lane-4"),
	PINCTRL_PIN(PIN_UPHY_5, "uphy-lane-5"),
	PINCTRL_PIN(PIN_UPHY_6, "uphy-lane-6"),
	PINCTRL_PIN(PIN_SATA_0, "uphy-lane-7"),
	PINCTRL_PIN(PIN_CDP_0,  "cdp-0"),
	PINCTRL_PIN(PIN_CDP_1,  "cdp-1"),
	PINCTRL_PIN(PIN_CDP_2,  "cdp-2"),
	PINCTRL_PIN(PIN_CDP_3,  "cdp-3"),
};

static const struct pinctrl_pin_desc tegra21xb01_pins[] = {
	PINCTRL_PIN(T21xB01_PIN_OTG_0,  "otg-0"),
	PINCTRL_PIN(T21xB01_PIN_OTG_1,  "otg-1"),
	PINCTRL_PIN(T21xB01_PIN_OTG_2,  "otg-2"),
	PINCTRL_PIN(T21xB01_PIN_OTG_3,  "otg-3"),
	PINCTRL_PIN(T21xB01_PIN_UPHY_0, "uphy-lane-0"),
	PINCTRL_PIN(T21xB01_PIN_UPHY_1, "uphy-lane-1"),
	PINCTRL_PIN(T21xB01_PIN_UPHY_2, "uphy-lane-2"),
	PINCTRL_PIN(T21xB01_PIN_UPHY_3, "uphy-lane-3"),
	PINCTRL_PIN(T21xB01_PIN_UPHY_4, "uphy-lane-4"),
	PINCTRL_PIN(T21xB01_PIN_UPHY_5, "uphy-lane-5"),
	PINCTRL_PIN(T21xB01_PIN_CDP_0,  "cdp-0"),
	PINCTRL_PIN(T21xB01_PIN_CDP_1,  "cdp-1"),
	PINCTRL_PIN(T21xB01_PIN_CDP_2,  "cdp-2"),
	PINCTRL_PIN(T21xB01_PIN_CDP_3,  "cdp-3"),
};

static const char * const tegra21x_hsic_groups[] = {
	"hsic-0",
};

static const char * const tegra21x_hsic_plus_groups[] = {
	"hsic-0",
};

static const char * const tegra21x_xusb_groups[] = {
	"otg-0",
	"otg-1",
	"otg-2",
	"otg-3",
	"cdp-0",
	"cdp-1",
	"cdp-2",
	"cdp-3",
};

static const char * const tegra21x_pcie_groups[] = {
	"uphy-lane-0",
	"uphy-lane-1",
	"uphy-lane-2",
	"uphy-lane-3",
	"uphy-lane-4",
};

static const char * const tegra21x_usb3_groups[] = {
	"uphy-lane-0",
	"uphy-lane-3",
	"uphy-lane-4",
	"uphy-lane-5",
	"uphy-lane-6",
	"uphy-lane-7",
};

static const char * const tegra21xb01_usb3_groups[] = {
	"uphy-lane-1",
	"uphy-lane-4",
	"uphy-lane-5",
};

static const char * const tegra21x_sata_groups[] = {
	"uphy-lane-7",
};

#define TEGRA21x_FUNCTION(_name)					\
	{								\
		.name = #_name,						\
		.num_groups = ARRAY_SIZE(tegra21x_##_name##_groups),	\
		.groups = tegra21x_##_name##_groups,			\
	}

#define TEGRA21xB01_FUNCTION(_name)					\
	{								\
		.name = #_name,						\
		.num_groups = ARRAY_SIZE(tegra21xb01_##_name##_groups),	\
		.groups = tegra21xb01_##_name##_groups,			\
	}

static struct tegra_padctl_uphy_function tegra21x_functions[] = {
	TEGRA21x_FUNCTION(hsic),
	TEGRA21x_FUNCTION(xusb),
	TEGRA21x_FUNCTION(pcie),
	TEGRA21x_FUNCTION(usb3),
	TEGRA21x_FUNCTION(sata),
};

static struct tegra_padctl_uphy_function tegra21xb01_functions[] = {
	TEGRA21x_FUNCTION(xusb),
	TEGRA21x_FUNCTION(pcie),
	TEGRA21xB01_FUNCTION(usb3),
};

static const unsigned int tegra21x_otg_functions[] = {
	TEGRA21x_FUNC_XUSB,
};

static const unsigned int tegra21x_hsic_functions[] = {
	TEGRA21x_FUNC_HSIC,
};

static const unsigned int tegra21x_uphy_functions[] = {
	TEGRA21x_FUNC_USB3,
	TEGRA21x_FUNC_PCIE,
	TEGRA21x_FUNC_SATA,
};

static const unsigned int tegra21xb01_otg_functions[] = {
	TEGRA21xB01_FUNC_XUSB,
};

static const unsigned int tegra21xb01_uphy_functions[] = {
	TEGRA21xB01_FUNC_USB3,
	TEGRA21xB01_FUNC_PCIE,
};

#define TEGRA21x_LANE(_name, _offset, _shift, _mask, _funcs)	\
	{								\
		.name = _name,						\
		.offset = _offset,					\
		.shift = _shift,					\
		.mask = _mask,						\
		.num_funcs = ARRAY_SIZE(tegra21x_##_funcs##_functions),	\
		.funcs = tegra21x_##_funcs##_functions,			\
	}

#define TEGRA21xB01_LANE(_name, _offset, _shift, _mask, _funcs)	\
	{								\
		.name = _name,						\
		.offset = _offset,					\
		.shift = _shift,					\
		.mask = _mask,						\
		.num_funcs = ARRAY_SIZE(tegra21xb01_##_funcs##_functions),\
		.funcs = tegra21xb01_##_funcs##_functions,		\
	}

static const struct tegra_padctl_uphy_lane tegra21x_lanes[] = {
	/* XUSB_PADCTL_USB2_PAD_MUX_0 */
	TEGRA21x_LANE("otg-0",  0x004,  0, 0x3, otg),
	TEGRA21x_LANE("otg-1",  0x004,  2, 0x3, otg),
	TEGRA21x_LANE("otg-2",  0x004,  4, 0x3, otg),
	TEGRA21x_LANE("otg-3",  0x004,  6, 0x3, otg),
	TEGRA21x_LANE("hsic-0", 0x004, 20, 0x1, hsic),
	/* XUSB_PADCTL_USB3_PAD_MUX_0 */
	TEGRA21x_LANE("uphy-lane-0", 0x28, 12, 0x3, uphy),
	TEGRA21x_LANE("uphy-lane-1", 0x28, 14, 0x3, uphy),
	TEGRA21x_LANE("uphy-lane-2", 0x28, 16, 0x3, uphy),
	TEGRA21x_LANE("uphy-lane-3", 0x28, 18, 0x3, uphy),
	TEGRA21x_LANE("uphy-lane-4", 0x28, 20, 0x3, uphy),
	TEGRA21x_LANE("uphy-lane-5", 0x28, 22, 0x3, uphy),
	TEGRA21x_LANE("uphy-lane-6", 0x28, 24, 0x3, uphy),
	/* SATA_PAD_LANE0 */
	TEGRA21x_LANE("uphy-lane-7", 0x28, 30, 0x3, uphy),
};

static const struct tegra_padctl_uphy_lane tegra21xb01_lanes[] = {
	/* XUSB_PADCTL_USB2_PAD_MUX_0 */
	TEGRA21xB01_LANE("otg-0",  0x004,  0, 0x3, otg),
	TEGRA21xB01_LANE("otg-1",  0x004,  2, 0x3, otg),
	TEGRA21xB01_LANE("otg-2",  0x004,  4, 0x3, otg),
	TEGRA21xB01_LANE("otg-3",  0x004,  6, 0x3, otg),
	/* XUSB_PADCTL_USB3_PAD_MUX_0 */
	TEGRA21xB01_LANE("uphy-lane-0", 0x28, 12, 0x3, uphy),
	TEGRA21xB01_LANE("uphy-lane-1", 0x28, 14, 0x3, uphy),
	TEGRA21xB01_LANE("uphy-lane-2", 0x28, 16, 0x3, uphy),
	TEGRA21xB01_LANE("uphy-lane-3", 0x28, 18, 0x3, uphy),
	TEGRA21xB01_LANE("uphy-lane-4", 0x28, 20, 0x3, uphy),
	TEGRA21xB01_LANE("uphy-lane-5", 0x28, 22, 0x3, uphy),
};

static const char * const tegra21x_supply_names[] = {
	"avdd_pll_uerefe",
	"hvdd_pex_pll_e",
	"dvdd_pex_pll",
	"hvddio_pex",
	"dvddio_pex",
	"hvdd_sata",
	"dvdd_sata_pll",
	"hvddio_sata",
	"dvddio_sata",
};

static const char * const tegra21xb01_supply_names[] = {
	"avdd_pll_uerefe",
	"hvdd_pex_pll_e",
	"dvdd_pex_pll",
	"hvddio_pex",
	"dvddio_pex",
};

static const struct tegra_padctl_uphy_soc tegra21x_soc = {
	.num_pins = ARRAY_SIZE(tegra21x_pins),
	.pins = tegra21x_pins,
	.num_functions = ARRAY_SIZE(tegra21x_functions),
	.functions = tegra21x_functions,
	.num_lanes = ARRAY_SIZE(tegra21x_lanes),
	.lanes = tegra21x_lanes,
	.hsic_port_offset = 8,
	.supply_names = tegra21x_supply_names,
	.num_supplies = ARRAY_SIZE(tegra21x_supply_names),
	.usb3_phy_set_lfps_detector = tegra210_usb3_phy_set_lfps_detector,
	.disable_u0_ts1_detect = true,
};

static const struct tegra_padctl_uphy_soc tegra21xb01_soc = {
	.num_pins = ARRAY_SIZE(tegra21xb01_pins),
	.pins = tegra21xb01_pins,
	.num_functions = ARRAY_SIZE(tegra21xb01_functions),
	.functions = tegra21xb01_functions,
	.num_lanes = ARRAY_SIZE(tegra21xb01_lanes),
	.lanes = tegra21xb01_lanes,
	.hsic_port_offset = 8,
	.supply_names = tegra21xb01_supply_names,
	.num_supplies = ARRAY_SIZE(tegra21xb01_supply_names),
	.disable_u0_ts1_detect = false,
};

static const struct of_device_id tegra_padctl_uphy_of_match[] = {
	{
		.compatible = "nvidia,tegra21x-padctl-uphy",
		.data = &tegra21x_soc
	},
	{
		.compatible = "nvidia,tegra21xb01-padctl-uphy",
		.data = &tegra21xb01_soc
	},
	{ }
};
MODULE_DEVICE_TABLE(of, tegra_padctl_uphy_of_match);

static int tegra_xusb_read_fuse_calibration(struct tegra_padctl_uphy *uphy)
{
	struct platform_device *pdev = to_platform_device(uphy->dev);
	struct device_node *np = pdev->dev.of_node;
	unsigned int i;
	u32 reg;
	s32 v;

	tegra_fuse_readl(TEGRA_FUSE_SKU_CALIB_0, &reg);
	dev_info(uphy->dev, "TEGRA_FUSE_SKU_CALIB_0 0x%x\n", reg);
	for (i = 0; i < TEGRA_UTMI_PHYS; i++) {
		uphy->calib.hs_curr_level[i] =
			(reg >> HS_CURR_LEVEL_PADX_SHIFT(i)) &
			HS_CURR_LEVEL_PAD_MASK;
	}
	uphy->calib.hs_squelch = (reg >> HS_SQUELCH_SHIFT) & HS_SQUELCH_MASK;
	uphy->calib.hs_term_range_adj = (reg >> HS_TERM_RANGE_ADJ_SHIFT) &
					HS_TERM_RANGE_ADJ_MASK;

	tegra_fuse_readl(TEGRA_FUSE_USB_CALIB_EXT_0, &reg);
	dev_info(uphy->dev, "TEGRA_FUSE_USB_CALIB_EXT_0 0x%x\n", reg);
	uphy->calib.rpd_ctrl = (reg >> RPD_CTRL_SHIFT) & RPD_CTRL_MASK;

	if (of_property_read_s32(np, "nvidia,hs_curr_level_offset", &v) == 0) {
		TRACE(uphy->dev, "HS current level offset %d\n", v);
		uphy->calib.hs_curr_level_offset = v;
	}

	return 0;
}

static void tegra_xusb_otg_vbus_work(struct work_struct *work)
{
	struct tegra_padctl_uphy *uphy =
		container_of(work, struct tegra_padctl_uphy, otg_vbus_work);
	struct device *dev = uphy->dev;
	int port = uphy->utmi_otg_port_base_1 - 1;
	u32 reg;
	int rc;

	if (!uphy->utmi_otg_port_base_1)
		return; /* nothing to do if there is no UTMI otg port */

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_VBUS_ID);
	TRACE(dev, "USB2_VBUS_ID 0x%x otg_vbus_on was %d\n", reg,
		uphy->otg_vbus_on);
	if ((reg & ID_OVERRIDE(~0)) == ID_OVERRIDE_GROUNDED) {
		/* entering host mode role */
		if (uphy->vbus[port] && !uphy->otg_vbus_on) {
			rc = regulator_enable(uphy->vbus[port]);
			if (rc) {
				dev_err(dev, "failed to enable otg port vbus %d\n"
					, rc);
			}
			uphy->otg_vbus_on = true;
		}
	} else if ((reg & ID_OVERRIDE(~0)) == ID_OVERRIDE_FLOATING) {
		/* leaving host mode role */
		if (uphy->vbus[port] && uphy->otg_vbus_on) {
			rc = regulator_disable(uphy->vbus[port]);
			if (rc) {
				dev_err(dev, "failed to disable otg port vbus %d\n"
					, rc);
			}
			uphy->otg_vbus_on = false;
		}
	}
}

static int tegra_xusb_setup_usb(struct tegra_padctl_uphy *uphy)
{
	struct phy *phy;
	unsigned int i;

	for (i = 0; i < TEGRA_USB3_PHYS; i++) {
		if (uphy->usb3_ports[i].port_cap == CAP_DISABLED)
			continue;
		if (uphy->host_mode_phy_disabled &&
			(uphy->usb3_ports[i].port_cap == HOST_ONLY))
			continue; /* no mailbox support */

		phy = devm_phy_create(uphy->dev, NULL, &usb3_phy_ops);
		if (IS_ERR(phy))
			return PTR_ERR(phy);

		uphy->usb3_phys[i] = phy;
		phy_set_drvdata(phy, uphy);
	}

	for (i = 0; i < TEGRA_UTMI_PHYS; i++) {
		char reg_name[sizeof("vbus-N")];

		if (uphy->utmi_ports[i].port_cap == CAP_DISABLED)
			continue;
		if (uphy->host_mode_phy_disabled &&
			(uphy->utmi_ports[i].port_cap == HOST_ONLY))
			continue; /* no mailbox support */

		sprintf(reg_name, "vbus-%d", i);
		uphy->vbus[i] = devm_regulator_get_optional(uphy->dev,
							    reg_name);
		if (IS_ERR(uphy->vbus[i])) {
			if (PTR_ERR(uphy->vbus[i]) == -EPROBE_DEFER)
				return -EPROBE_DEFER;

			uphy->vbus[i] = NULL;
		}

		phy = devm_phy_create(uphy->dev, NULL, &utmi_phy_ops);
		if (IS_ERR(phy))
			return PTR_ERR(phy);

		uphy->utmi_phys[i] = phy;
		phy_set_drvdata(phy, uphy);
	}

	if (uphy->host_mode_phy_disabled)
		goto skip_hsic; /* no mailbox support */

	uphy->vddio_hsic = devm_regulator_get(uphy->dev, "vddio-hsic");
	if (IS_ERR(uphy->vddio_hsic))
		return PTR_ERR(uphy->vddio_hsic);

	for (i = 0; i < TEGRA_HSIC_PHYS; i++) {
		phy = devm_phy_create(uphy->dev, NULL, &hsic_phy_ops);
		if (IS_ERR(phy))
			return PTR_ERR(phy);

		uphy->hsic_phys[i] = phy;
		phy_set_drvdata(phy, uphy);
	}

	for (i = 0; i < TEGRA_CDP_PHYS; i++) {
		phy = devm_phy_create(uphy->dev, NULL, &cdp_phy_ops);
		if (IS_ERR(phy))
			return PTR_ERR(phy);

		uphy->cdp_phys[i] = phy;
		phy_set_drvdata(phy, uphy);
	}

skip_hsic:
	return 0;
}

#ifdef DEBUG
#define reg_dump(_dev, _base, _reg) \
	dev_dbg(_dev, "%s @%x = 0x%x\n", #_reg, _reg, ioread32(_base + _reg))
#else
#define reg_dump(_dev, _base, _reg)	do {} while (0)
#endif

static int tegra21x_uphy_pll_init(struct tegra_padctl_uphy *uphy)
{
	int rc;

	if (uphy->pcie_lanes) {
		rc = tegra21x_pcie_uphy_pll_init(uphy);
		if (rc)
			return rc;
	}

	if (uphy->usb3_lanes) {
		rc = tegra21x_usb3_uphy_pll_init(uphy);
		if (rc)
			return rc;
	}

	/* always enable SATA PLL HW sequencer to ensure PLLE could
	 * enable HW sequencer, set SATA PLL to SATA as default
	 */
	if (!(uphy->usb3_lanes & SATA_LANE_MASK)) {
		rc = tegra21x_sata_uphy_pll_init(uphy);
		if (rc)
			return rc;
	}

	return 0;
}

static void tegra21x_padctl_save(struct tegra_padctl_uphy *uphy)
{
	uphy->padctl_context.vbus_id =
					padctl_readl(uphy, XUSB_PADCTL_USB2_VBUS_ID);
	uphy->padctl_context.usb2_pad_mux =
					padctl_readl(uphy, XUSB_PADCTL_USB2_PAD_MUX_0);
	uphy->padctl_context.usb2_port_cap =
					padctl_readl(uphy, XUSB_PADCTL_USB2_PORT_CAP_0);
}

static void tegra21x_padctl_restore(struct tegra_padctl_uphy *uphy)
{
	padctl_writel(uphy, uphy->padctl_context.vbus_id,
						XUSB_PADCTL_USB2_VBUS_ID);
	padctl_writel(uphy, uphy->padctl_context.usb2_pad_mux,
						XUSB_PADCTL_USB2_PAD_MUX_0);
	padctl_writel(uphy, uphy->padctl_context.usb2_port_cap,
						XUSB_PADCTL_USB2_PORT_CAP_0);
}

static int tegra21x_padctl_uphy_suspend(struct device *dev)
{
	struct tegra_padctl_uphy *uphy = dev_get_drvdata(dev);

	TRACE(dev, "\n");

	tegra21x_padctl_save(uphy);

	clk_disable_unprepare(uphy->plle);
	uphy->plle_state = PLL_POWER_DOWN;
	uphy->uphy_pll_state[0] = UPHY_PLL_POWER_DOWN;
	uphy->uphy_pll_state[1] = UPHY_PLL_POWER_DOWN;

	return 0;
}

static int tegra21x_padctl_uphy_resume(struct device *dev)
{
	struct tegra_padctl_uphy *uphy = dev_get_drvdata(dev);

	TRACE(dev, "\n");

	tegra21x_padctl_restore(uphy);

	if (tegra210_plle_hw_sequence_is_enabled()) {
		/* PLLE in HW when .resume_noirq being called indicated system
		 * didn't reach SC7. Hence skip PLL init and invoke
		 * clk_prepare_enable(uphy->plle) to update PLLE enable_count.
		 * Please note that clk_plle_tegra210_enable won't update PLLE
		 * regs if PLLE is in HW.
		 */
		TRACE(dev, "skip PLL init as PLLE in HW");
		uphy->uphy_pll_state[0] = UPHY_PLL_POWER_UP_HW_SEQ;
		uphy->uphy_pll_state[1] = UPHY_PLL_POWER_UP_HW_SEQ;
		uphy->plle_state = PLL_POWER_UP_HW_SEQ;
		return clk_prepare_enable(uphy->plle);
	} else
		return tegra21x_uphy_pll_init(uphy);
}

static const struct dev_pm_ops tegra21x_padctl_uphy_pm_ops = {
	.suspend_noirq = tegra21x_padctl_uphy_suspend,
	.resume_noirq = tegra21x_padctl_uphy_resume,
};

static int tegra21x_padctl_uphy_probe(struct platform_device *pdev)
{
	struct tegra_padctl_uphy *uphy;
	const struct of_device_id *match;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct phy *phy;
	int i;
	int err;

	uphy = devm_kzalloc(dev, sizeof(*uphy), GFP_KERNEL);
	if (!uphy)
		return -ENOMEM;

	platform_set_drvdata(pdev, uphy);
	mutex_init(&uphy->lock);
	uphy->dev = dev;

	match = of_match_node(tegra_padctl_uphy_of_match, pdev->dev.of_node);
	if (!match)
		return -ENODEV;
	uphy->soc = match->data;

	/* get registers */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "padctl");
	uphy->padctl_regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(uphy->padctl_regs))
		return PTR_ERR(uphy->padctl_regs);
	dev_info(dev, "padctl mmio start %pa end %pa\n",
		 &res->start, &res->end);

	/* XUSB_PADCTL_UPHY_PLL_P0_CTL_1_0 */
	uphy->uphy_pll_regs[0]	= uphy->padctl_regs + 0x360;
	/* XUSB_PADCTL_UPHY_MISC_PAD_P0_CTL_1_0 */
	uphy->uphy_lane_regs[0]	= uphy->padctl_regs + 0x460;
	uphy->uphy_lane_regs[1]	= uphy->padctl_regs + 0x4a0;
	uphy->uphy_lane_regs[2]	= uphy->padctl_regs + 0x4e0;
	uphy->uphy_lane_regs[3]	= uphy->padctl_regs + 0x520;
	uphy->uphy_lane_regs[4]	= uphy->padctl_regs + 0x560;
	uphy->uphy_lane_regs[5]	= uphy->padctl_regs + 0x5a0;
	uphy->uphy_lane_regs[6]	= uphy->padctl_regs + 0x5e0;
	/* XUSB_PADCTL_UPHY_PLL_S0_CTL_1_0 */
	uphy->uphy_pll_regs[1]	= uphy->padctl_regs + 0x860;
	/* XUSB_PADCTL_UPHY_MISC_PAD_S0_CTL_1_0 */
	uphy->uphy_lane_regs[7]	= uphy->padctl_regs + 0x960;

	if (tegra_platform_is_silicon()) {
		err = tegra_xusb_read_fuse_calibration(uphy);
		if (err < 0)
			return err;
	}

	/* get UPHY PLL resets */
	uphy->uphy_pll_rst[0] = devm_reset_control_get(dev, "pex_uphy");
	if (IS_ERR(uphy->uphy_pll_rst[0])) {
		dev_err(uphy->dev, "failed to get pex uphy reset\n");
		return PTR_ERR(uphy->uphy_pll_rst[0]);
	}

	uphy->uphy_pll_rst[1] = devm_reset_control_get(dev, "sata_uphy");
	if (IS_ERR(uphy->uphy_pll_rst[1])) {
		dev_err(uphy->dev, "failed to get sata uphy reset\n");
		return PTR_ERR(uphy->uphy_pll_rst[1]);
	}

	/* get other resets */
	uphy->padctl_rst = devm_reset_control_get(dev, "padctl");
	if (IS_ERR(uphy->padctl_rst)) {
		dev_err(uphy->dev, "failed to get padctl reset\n");
		return PTR_ERR(uphy->padctl_rst);
	}

	/* get clocks */
	uphy->plle = devm_clk_get(dev, "pll_e");
	if (IS_ERR(uphy->plle)) {
		dev_err(dev, "failed to get plle clock\n");
		return PTR_ERR(uphy->plle);
	}

	uphy->usb2_trk_clk = devm_clk_get(dev, "usb2_trk");
	if (IS_ERR(uphy->usb2_trk_clk)) {
		dev_err(dev, "failed to get usb2_trk clock\n");
		return PTR_ERR(uphy->usb2_trk_clk);
	}

	uphy->hsic_trk_clk = devm_clk_get(dev, "hsic_trk");
	if (IS_ERR(uphy->hsic_trk_clk)) {
		dev_err(dev, "failed to get hsic_trk clock\n");
		return PTR_ERR(uphy->hsic_trk_clk);
	}

	/* init regulators */
	err = tegra21x_padctl_uphy_regulators_init(uphy);
	if (err < 0)
		return err;

	err = regulator_bulk_enable(uphy->soc->num_supplies, uphy->supplies);
	if (err) {
		dev_err(dev, "failed to enable regulators %d\n", err);
		return err;
	}

	/* deassert resets */
	err = reset_control_deassert(uphy->padctl_rst);
	if (err) {
		dev_err(dev, "failed to deassert padctl_rst %d\n", err);
		goto disable_regulators;
	}

	memset(&uphy->desc, 0, sizeof(uphy->desc));
	uphy->desc.name = dev_name(dev);
	uphy->desc.pins = uphy->soc->pins;
	uphy->desc.npins = uphy->soc->num_pins;
	uphy->desc.pctlops = &tegra_xusb_padctl_pinctrl_ops;
	uphy->desc.pmxops = &tegra21x_padctl_uphy_pinmux_ops;
	uphy->desc.confops = &tegra_padctl_uphy_pinconf_ops;
	uphy->desc.owner = THIS_MODULE;

	/* initialize uphy_lane as non-zero value otherwise
	 * uphy-lane-0 might be mapped to incorrect port
	 */
	for (i = 0; i < TEGRA_USB3_PHYS; i++)
		uphy->usb3_ports[i].uphy_lane = T21x_UPHY_LANES;

	for (i = 0; i < TEGRA_UTMI_PHYS; i++)
		uphy->utmi_ports[i].usb3_port_fake = -1;

	uphy->pinctrl = pinctrl_register(&uphy->desc, &pdev->dev, uphy);
	if (!uphy->pinctrl) {
		dev_err(&pdev->dev, "failed to register pinctrl\n");
		err = -ENODEV;
		goto assert_padctl_rst;
	}

	for (i = 0; i < TEGRA_PCIE_PHYS; i++) {
		phy = devm_phy_create(dev, NULL, &pcie_phy_ops);
		if (IS_ERR(phy)) {
			err = PTR_ERR(phy);
			goto unregister;
		}
		uphy->pcie_phys[i] = phy;
		phy_set_drvdata(phy, uphy);
	}

	if (uphy->sata_lanes) {
		phy = devm_phy_create(dev, NULL, &sata_phy_ops);
		if (IS_ERR(phy)) {
			err = PTR_ERR(phy);
			goto unregister;
		}
		uphy->sata_phys[0] = phy;
		phy_set_drvdata(phy, uphy);
	}

	INIT_WORK(&uphy->otg_vbus_work, tegra_xusb_otg_vbus_work);
	INIT_WORK(&uphy->mbox_req_work, tegra_xusb_phy_mbox_work);
	uphy->mbox_client.dev = dev;
	uphy->mbox_client.tx_block = true;
	uphy->mbox_client.tx_tout = 0;
	uphy->mbox_client.rx_callback = tegra_xusb_phy_mbox_rx;
	uphy->mbox_chan = mbox_request_channel(&uphy->mbox_client, 0);
	if (IS_ERR(uphy->mbox_chan)) {
		err = PTR_ERR(uphy->mbox_chan);
		if (err == -EPROBE_DEFER) {
			dev_info(&pdev->dev, "mailbox is not ready yet\n");
			goto unregister;
		} else {
			dev_warn(&pdev->dev,
				 "failed to get mailbox, USB Host PHY support disabled\n");
			uphy->host_mode_phy_disabled = true;
		}
	}

	err = tegra_xusb_setup_usb(uphy);
	if (err)
		goto free_mailbox;

	uphy->provider = devm_of_phy_provider_register(dev,
					tegra21x_padctl_uphy_xlate);
	if (IS_ERR(uphy->provider)) {
		err = PTR_ERR(uphy->provider);
		dev_err(&pdev->dev, "failed to register PHYs: %d\n", err);
		goto free_mailbox;
	}

	err = sysfs_create_group(&pdev->dev.kobj, &padctl_uphy_attr_group);
	if (err) {
		dev_err(&pdev->dev, "cannot create sysfs group: %d\n", err);
		goto free_mailbox;
	}

	uphy->prod_list = devm_tegra_prod_get(&pdev->dev);
	if (IS_ERR(uphy->prod_list)) {
		dev_warn(&pdev->dev, "Prod-settings not available\n");
		uphy->prod_list = NULL;
	}

	uphy->sata_bypass_fuse =
		of_property_read_bool(np, "nvidia,sata-use-prods");

	err = tegra21x_uphy_pll_init(uphy);
	if (err) {
		dev_err(dev, "failed to initialize UPHY PLLs %d\n", err);
		goto remove_sysfs;
	}

	dev_info(&pdev->dev, "Done tegra21x_padctl_uphy_probe\n");
	return 0;

remove_sysfs:
	sysfs_remove_group(&pdev->dev.kobj, &padctl_uphy_attr_group);
free_mailbox:
	if (!IS_ERR(uphy->mbox_chan)) {
		cancel_work_sync(&uphy->mbox_req_work);
		mbox_free_channel(uphy->mbox_chan);
	}
unregister:
	pinctrl_unregister(uphy->pinctrl);
assert_padctl_rst:
	reset_control_assert(uphy->padctl_rst);
disable_regulators:
	regulator_bulk_disable(uphy->soc->num_supplies, uphy->supplies);
	return err;
}

static int tegra21x_padctl_uphy_remove(struct platform_device *pdev)
{
	struct tegra_padctl_uphy *uphy = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &padctl_uphy_attr_group);

	if (!IS_ERR(uphy->mbox_chan)) {
		cancel_work_sync(&uphy->mbox_req_work);
		mbox_free_channel(uphy->mbox_chan);
	}

	pinctrl_unregister(uphy->pinctrl);
	reset_control_assert(uphy->padctl_rst);
	regulator_bulk_disable(uphy->soc->num_supplies, uphy->supplies);
	uphy->cdp_used = false;

	return 0;
}

static struct platform_driver tegra21x_padctl_uphy_driver = {
	.driver = {
		.name = "tegra21x-padctl-uphy",
		.of_match_table = tegra_padctl_uphy_of_match,
		.pm = &tegra21x_padctl_uphy_pm_ops,
	},
	.probe = tegra21x_padctl_uphy_probe,
	.remove = tegra21x_padctl_uphy_remove,
};
module_platform_driver(tegra21x_padctl_uphy_driver);

/* Tegra Generic PHY Extensions */
void tegra21x_phy_xusb_utmi_pad_power_on(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
	reg &= ~(USB2_OTG_PD | USB2_OTG_PD2);
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));
	reg &= ~USB2_OTG_PD_DR;
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_power_on);

void tegra21x_phy_xusb_utmi_pad_power_down(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
	reg |= (USB2_OTG_PD | USB2_OTG_PD2);
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));
	reg |= USB2_OTG_PD_DR;
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_1(port));
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_power_down);

void tegra21x_phy_xusb_set_dcd_debounce_time(struct phy *phy, u32 val)
{
	struct tegra_padctl_uphy *uphy;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);

	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_TDCD_DBNC_TIMER_0);
	reg &= ~TDCD_DBNC(~0);
	reg |= TDCD_DBNC(val);
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_TDCD_DBNC_TIMER_0);
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_set_dcd_debounce_time);

u32 tegra21x_phy_xusb_noncompliant_div_detect(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	reg |= DIV_DET_EN;
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	udelay(10);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	reg &= ~DIV_DET_EN;
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	dev_dbg(uphy->dev, "reg = 0x%x", reg);
	return reg;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_noncompliant_div_detect);

void tegra21x_phy_xusb_utmi_pad_charger_detect_on(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	/* power up necessary stuff */
	tegra21x_usb2_trk_on(uphy);

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
	reg &= ~USB2_OTG_PD;
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
	reg |= (USB2_OTG_PD2 | USB2_OTG_PD2_OVRD_EN);
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg &= ~PD_CHG;
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	/* Set DP/DN Pull up/down to zero by default */
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	reg |= (USBOP_RPD_OVRD | USBOP_RPU_OVRD |
	USBON_RPD_OVRD | USBON_RPU_OVRD);
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	reg &= ~(USBOP_RPD_OVRD_VAL | USBOP_RPU_OVRD_VAL |
		USBON_RPD_OVRD_VAL | USBON_RPU_OVRD_VAL);
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	/* Disable DP/DN as src/sink */
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg &= ~(OP_SRC_EN | ON_SINK_EN |
		ON_SRC_EN | OP_SINK_EN);
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_charger_detect_on);

void tegra21x_phy_xusb_utmi_pad_charger_detect_off(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	reg &= ~(USBOP_RPD_OVRD | USBOP_RPU_OVRD |
		USBON_RPD_OVRD | USBON_RPU_OVRD);
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	/* power down necessary stuff */
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg |= PD_CHG;
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
	reg &= ~(USB2_OTG_PD2_OVRD_EN | USB2_OTG_PD2);
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));
	reg |= USB2_OTG_PD;
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_OTG_PADX_CTL_0(port));

	tegra21x_usb2_trk_off(uphy);
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_charger_detect_off);

void tegra21x_phy_xusb_utmi_pad_enable_detect_filters(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	reg = padctl_readl(uphy,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg |= (VDCD_DET_FILTER_EN | VDAT_DET_FILTER_EN |
	ZIP_FILTER_EN | ZIN_FILTER_EN);
	padctl_writel(uphy, reg,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_enable_detect_filters);

void tegra21x_phy_xusb_utmi_pad_disable_detect_filters(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	reg = padctl_readl(uphy,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg &= ~(VDCD_DET_FILTER_EN | VDAT_DET_FILTER_EN |
	ZIP_FILTER_EN | ZIN_FILTER_EN);
	padctl_writel(uphy, reg,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_disable_detect_filters);

bool tegra21x_phy_xusb_utmi_pad_primary_charger_detect(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;
	bool ret;

	if (!phy)
		return false;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	/* Source D+ to D- */
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg |= OP_SRC_EN | ON_SINK_EN;
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	/* Wait for TVDPSRC_ON */
	msleep(40);

	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	ret = !!(reg & VDAT_DET);

	/* Turn off OP_SRC, ON_SINK, clear VDAT, ZIN status change */
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg &= ~(OP_SRC_EN | ON_SINK_EN);
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	return ret;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_primary_charger_detect);

bool tegra21x_phy_xusb_utmi_pad_secondary_charger_detect(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;
	bool ret;

	if (!phy)
		return false;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	/* Source D- to D+ */
	reg = padctl_readl(uphy,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg |= ON_SRC_EN | OP_SINK_EN;
	padctl_writel(uphy, reg,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	/* Wait for TVDPSRC_ON */
	msleep(40);

	reg = padctl_readl(uphy,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	ret = !(reg & VDAT_DET);

	/* Turn off ON_SRC, OP_SINK, clear VDAT, ZIP status change */
	reg = padctl_readl(uphy,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg &= ~(ON_SRC_EN | OP_SINK_EN);
	padctl_writel(uphy, reg,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	return ret;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_secondary_charger_detect);

/* ignoring dir since T210 doesn't support VREG_DIR bits */
void tegra21x_phy_xusb_utmi_pad_set_protection_level(struct phy *phy, int level,
						  enum tegra_vbus_dir dir)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	reg = padctl_readl(uphy,
	XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	if (level < 0) {
		/* disable pad protection */
		reg |= VREG_FIX18;
		reg &= ~VREG_LEV(~0);
	} else {
		reg &= ~VREG_FIX18;
		reg &= ~VREG_LEV(~0);
		reg |= VREG_LEV(level);
	}
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_set_protection_level);

bool tegra21x_phy_xusb_utmi_pad_dcd(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;
	u32 reg;
	int dcd_timeout_ms = 0;
	bool ret = false;

	if (!phy)
		return false;

	uphy = phy_get_drvdata(phy);
	port = utmi_phy_to_port(phy);

	/* data contact detection */
	/* Turn on IDP_SRC */
	dev_dbg(uphy->dev, "DCD enabling OP_I_SRC_EN");
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg |= OP_I_SRC_EN;
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	/* Turn on D- pull-down resistor */
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	reg |= USBON_RPD_OVRD_VAL;
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	/* Wait for TDCD_DBNC */
	usleep_range(10000, 120000);

	while (dcd_timeout_ms < TDCD_TIMEOUT_MS) {
		reg = padctl_readl(uphy,
			XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
		if (reg & DCD_DETECTED) {
			dev_dbg(uphy->dev, "USB2 port %d DCD successful\n",
				port);
			ret = true;
			break;
		}
		usleep_range(20000, 22000);
		dcd_timeout_ms += 22;
	}

	if (!ret)
		dev_info(uphy->dev, "%s:%d DCD timeout %d ms\n",
				__func__, __LINE__, dcd_timeout_ms);

	/* Turn off IP_SRC, clear DCD DETECTED*/
	dev_dbg(uphy->dev, "DCD disabling OP_I_SRC_EN");
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));
	reg &= ~OP_I_SRC_EN;
	reg |= DCD_DETECTED;
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL0(port));

	/* Turn off D- pull-down resistor */
	reg = padctl_readl(uphy,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));
	reg &= ~USBON_RPD_OVRD_VAL;
	padctl_writel(uphy, reg,
		XUSB_PADCTL_USB2_BATTERY_CHRG_OTGPADX_CTL1(port));

	return ret;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_utmi_pad_dcd);

int tegra21x_phy_xusb_enable_sleepwalk(struct phy *phy,
				    enum usb_device_speed speed)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port;

	if (is_utmi_phy(phy)) {
		port = utmi_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		tegra21x_utmi_phy_get_pad_config(uphy, port,
							&uphy->utmi_pad_cfg);
		return tegra_pmc_utmi_phy_enable_sleepwalk(port, speed,
							&uphy->utmi_pad_cfg);
	} else if (is_hsic_phy(phy)) {
		port = hsic_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra_pmc_hsic_phy_enable_sleepwalk(port);
	} else if (is_usb3_phy(phy)) {
		port = usb3_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_usb3_phy_enable_wakelogic(uphy, port);
	} else
		return -EINVAL;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_enable_sleepwalk);

int tegra21x_phy_xusb_disable_sleepwalk(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy = phy_get_drvdata(phy);
	int port;

	if (is_utmi_phy(phy)) {
		port = utmi_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra_pmc_utmi_phy_disable_sleepwalk(port);
	} else if (is_hsic_phy(phy)) {
		port = hsic_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra_pmc_hsic_phy_disable_sleepwalk(port);
	} else if (is_usb3_phy(phy)) {
		port = usb3_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_usb3_phy_disable_wakelogic(uphy, port);
	} else
		return -EINVAL;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_disable_sleepwalk);

static int tegra21x_padctl_vbus_override(struct tegra_padctl_uphy *uphy,
					 bool on)
{
	u32 reg;

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_VBUS_ID);
	if (on) {
		reg |= VBUS_OVERRIDE_VBUS_ON;
		reg &= ~ID_OVERRIDE(~0);
		reg |= ID_OVERRIDE_FLOATING;
	} else
		reg &= ~VBUS_OVERRIDE_VBUS_ON;
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_VBUS_ID);

	schedule_work(&uphy->otg_vbus_work);

	return 0;
}

int tegra21x_phy_xusb_set_vbus_override(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	return tegra21x_padctl_vbus_override(uphy, true);
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_set_vbus_override);

int tegra21x_phy_xusb_clear_vbus_override(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	return tegra21x_padctl_vbus_override(uphy, false);
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_clear_vbus_override);

static int tegra21x_padctl_id_override(struct tegra_padctl_uphy *uphy,
					 bool grounded)
{
	u32 reg;

	reg = padctl_readl(uphy, XUSB_PADCTL_USB2_VBUS_ID);
	if (grounded) {
		if (reg & VBUS_OVERRIDE_VBUS_ON) {
			reg &= ~VBUS_OVERRIDE_VBUS_ON;
			padctl_writel(uphy, reg, XUSB_PADCTL_USB2_VBUS_ID);
			usleep_range(1000, 2000);

			reg = padctl_readl(uphy, XUSB_PADCTL_USB2_VBUS_ID);
		}

		reg &= ~ID_OVERRIDE(~0);
		reg |= ID_OVERRIDE_GROUNDED;
	} else {
		reg &= ~ID_OVERRIDE(~0);
		reg |= ID_OVERRIDE_FLOATING;
	}
	padctl_writel(uphy, reg, XUSB_PADCTL_USB2_VBUS_ID);

	schedule_work(&uphy->otg_vbus_work);

	return 0;
}

int tegra21x_phy_xusb_set_id_override(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	return tegra21x_padctl_id_override(uphy, true);
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_set_id_override);

int tegra21x_phy_xusb_clear_id_override(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	return tegra21x_padctl_id_override(uphy, false);
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_clear_id_override);

bool tegra21x_phy_xusb_has_otg_cap(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;

	if (!phy)
		return false;

	uphy = phy_get_drvdata(phy);
	if (is_utmi_phy(phy)) {
		if ((uphy->utmi_otg_port_base_1) &&
			uphy->utmi_phys[uphy->utmi_otg_port_base_1 - 1] == phy)
			return true;
	} else if (is_usb3_phy(phy)) {
		if ((uphy->usb3_otg_port_base_1) &&
			uphy->usb3_phys[uphy->usb3_otg_port_base_1 - 1] == phy)
			return true;
	}

	return false;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_has_otg_cap);

static int tegra21x_usb3_phy_set_wake(struct tegra_padctl_uphy *uphy,
					 int port, bool enable)
{
	u32 reg;

	mutex_lock(&uphy->lock);
	if (enable) {
		TRACE(uphy->dev, "enable USB3 port %d wake\n", port);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= SS_PORT_WAKEUP_EVENT(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= SS_PORT_WAKE_INTERRUPT_ENABLE(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= SSPX_ELPG_VCORE_DOWN(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);
	} else {
		TRACE(uphy->dev, "disable USB3 port %d wake\n", port);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg &= ~SS_PORT_WAKE_INTERRUPT_ENABLE(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= SS_PORT_WAKEUP_EVENT(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~SSPX_ELPG_VCORE_DOWN(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);
	}
	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_utmi_phy_set_wake(struct tegra_padctl_uphy *uphy,
					 int port, bool enable)
{
	u32 reg;

	mutex_lock(&uphy->lock);
	if (enable) {
		TRACE(uphy->dev, "enable UTMI port %d wake\n", port);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= USB2_PORT_WAKEUP_EVENT(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= USB2_PORT_WAKE_INTERRUPT_ENABLE(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	} else {
		TRACE(uphy->dev, "disable UTMI port %d wake\n", port);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg &= ~USB2_PORT_WAKE_INTERRUPT_ENABLE(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= USB2_PORT_WAKEUP_EVENT(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);
	}
	mutex_unlock(&uphy->lock);

	return 0;
}

static int tegra21x_hsic_phy_set_wake(struct tegra_padctl_uphy *uphy,
					 int port, bool enable)
{
	u32 reg;

	mutex_lock(&uphy->lock);
	if (enable) {
		TRACE(uphy->dev, "enable HSIC port %d wake\n", port);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= USB2_HSIC_PORT_WAKEUP_EVENT(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg |= USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

	} else {
		TRACE(uphy->dev, "disable HSIC port %d wake\n", port);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg &= ~USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);

		usleep_range(10, 20);

		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
		reg &= ~ALL_WAKE_EVENTS;
		reg |= USB2_HSIC_PORT_WAKEUP_EVENT(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_0);
	}
	mutex_unlock(&uphy->lock);

	return 0;
}

int tegra21x_phy_xusb_enable_wake(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;


	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	if (is_utmi_phy(phy)) {
		port = utmi_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_utmi_phy_set_wake(uphy, port, true);
	} else if (is_hsic_phy(phy)) {
		port = hsic_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_hsic_phy_set_wake(uphy, port, true);
	} else if (is_usb3_phy(phy)) {
		port = usb3_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_usb3_phy_set_wake(uphy, port, true);
	} else
		return -EINVAL;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_enable_wake);

int tegra21x_phy_xusb_disable_wake(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	if (is_utmi_phy(phy)) {
		port = utmi_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_utmi_phy_set_wake(uphy, port, false);
	} else if (is_hsic_phy(phy)) {
		port = hsic_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_hsic_phy_set_wake(uphy, port, false);
	} else if (is_usb3_phy(phy)) {
		port = usb3_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_usb3_phy_set_wake(uphy, port, false);
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_disable_wake);

static int tegra21x_usb3_phy_remote_wake_detected(struct tegra_padctl_uphy *uphy
					, int port)
{
	u32 reg;

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
	if ((reg & SS_PORT_WAKE_INTERRUPT_ENABLE(port)) &&
			(reg & SS_PORT_WAKEUP_EVENT(port)))
		return true;
	else
		return false;
}

static int tegra21x_utmi_phy_remote_wake_detected(struct tegra_padctl_uphy *uphy
					, int port)
{
	u32 reg;

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
	if ((reg & USB2_PORT_WAKE_INTERRUPT_ENABLE(port)) &&
			(reg & USB2_PORT_WAKEUP_EVENT(port)))
		return true;
	else
		return false;
}

static int tegra21x_hsic_phy_remote_wake_detected(struct tegra_padctl_uphy *uphy
					, int port)
{
	u32 reg;

	reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_0);
	if ((reg & USB2_HSIC_PORT_WAKE_INTERRUPT_ENABLE(port)) &&
			(reg & USB2_HSIC_PORT_WAKEUP_EVENT(port)))
		return true;
	else
		return false;
}

int tegra21x_phy_xusb_remote_wake_detected(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	if (is_utmi_phy(phy)) {
		port = utmi_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_utmi_phy_remote_wake_detected(uphy, port);
	} else if (is_hsic_phy(phy)) {
		port = hsic_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_hsic_phy_remote_wake_detected(uphy, port);
	} else if (is_usb3_phy(phy)) {
		port = usb3_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_usb3_phy_remote_wake_detected(uphy, port);
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_remote_wake_detected);

int tegra21x_phy_xusb_pretend_connected(struct phy *phy)
{
	struct tegra_padctl_uphy *uphy;
	int port;

	if (!phy)
		return 0;

	uphy = phy_get_drvdata(phy);

	/* applicable to HSIC only */
	if (is_hsic_phy(phy)) {
		port = hsic_phy_to_port(phy);
		if (port < 0)
			return -EINVAL;
		return tegra21x_hsic_phy_pretend_connected(uphy, port);
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_pretend_connected);

void t210_clamp_en_early(struct phy *phy, bool on)
{
	struct tegra_padctl_uphy *uphy;
	u32 reg;
	int port;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = usb3_phy_to_port(phy);

	if (port < 0) {
		dev_err(uphy->dev, "Invalid port number\n");
		return;
	}

	if ((on && uphy->usb3_ports[port].clamp_en_early_enabled) ||
		(!on && !uphy->usb3_ports[port].clamp_en_early_enabled))
		return;

	uphy->usb3_ports[port].clamp_en_early_enabled = on;

	if (on) {
		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg |= SSPX_ELPG_CLAMP_EN_EARLY(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);
	} else {
		reg = padctl_readl(uphy, XUSB_PADCTL_ELPG_PROGRAM_1);
		reg &= ~SSPX_ELPG_CLAMP_EN_EARLY(port);
		padctl_writel(uphy, reg, XUSB_PADCTL_ELPG_PROGRAM_1);
	}
}
EXPORT_SYMBOL_GPL(t210_clamp_en_early);

void t210_receiver_detector(struct phy *phy, bool on)
{
	struct tegra_padctl_uphy *uphy;
	u32 mask, reg;
	unsigned int uphy_lane;
	int port;

	if (!phy)
		return;

	uphy = phy_get_drvdata(phy);
	port = usb3_phy_to_port(phy);
	if (port < 0) {
		dev_err(uphy->dev, "Invalid port number\n");
		return;
	}

	uphy_lane = uphy->usb3_ports[port].uphy_lane;

	if ((on && !uphy->usb3_ports[port].receiver_detector_disabled) ||
		(!on && uphy->usb3_ports[port].receiver_detector_disabled))
		return;

	uphy->usb3_ports[port].receiver_detector_disabled = !on;

	if (!on) {
		mask = TX_IDDQ | RX_IDDQ;
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_2);
		reg &= ~mask;
		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_2);

		mask = TX_IDDQ_OVRD | RX_IDDQ_OVRD;
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_2);
		reg |= mask;
		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_2);

		mask = (AUX_TX_RDET_CLK_EN | AUX_TX_RDET_BYP |
			AUX_TX_RDET_EN | AUX_TX_TERM_EN);
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_1);
		reg &= ~mask;
		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_1);

		mask = AUX_TX_MODE_OVRD;
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_1);
		reg |= mask;
		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_1);
	} else {
		mask = AUX_TX_MODE_OVRD;
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_1);
		reg &= ~mask;
		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_1);

		mask = TX_IDDQ_OVRD | RX_IDDQ_OVRD;
		reg = uphy_lane_readl(uphy, uphy_lane, UPHY_MISC_PAD_CTL_2);
		reg &= ~mask;
		uphy_lane_writel(uphy, uphy_lane, reg, UPHY_MISC_PAD_CTL_2);
	}

}
EXPORT_SYMBOL_GPL(t210_receiver_detector);

int tegra21x_phy_xusb_set_reverse_id(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_set_reverse_id);

int tegra21x_phy_xusb_clear_reverse_id(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_clear_reverse_id);

int tegra21x_phy_xusb_generate_srp(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_generate_srp);

int tegra21x_phy_xusb_enable_srp_detect(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_enable_srp_detect);

int tegra21x_phy_xusb_disable_srp_detect(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_disable_srp_detect);

bool tegra21x_phy_xusb_srp_detected(struct phy *phy)
{
	return false;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_srp_detected);

int tegra21x_phy_xusb_enable_otg_int(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_enable_otg_int);

int tegra21x_phy_xusb_disable_otg_int(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_disable_otg_int);

int tegra21x_phy_xusb_ack_otg_int(struct phy *phy)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_ack_otg_int);

int tegra21x_phy_xusb_get_otg_vbus_id(struct phy *phy,
				   struct tegra_xusb_otg_vbus_id *change)
{
	return 0;
}
EXPORT_SYMBOL_GPL(tegra21x_phy_xusb_get_otg_vbus_id);

MODULE_AUTHOR("BH Hsieh <bhsieh@nvidia.com>");
MODULE_DESCRIPTION("Tegra 21x XUSB PADCTL and UPHY PLL/Lane driver");
MODULE_LICENSE("GPL v2");
