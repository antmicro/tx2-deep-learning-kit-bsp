/*
 * NVIDIA Tegra xHCI host controller driver for T186/future chips
 *
 * Copyright (C) 2015-2017, NVIDIA Corporation.  All rights reserved.
 * Copyright (C) 2014 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/usb/of.h>
#include <linux/usb/otg.h>
#include <linux/kthread.h>
#include <linux/circ_buf.h>
#include <linux/extcon.h>
#include <linux/pm_qos.h>

#include <linux/tegra-powergate.h>
#include <soc/tegra/chip-id.h>
#include <linux/tegra_pm_domains.h>
#include <soc/tegra/xusb.h>

#include <linux/platform/tegra/emc_bwmgr.h>
#include <linux/platform/tegra/bwmgr_mc.h>

#include "xhci.h"

#ifdef DEBUG
#define reg_dump(_dev, _base, _reg) \
	dev_dbg(_dev, "%s @%x = 0x%x\n", #_reg, _reg, ioread32(_base + _reg))
#else
#define reg_dump(_dev, _base, _reg)					\
	do {								\
		if (0)							\
			dev_dbg(_dev, "%s @%x = 0x%x\n",		\
				#_reg, _reg, ioread32(_base + _reg));	\
	} while (0)
#endif

#define TEGRA_XHCI_SS_CLK_HIGH_SPEED 120000000
#define TEGRA_XHCI_SS_CLK_LOW_SPEED 12000000

/* FPCI CFG registers */
#define XUSB_CFG_1				0x004
#define  XUSB_IO_SPACE_EN			BIT(0)
#define  XUSB_MEM_SPACE_EN			BIT(1)
#define  XUSB_BUS_MASTER_EN			BIT(2)
#define XUSB_CFG_4				0x010
#define  XUSB_BASE_ADDR_SHIFT			15
#define  XUSB_BASE_ADDR_MASK			0x1ffff
#define XUSB_CFG_16				0x040
#define XUSB_CFG_24				0x060
#define XUSB_CFG_AXI_CFG			0x0f8
#define XUSB_CFG_ARU_C11_CSBRANGE		0x41c
#define XUSB_CFG_ARU_CONTEXT			0x43c
#define XUSB_CFG_ARU_CONTEXT_HS_PLS		0x478
#define XUSB_CFG_ARU_CONTEXT_FS_PLS		0x47c
#define XUSB_CFG_ARU_CONTEXT_HSFS_SPEED		0x480
#define XUSB_CFG_ARU_CONTEXT_HSFS_PP		0x484
#define XUSB_CFG_ARU_FW_SCRATCH			0x00000440
#define XUSB_CFG_CSB_BASE_ADDR			0x800

/* IPFS registers */
#define XUSB_HOST_MSI_BAR_SZ_0		0x0c0
#define XUSB_HOST_MSI_AXI_BAR_ST_0		0x0c4
#define XUSB_HOST_MSI_FPCI_BAR_ST_0	0x0c8
#define XUSB_HOST_MSI_VEC0_0		0x100
#define XUSB_HOST_MSI_EN_VEC0_0		0x140
#define XUSB_HOST_CONFIGURATION_0		0x180
#define  IPFS_EN_FPCI				BIT(0)
#define XUSB_HOST_FPCI_ERROR_MASKS_0	0x184
#define XUSB_HOST_INTR_MASK_0		0x188
#define  IPFS_IP_INT_MASK			BIT(16)
#define XUSB_HOST_IPFS_INTR_ENABLE_0	0x198
#define XUSB_HOST_UFPCI_CONFIG_0		0x19c
#define XUSB_HOST_CLKGATE_HYSTERESIS_0	0x1bc
#define XUSB_HOST_MCCIF_FIFOCTRL_0		0x1dc

#define ARU_CONTEXT_HS_PLS_SUSPEND	3
#define ARU_CONTEXT_HS_PLS_FS_MODE	6
#define FPCI_CTX_HS_PLS(hs_pls, pad)	((hs_pls >> (4 * pad)) & 0xf)

#define CSB_PAGE_SELECT_MASK			0x7fffff
#define CSB_PAGE_SELECT_SHIFT			9
#define CSB_PAGE_OFFSET_MASK			0x1ff
#define CSB_PAGE_SELECT(addr)	((addr) >> (CSB_PAGE_SELECT_SHIFT) &	\
				 CSB_PAGE_SELECT_MASK)
#define CSB_PAGE_OFFSET(addr)	((addr) & CSB_PAGE_OFFSET_MASK)

/* Falcon CSB registers */
#define XUSB_FALC_CPUCTL			0x100
#define  CPUCTL_STARTCPU			BIT(1)
#define  CPUCTL_STATE_HALTED			BIT(4)
#define  CPUCTL_STATE_STOPPED			BIT(5)
#define XUSB_FALC_BOOTVEC			0x104
#define XUSB_FALC_DMACTL			0x10c
#define XUSB_FALC_IMFILLRNG1			0x154
#define  IMFILLRNG1_TAG_MASK			0xffff
#define  IMFILLRNG1_TAG_LO_SHIFT		0
#define  IMFILLRNG1_TAG_HI_SHIFT		16
#define XUSB_FALC_IMFILLCTL			0x158

/* MP CSB registers */
#define XUSB_CSB_MP_ILOAD_ATTR			0x101a00
#define XUSB_CSB_MP_ILOAD_BASE_LO		0x101a04
#define XUSB_CSB_MP_ILOAD_BASE_HI		0x101a08
#define XUSB_CSB_MP_L2IMEMOP_SIZE		0x101a10
#define  L2IMEMOP_SIZE_SRC_OFFSET_SHIFT		8
#define  L2IMEMOP_SIZE_SRC_OFFSET_MASK		0x3ff
#define  L2IMEMOP_SIZE_SRC_COUNT_SHIFT		24
#define  L2IMEMOP_SIZE_SRC_COUNT_MASK		0xff
#define XUSB_CSB_MP_L2IMEMOP_TRIG		0x101a14
#define  L2IMEMOP_ACTION_SHIFT			24
#define  L2IMEMOP_INVALIDATE_ALL		(0x40 << L2IMEMOP_ACTION_SHIFT)
#define  L2IMEMOP_LOAD_LOCKED_RESULT		(0x11 << L2IMEMOP_ACTION_SHIFT)
#define XUSB_CSB_MEMPOOL_L2IMEMOP_RESULT	0x00101A18
#define  L2IMEMOP_RESULT_VLD			(1 << 31)
#define XUSB_CSB_MP_APMAP			0x10181c
#define  APMAP_BOOTPATH				BIT(31)

#define IMEM_BLOCK_SIZE				256

#define FW_IOCTL_LOG_DEQUEUE_LOW	(4)
#define FW_IOCTL_LOG_DEQUEUE_HIGH	(5)
#define FW_IOCTL_DATA_SHIFT		(0)
#define FW_IOCTL_DATA_MASK		(0x00ffffff)
#define FW_IOCTL_TYPE_SHIFT		(24)
#define FW_IOCTL_TYPE_MASK		(0xff000000)
#define FW_LOG_SIZE			((int) sizeof(struct log_entry))
#define FW_LOG_COUNT			(4096)
#define FW_LOG_RING_SIZE		(FW_LOG_SIZE * FW_LOG_COUNT)
#define FW_LOG_PAYLOAD_SIZE		(27)
#define DRIVER				(0x01)
#define CIRC_BUF_SIZE			(4 * (1 << 20))	/* 4MB */
#define FW_LOG_THREAD_RELAX		(msecs_to_jiffies(500))

/* tegra_xhci_firmware_log.flags bits */
#define FW_LOG_CONTEXT_VALID		(0)
#define FW_LOG_FILE_OPENED		(1)

#define FW_MAJOR_VERSION(x)		(((x) >> 24) & 0xff)
#define FW_MINOR_VERSION(x)		(((x) >> 16) & 0xff)

/* firmware retry limit */
#define FW_RETRY_COUNT			(5)

/* device quirks */
#define QUIRK_FOR_SS_DEVICE				BIT(0)
#define QUIRK_FOR_HS_DEVICE				BIT(1)
#define QUIRK_FOR_FS_DEVICE				BIT(2)
#define QUIRK_FOR_LS_DEVICE				BIT(3)
#define QUIRK_FOR_USB2_DEVICE \
	(QUIRK_FOR_HS_DEVICE | QUIRK_FOR_FS_DEVICE | QUIRK_FOR_LS_DEVICE)

#define USB_DEVICE_USB3(vid, pid) \
	USB_DEVICE(vid, pid), \
	.driver_info = (QUIRK_FOR_USB2_DEVICE | QUIRK_FOR_SS_DEVICE),

#define USB_DEVICE_USB2(vid, pid) \
	USB_DEVICE(vid, pid), \
	.driver_info = QUIRK_FOR_USB2_DEVICE,

#define USB_DEVICE_SS(vid, pid) \
	USB_DEVICE(vid, pid), \
	.driver_info = QUIRK_FOR_SS_DEVICE,

#define USB_DEVICE_HS(vid, pid) \
	USB_DEVICE(vid, pid), \
	.driver_info = QUIRK_FOR_HS_DEVICE,

#define USB_DEVICE_FS(vid, pid) \
	USB_DEVICE(vid, pid), \
	.driver_info = QUIRK_FOR_FS_DEVICE,

#define USB_DEVICE_LS(vid, pid) \
	USB_DEVICE(vid, pid), \
	.driver_info = QUIRK_FOR_LS_DEVICE,

/* Device ID */
#define XHCI_DEVICE_ID_T210     0x0fad
#define XHCI_DEVICE_ID_T186     0x10e2

#define XHCI_IS_T210(t) (t->soc_config ? \
	(t->soc_config->device_id == XHCI_DEVICE_ID_T210) : false)
#define XHCI_IS_T186(t) (t->soc_config ? \
	(t->soc_config->device_id == XHCI_DEVICE_ID_T186) : false)

/* Number of PMQOS clusters */
#define PM_QOS_CLUSTERS		2

/* default parameters for boosting CPU freq */
#define XHCI_BOOST_TIMEOUT		2000 /* 2 seconds */
#define XHCI_BOOST_TRIGGER_SIZE		16384 /* 16KB */

static const struct usb_device_id disable_usb_persist_quirk_list[] = {
	/* Sandisk Extreme USB 3.0 pen drive, SuperSpeed */
	{ USB_DEVICE_SS(0x0781, 0x5580) },
	{ }  /* terminating entry must be last */
};

static int usb_match_speed(struct usb_device *udev,
			    const struct usb_device_id *id)
{
	if (!id)
		return 0;

	if ((id->driver_info & QUIRK_FOR_SS_DEVICE) &&
					udev->speed == USB_SPEED_SUPER)
		return 1;

	if ((id->driver_info & QUIRK_FOR_HS_DEVICE) &&
					udev->speed == USB_SPEED_HIGH)
		return 1;

	if ((id->driver_info & QUIRK_FOR_FS_DEVICE) &&
					udev->speed == USB_SPEED_FULL)
		return 1;

	if ((id->driver_info & QUIRK_FOR_LS_DEVICE) &&
					udev->speed == USB_SPEED_LOW)
		return 1;

	return 0;
}

#if IS_ENABLED(CONFIG_PM_GENERIC_DOMAINS)
static struct of_device_id tegra_xusba_pd[] = {
	{ .compatible = "nvidia,tegra186-xusba-pd", },
	{ .compatible = "nvidia,tegra210-xusba-pd", },
	{},
};

static struct of_device_id tegra_xusbc_pd[] = {
	{ .compatible = "nvidia,tegra186-xusbc-pd", },
	{ .compatible = "nvidia,tegra210-xusbc-pd", },
	{},
};
#endif

enum build_info_log {
	LOG_NONE = 0,
	LOG_MEMORY
};
struct tegra_xhci_fw_cfgtbl {
	u32 boot_loadaddr_in_imem;
	u32 boot_codedfi_offset;
	u32 boot_codetag;
	u32 boot_codesize;
	u32 phys_memaddr;
	u16 reqphys_memsize;
	u16 alloc_phys_memsize;
	u32 rodata_img_offset;
	u32 rodata_section_start;
	u32 rodata_section_end;
	u32 main_fnaddr;
	u32 fwimg_cksum;
	u32 fwimg_created_time;
	u32 imem_resident_start;
	u32 imem_resident_end;
	u32 idirect_start;
	u32 idirect_end;
	u32 l2_imem_start;
	u32 l2_imem_end;
	u32 version_id;
	u8 init_ddirect;
	u8 reserved[3];
	u32 phys_addr_log_buffer;
	u32 total_log_entries;
	u32 dequeue_ptr;
	u32 dummy_var[2];
	u32 fwimg_len;
	u8 magic[8];
	u32 ss_low_power_entry_timeout;
	u8 num_hsic_port;
	u8 ss_portmap;
	u8 build_log:4;
	u8 build_type:4;
	u8 padding[137]; /* Padding to make 256-bytes cfgtbl */
};

struct tegra_xhci_ipfs_context {
	u32 msi_bar_sz;
	u32 msi_axi_barst;
	u32 msi_fpci_barst;
	u32 msi_vec0;
	u32 msi_en_vec0;
	u32 fpci_error_masks;
	u32 intr_mask;
	u32 ipfs_intr_enable;
	u32 ufpci_config;
	u32 clkgate_hysteresis;
	u32 xusb_host_mccif_fifo_cntrl;
};

struct tegra_xhci_fpci_context {
	u32 hs_pls;
	u32 fs_pls;
	u32 hsfs_speed;
	u32 hsfs_pp;
	u32 cfg_aru;
	u32 cfg_order;
	u32 cfg_fladj;
	u32 cfg_sid;
};

enum tegra_xhci_phy_type {
	USB3_PHY,
	UTMI_PHY,
	HSIC_PHY,
	MAX_PHY_TYPES,
};

static const char * const tegra_phy_names[] = {
	[USB3_PHY] = "usb3",
	[UTMI_PHY] = "utmi",
	[HSIC_PHY] = "hsic",
};

struct tegra_xhci_soc_config {
	u16 device_id;
	const char *firmware_file;
	bool lpm_support;
	unsigned int num_phys[MAX_PHY_TYPES];
	unsigned int utmi_port_offset;
	unsigned int hsic_port_offset;
	void (*utmi_pad_power_on)(struct phy *utmi_phy);
	void (*utmi_pad_power_off)(struct phy *utmi_phy);
	void (*set_protection_level)(struct phy *utmi_phy,
		int level, enum tegra_vbus_dir dir);
	int (*utmi_vbus_power_on)(struct phy *phy);
	int (*utmi_vbus_enable)(struct phy *phy);
	int (*utmi_vbus_power_off)(struct phy *phy);
	int (*overcurrent_detected)(struct phy *phy);
	void (*handle_overcurrent)(struct phy *phy);
	int (*set_id_override)(struct phy *phy);
	int (*clear_id_override)(struct phy *phy);
	bool (*has_otg_cap)(struct phy *phy);
	int (*enable_sleepwalk)(struct phy *phy, enum usb_device_speed speed);
	int (*disable_sleepwalk)(struct phy *phy);
	int (*enable_wake)(struct phy *phy);
	int (*disable_wake)(struct phy *phy);
	int (*pretend_connected)(struct phy *phy);
	int (*remote_wake_detected)(struct phy *phy);

	void (*receiver_detector)(struct phy *phy, bool on);
	void (*clamp_en_early)(struct phy *phy, bool on);
	bool disable_u0_ts1_detect;

	int num_supplies;
	const char *supply_names[];
};

static void __iomem *car_base;
static void __iomem *padctl_base;
#define CLK_RST_CONTROLLER_RST_DEV_XUSB_0	(0x470000)
#define   SWR_XUSB_HOST_RST			(1 << 0)
#define   SWR_XUSB_DEV_RST			(1 << 1)
#define   SWR_XUSB_PADCTL_RST			(1 << 2)
#define   SWR_XUSB_SS_RST			(1 << 3)

struct log_entry {
	u32 sequence_no;
	u8 data[FW_LOG_PAYLOAD_SIZE];
	u8 owner;
};

struct tegra_xhci_firmware_log {
	dma_addr_t phys_addr;		/* dma-able address */
	void *virt_addr;		/* kernel va of the shared log buffer */
	struct log_entry *dequeue;	/* current dequeue pointer (va) */
	struct circ_buf circ;		/* big circular buffer */
	u32 seq;			/* log sequence number */

	struct task_struct *thread;	/* a thread to consume log */
	struct mutex mutex;
	wait_queue_head_t read_wait;
	wait_queue_head_t write_wait;
	wait_queue_head_t intr_wait;
	struct dentry *log_file;
	unsigned long flags;
};

struct tegra_xhci_hcd {
	struct device *dev;
	struct usb_hcd *hcd;

	struct mutex lock;

	int irq;
	int padctl_irq;

	void __iomem *fpci_base;

	const struct tegra_xhci_soc_config *soc_config;

	int num_supplies;
	struct regulator_bulk_data *supplies;

	void __iomem *ipfs_base;
	struct tegra_xhci_ipfs_context ipfs_ctx;
	struct clk *host_clk;
	struct clk *falc_clk;
	struct clk *ss_clk;
	struct clk *ss_src_clk;
	struct clk *hs_src_clk;
	struct clk *fs_src_clk;
	struct clk *pll_u_480m;
	struct clk *clk_m;

	int pgid_ss;
	int pgid_host;

	/* bandwidth manager handle */
	struct tegra_bwmgr_client *bwmgr_handle;

	int num_phys;
	struct phy **phys[MAX_PHY_TYPES];
	struct phy **cdp_phys;
	unsigned enabled_ss_ports; /* bitmap */
	unsigned num_hsic_port; /* count */

	struct work_struct mbox_req_work;
	struct tegra_xusb_mbox_msg mbox_req;
	struct mbox_client mbox_client;
	struct mbox_chan *mbox_chan;

	struct tegra_xhci_fpci_context fpci_ctx;

	bool suspended;
	bool lp0_exit;

	/* Firmware loading related */
	void *fw_data;
	size_t fw_size;
	dma_addr_t fw_dma_addr;
	struct delayed_work firmware_retry_work;
	int fw_retry_count;
	bool fw_loaded;

	struct dentry *debugfs_dir;
	struct dentry *dump_ring_file;
	struct tegra_xhci_firmware_log log;

	int utmi_otg_port_base_1; /* one based utmi port number */
	int usb3_otg_port_base_1; /* one based usb3 port number */
	struct extcon_dev *id_extcon;
	struct notifier_block id_extcon_nb;
	struct work_struct id_extcon_work;

	bool host_mode;
	bool otg_role_initialized;
	struct delayed_work otg_retry_work;

	bool pmc_usb_wakes_disabled;

	u8 *connected_utmi_ports; /* keep track of connected UTMI ports */
	bool cdp_enabled;

	struct mutex mbox_lock_ack;
	u32 fw_ack; /* storing the mbox cmd_type from FW */
	wait_queue_head_t fw_ack_wq; /* sleep support for FW to SW mbox ack */

	struct work_struct oc_work;

	struct mutex boost_cpufreq_lock;
	struct pm_qos_request core_req;
	struct pm_qos_request boost_cpufreq_req[PM_QOS_CLUSTERS];
	struct work_struct boost_cpufreq_work;
	unsigned int boost_cpu_freq;
	unsigned int boost_cpu_trigger;
	unsigned long cpufreq_last_boosted;
	bool cpu_boost_enabled;
};

static struct hc_driver __read_mostly tegra_xhci_hc_driver;

static void tegra_xusb_boost_cpu_freq_fn(struct work_struct *work)
{
	struct tegra_xhci_hcd *tegra = container_of(work, struct tegra_xhci_hcd,
							boost_cpufreq_work);
	unsigned long delay = XHCI_BOOST_TIMEOUT;
	s32 cpufreq = tegra->boost_cpu_freq * 1000;

	mutex_lock(&tegra->boost_cpufreq_lock);

	dev_dbg(tegra->dev, "boost cpu freq %d kHz, with timeout %lu ms\n",
							cpufreq, delay);

	if (XHCI_IS_T210(tegra)) {
		pm_qos_update_request_timeout(&tegra->core_req,
			PM_QOS_MAX_ONLINE_CPUS_DEFAULT_VALUE, delay * 1000);
		pm_qos_update_request_timeout(&tegra->boost_cpufreq_req[0],
						cpufreq, delay * 1000);
	} else {
		pm_qos_update_request_timeout(&tegra->boost_cpufreq_req[0],
						cpufreq, delay * 1000);
		pm_qos_update_request_timeout(&tegra->boost_cpufreq_req[1],
						cpufreq, delay * 1000);
	}

	tegra->cpufreq_last_boosted = jiffies;
	mutex_unlock(&tegra->boost_cpufreq_lock);
}

static void tegra_xusb_boost_cpu_init(struct tegra_xhci_hcd *tegra)
{
	INIT_WORK(&tegra->boost_cpufreq_work, tegra_xusb_boost_cpu_freq_fn);

	if (XHCI_IS_T210(tegra)) {
		pm_qos_add_request(&tegra->core_req, PM_QOS_MIN_ONLINE_CPUS,
							PM_QOS_DEFAULT_VALUE);
		pm_qos_add_request(&tegra->boost_cpufreq_req[0],
				PM_QOS_CPU_FREQ_MIN, PM_QOS_DEFAULT_VALUE);
	} else {
		pm_qos_add_request(&tegra->boost_cpufreq_req[0],
				PM_QOS_CLUSTER0_FREQ_MIN, PM_QOS_DEFAULT_VALUE);
		pm_qos_add_request(&tegra->boost_cpufreq_req[1],
				PM_QOS_CLUSTER1_FREQ_MIN, PM_QOS_DEFAULT_VALUE);
	}

	mutex_init(&tegra->boost_cpufreq_lock);
}

static void tegra_xusb_boost_cpu_deinit(struct tegra_xhci_hcd *tegra)
{
	cancel_work_sync(&tegra->boost_cpufreq_work);

	if (XHCI_IS_T210(tegra)) {
		pm_qos_remove_request(&tegra->core_req);
		pm_qos_remove_request(&tegra->boost_cpufreq_req[0]);
	} else {
		pm_qos_remove_request(&tegra->boost_cpufreq_req[0]);
		pm_qos_remove_request(&tegra->boost_cpufreq_req[1]);
	}

	mutex_destroy(&tegra->boost_cpufreq_lock);
}

static void tegra_xhci_enable_receiver_detector(struct tegra_xhci_hcd *tegra,
						struct phy *phy)
{
	if (tegra->soc_config->receiver_detector)
		tegra->soc_config->receiver_detector(phy, true);
}

static void tegra_xhci_disable_receiver_detector(struct tegra_xhci_hcd *tegra,
						struct phy *phy)
{
	if (tegra->soc_config->receiver_detector)
		tegra->soc_config->receiver_detector(phy, false);
}

static void tegra_xhci_enable_clamp_en_early(struct tegra_xhci_hcd *tegra,
						struct phy *phy)
{
	if (tegra->soc_config->clamp_en_early)
		tegra->soc_config->clamp_en_early(phy, true);
}

static void tegra_xhci_disable_clamp_en_early(struct tegra_xhci_hcd *tegra,
						struct phy *phy)
{
	if (tegra->soc_config->clamp_en_early)
		tegra->soc_config->clamp_en_early(phy, false);
}

static inline struct tegra_xhci_hcd *hcd_to_tegra_xhci(struct usb_hcd *hcd)
{
	return (struct tegra_xhci_hcd *) dev_get_drvdata(hcd->self.controller);
}

static inline struct tegra_xhci_hcd *
mbox_work_to_tegra(struct work_struct *work)
{
	return container_of(work, struct tegra_xhci_hcd, mbox_req_work);
}

static inline u32 fpci_readl(struct tegra_xhci_hcd *tegra, u32 addr)
{
	u32 val = readl(tegra->fpci_base + addr);

	dev_dbg(tegra->dev, "%s addr 0x%x val 0x%x\n", __func__, addr, val);
	return val;
}

static inline void fpci_writel(struct tegra_xhci_hcd *tegra, u32 val, u32 addr)
{
	dev_dbg(tegra->dev, "%s addr 0x%x val 0x%x\n", __func__, addr, val);
	writel(val, tegra->fpci_base + addr);
}

static inline u32 ipfs_readl(struct tegra_xhci_hcd *tegra, u32 addr)
{
	u32 val = readl(tegra->ipfs_base + addr);

	dev_dbg(tegra->dev, "%s addr 0x%x val 0x%x\n", __func__, addr, val);
	return val;
}

static inline void ipfs_writel(struct tegra_xhci_hcd *tegra, u32 val, u32 addr)
{
	dev_dbg(tegra->dev, "%s addr 0x%x val 0x%x\n", __func__, addr, val);
	writel(val, tegra->ipfs_base + addr);
}

static u32 csb_readl(struct tegra_xhci_hcd *tegra, u32 addr)
{
	u32 page, offset;
	u32 val;

	page = CSB_PAGE_SELECT(addr);
	offset = CSB_PAGE_OFFSET(addr);
	fpci_writel(tegra, page, XUSB_CFG_ARU_C11_CSBRANGE);
	val = fpci_readl(tegra, XUSB_CFG_CSB_BASE_ADDR + offset);
	dev_dbg(tegra->dev, "%s addr 0x%x val 0x%x\n", __func__, addr, val);
	return val;
}

static void csb_writel(struct tegra_xhci_hcd *tegra, u32 val, u32 addr)
{
	u32 page, offset;

	dev_dbg(tegra->dev, "%s addr 0x%x val 0x%x\n", __func__, addr, val);
	page = CSB_PAGE_SELECT(addr);
	offset = CSB_PAGE_OFFSET(addr);
	fpci_writel(tegra, page, XUSB_CFG_ARU_C11_CSBRANGE);
	fpci_writel(tegra, val, XUSB_CFG_CSB_BASE_ADDR + offset);
}

/**
 * fw_log_next - find next log entry in a tegra_xhci_firmware_log context.
 *	This function takes care of wrapping. That means when current log entry
 *	is the last one, it returns with the first one.
 *
 * @param log	The tegra_xhci_firmware_log context.
 * @param this	The current log entry.
 * @return	The log entry which is next to the current one.
 */
static inline struct log_entry *fw_log_next(
		struct tegra_xhci_firmware_log *log, struct log_entry *this)
{
	struct log_entry *first = (struct log_entry *) log->virt_addr;
	struct log_entry *last = first + FW_LOG_COUNT - 1;

	WARN((this < first) || (this > last), "%s: invalid input\n", __func__);

	return (this == last) ? first : (this + 1);
}

/**
 * fw_log_update_dequeue_pointer - update dequeue pointer to both firmware and
 *	tegra_xhci_firmware_log.dequeue.
 *
 * @param log	The tegra_xhci_firmware_log context.
 * @param n	Counts of log entries to fast-forward.
 */
static inline void fw_log_update_deq_pointer(
		struct tegra_xhci_firmware_log *log, int n)
{
	struct tegra_xhci_hcd *tegra =
			container_of(log, struct tegra_xhci_hcd, log);
	struct device *dev = tegra->dev;
	struct log_entry *deq = tegra->log.dequeue;
	dma_addr_t physical_addr;
	u32 reg;

	dev_vdbg(dev, "curr 0x%p fast-forward %d entries\n", deq, n);
	while (n-- > 0)
		deq = fw_log_next(log, deq);

	tegra->log.dequeue = deq;
	physical_addr = tegra->log.phys_addr +
			((u8 *)deq - (u8 *)tegra->log.virt_addr);

	/* update dequeue pointer to firmware */
	reg = (FW_IOCTL_LOG_DEQUEUE_LOW << FW_IOCTL_TYPE_SHIFT);
	reg |= (physical_addr & 0xffff); /* lower 16-bits */
	iowrite32(reg, tegra->fpci_base + XUSB_CFG_ARU_FW_SCRATCH);

	reg = (FW_IOCTL_LOG_DEQUEUE_HIGH << FW_IOCTL_TYPE_SHIFT);
	reg |= ((physical_addr >> 16) & 0xffff); /* higher 16-bits */
	iowrite32(reg, tegra->fpci_base + XUSB_CFG_ARU_FW_SCRATCH);

	dev_vdbg(dev, "new 0x%p physical addr 0x%x\n", deq, (u32)physical_addr);
}

static inline bool circ_buffer_full(struct circ_buf *circ)
{
	int space = CIRC_SPACE(circ->head, circ->tail, CIRC_BUF_SIZE);

	return (space <= FW_LOG_SIZE);
}

static inline bool fw_log_available(struct tegra_xhci_hcd *tegra)
{
	return (tegra->log.dequeue->owner == DRIVER);
}

/**
 * fw_log_wait_empty_timeout - wait firmware log thread to clean up shared
 *	log buffer.
 * @param tegra:	tegra_xhci_hcd context
 * @param msec:		timeout value in millisecond
 * @return true:	shared log buffer is empty,
 *	   false:	shared log buffer isn't empty.
 */
static inline bool fw_log_wait_empty_timeout(struct tegra_xhci_hcd *tegra,
		unsigned timeout)
{
	unsigned long target = jiffies + msecs_to_jiffies(timeout);
	struct circ_buf *circ = &tegra->log.circ;
	bool ret;

	mutex_lock(&tegra->log.mutex);

	while (fw_log_available(tegra) && time_is_after_jiffies(target)) {
		if (circ_buffer_full(circ) &&
			!test_bit(FW_LOG_FILE_OPENED, &tegra->log.flags))
			break; /* buffer is full but nobody is reading log */

		mutex_unlock(&tegra->log.mutex);
		usleep_range(1000, 2000);
		mutex_lock(&tegra->log.mutex);
	}

	ret = fw_log_available(tegra);
	mutex_unlock(&tegra->log.mutex);

	return ret;
}

/**
 * fw_log_copy - copy firmware log from device's buffer to driver's circular
 *	buffer.
 * @param tegra	tegra_xhci_hcd context
 * @return true,	We still have firmware log in device's buffer to copy.
 *			This function returned due the driver's circular buffer
 *			is full. Caller should invoke this function again as
 *			soon as there is space in driver's circular buffer.
 *	   false,	Device's buffer is empty.
 */
static inline bool fw_log_copy(struct tegra_xhci_hcd *tegra)
{
	struct device *dev = tegra->dev;
	struct circ_buf *circ = &tegra->log.circ;
	int head, tail;
	int buffer_len, copy_len;
	struct log_entry *entry;
	struct log_entry *first = tegra->log.virt_addr;

	while (fw_log_available(tegra)) {

		/* calculate maximum contiguous driver buffer length */
		head = circ->head;
		tail = ACCESS_ONCE(circ->tail);
		buffer_len = CIRC_SPACE_TO_END(head, tail, CIRC_BUF_SIZE);
		/* round down to FW_LOG_SIZE */
		buffer_len -= (buffer_len % FW_LOG_SIZE);
		if (!buffer_len)
			return true; /* log available but no space left */

		/* calculate maximum contiguous log copy length */
		entry = tegra->log.dequeue;
		copy_len = 0;
		do {
			if (tegra->log.seq != entry->sequence_no) {
				dev_warn(dev,
				"%s: discontinuous seq no, expect %u get %u\n",
				__func__, tegra->log.seq, entry->sequence_no);
			}
			tegra->log.seq = entry->sequence_no + 1;

			copy_len += FW_LOG_SIZE;
			buffer_len -= FW_LOG_SIZE;
			if (!buffer_len)
				break; /* no space left */
			entry = fw_log_next(&tegra->log, entry);
		} while ((entry->owner == DRIVER) && (entry != first));

		memcpy(&circ->buf[head], tegra->log.dequeue, copy_len);
		memset(tegra->log.dequeue, 0, copy_len);
		circ->head = (circ->head + copy_len) & (CIRC_BUF_SIZE - 1);

		mb(); /* make sure controller sees it */

		fw_log_update_deq_pointer(&tegra->log, copy_len/FW_LOG_SIZE);

		dev_vdbg(dev, "copied %d entries, new dequeue 0x%p\n",
				copy_len/FW_LOG_SIZE, tegra->log.dequeue);
		wake_up_interruptible(&tegra->log.read_wait);
	}

	return false;
}

static int fw_log_thread(void *data)
{
	struct tegra_xhci_hcd *tegra = data;
	struct device *dev = tegra->dev;
	struct circ_buf *circ = &tegra->log.circ;
	bool logs_left;

	dev_dbg(dev, "start firmware log thread\n");

	do {
		mutex_lock(&tegra->log.mutex);
		if (circ_buffer_full(circ)) {
			mutex_unlock(&tegra->log.mutex);
			dev_info(dev, "%s: circ buffer full\n", __func__);
			wait_event_interruptible(tegra->log.write_wait,
			    kthread_should_stop() || !circ_buffer_full(circ));
			mutex_lock(&tegra->log.mutex);
		}

		logs_left = fw_log_copy(tegra);
		mutex_unlock(&tegra->log.mutex);

		/* relax if no logs left  */
		if (!logs_left)
			wait_event_interruptible_timeout(tegra->log.intr_wait,
				fw_log_available(tegra), FW_LOG_THREAD_RELAX);
	} while (!kthread_should_stop());

	dev_dbg(dev, "stop firmware log thread\n");
	return 0;
}

static inline bool circ_buffer_empty(struct circ_buf *circ)
{
	return (CIRC_CNT(circ->head, circ->tail, CIRC_BUF_SIZE) == 0);
}

static ssize_t fw_log_file_read(struct file *file, char __user *buf,
		size_t count, loff_t *offp)
{
	struct tegra_xhci_hcd *tegra = file->private_data;
	struct device *dev = tegra->dev;
	struct circ_buf *circ = &tegra->log.circ;
	int head, tail;
	size_t n = 0;
	int s;

	mutex_lock(&tegra->log.mutex);

	while (circ_buffer_empty(circ)) {
		mutex_unlock(&tegra->log.mutex);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN; /* non-blocking read */

		dev_dbg(dev, "%s: nothing to read\n", __func__);

		if (wait_event_interruptible(tegra->log.read_wait,
				!circ_buffer_empty(circ)))
			return -ERESTARTSYS;

		if (mutex_lock_interruptible(&tegra->log.mutex))
			return -ERESTARTSYS;
	}

	while (count > 0) {
		head = ACCESS_ONCE(circ->head);
		tail = circ->tail;
		s = min_t(int, count,
				CIRC_CNT_TO_END(head, tail, CIRC_BUF_SIZE));

		if (s > 0) {
			if (copy_to_user(&buf[n], &circ->buf[tail], s)) {
				dev_warn(dev, "copy_to_user failed\n");
				mutex_unlock(&tegra->log.mutex);
				return -EFAULT;
			}
			circ->tail = (circ->tail + s) & (CIRC_BUF_SIZE - 1);

			count -= s;
			n += s;
		} else
			break;
	}

	mutex_unlock(&tegra->log.mutex);

	wake_up_interruptible(&tegra->log.write_wait);

	dev_dbg(dev, "%s: %zu bytes\n", __func__, n);

	return n;
}

static int fw_log_file_open(struct inode *inode, struct file *file)
{
	struct tegra_xhci_hcd *tegra;

	file->private_data = inode->i_private;
	tegra = file->private_data;

	if (test_and_set_bit(FW_LOG_FILE_OPENED, &tegra->log.flags)) {
		dev_info(tegra->dev, "%s: already opened\n", __func__);
		return -EBUSY;
	}

	return 0;
}

static int fw_log_file_close(struct inode *inode, struct file *file)
{
	struct tegra_xhci_hcd *tegra = file->private_data;

	clear_bit(FW_LOG_FILE_OPENED, &tegra->log.flags);

	return 0;
}

static const struct file_operations firmware_log_fops = {
		.open		= fw_log_file_open,
		.release	= fw_log_file_close,
		.read		= fw_log_file_read,
		.owner		= THIS_MODULE,
};

#ifdef CONFIG_USB_XHCI_HCD_DEBUGGING
static ssize_t dump_ring_file_write(struct file *file, const char __user *buf,
	size_t count, loff_t *offp)
{
	struct tegra_xhci_hcd *tegra = file->private_data;
	struct xhci_hcd *xhci = hcd_to_xhci(tegra->hcd);

	/* trigger xhci_event_ring_work() for debug */
	del_timer_sync(&xhci->event_ring_timer);
	xhci->event_ring_timer.expires = jiffies;
	add_timer(&xhci->event_ring_timer);

	return count;
}

static const struct file_operations dump_ring_fops = {
		.write		= dump_ring_file_write,
		.owner		= THIS_MODULE,
};
#endif

static int fw_log_init(struct tegra_xhci_hcd *tegra)
{
	struct device *dev = tegra->dev;
	int rc = 0;

	if (!tegra->debugfs_dir)
		return -ENODEV; /* no debugfs support */

	if (test_bit(FW_LOG_CONTEXT_VALID, &tegra->log.flags))
		return 0; /* already done */

	/* allocate buffer to be shared between driver and firmware */
	tegra->log.virt_addr = dma_alloc_writecombine(dev,
			FW_LOG_RING_SIZE, &tegra->log.phys_addr, GFP_KERNEL);

	if (!tegra->log.virt_addr) {
		dev_err(dev, "dma_alloc_writecombine() size %d failed\n",
				FW_LOG_RING_SIZE);
		return -ENOMEM;
	}

	dev_info(dev, "%d bytes log buffer physical 0x%u virtual 0x%p\n",
		FW_LOG_RING_SIZE, (u32)tegra->log.phys_addr,
		tegra->log.virt_addr);

	memset(tegra->log.virt_addr, 0, FW_LOG_RING_SIZE);
	tegra->log.dequeue = tegra->log.virt_addr;

	tegra->log.circ.buf = vmalloc(CIRC_BUF_SIZE);
	if (!tegra->log.circ.buf) {
		rc = -ENOMEM;
		goto error_free_dma;
	}

	tegra->log.circ.head = 0;
	tegra->log.circ.tail = 0;

	init_waitqueue_head(&tegra->log.read_wait);
	init_waitqueue_head(&tegra->log.write_wait);
	init_waitqueue_head(&tegra->log.intr_wait);

	mutex_init(&tegra->log.mutex);

	tegra->log.log_file = debugfs_create_file("firmware_log", S_IRUGO,
			tegra->debugfs_dir, tegra, &firmware_log_fops);
	if ((!tegra->log.log_file) ||
			(tegra->log.log_file == ERR_PTR(-ENODEV))) {
		dev_warn(dev, "debugfs_create_file() failed\n");
		rc = -ENOMEM;
		goto error_free_mem;
	}

	tegra->log.thread = kthread_run(fw_log_thread, tegra, "xusb-fw-log");
	if (IS_ERR(tegra->log.thread)) {
		dev_warn(dev, "kthread_run() failed\n");
		rc = -ENOMEM;
		goto error_remove_debugfs_file;
	}

	set_bit(FW_LOG_CONTEXT_VALID, &tegra->log.flags);
	return rc;

error_remove_debugfs_file:
	debugfs_remove(tegra->log.log_file);
error_free_mem:
	vfree(tegra->log.circ.buf);
error_free_dma:
	dma_free_writecombine(dev, FW_LOG_RING_SIZE,
			tegra->log.virt_addr, tegra->log.phys_addr);
	memset(&tegra->log, 0, sizeof(tegra->log));
	return rc;
}

static void fw_log_deinit(struct tegra_xhci_hcd *tegra)
{
	struct device *dev = tegra->dev;

	if (test_and_clear_bit(FW_LOG_CONTEXT_VALID, &tegra->log.flags)) {

		debugfs_remove(tegra->log.log_file);

		wake_up_interruptible(&tegra->log.read_wait);
		wake_up_interruptible(&tegra->log.write_wait);
		kthread_stop(tegra->log.thread);

		mutex_lock(&tegra->log.mutex);
		dma_free_writecombine(dev, FW_LOG_RING_SIZE,
			tegra->log.virt_addr, tegra->log.phys_addr);
		vfree(tegra->log.circ.buf);
		tegra->log.circ.head = tegra->log.circ.tail = 0;
		mutex_unlock(&tegra->log.mutex);

		mutex_destroy(&tegra->log.mutex);
	}
}

static void tegra_xhci_cfg(struct tegra_xhci_hcd *tegra)
{
	u32 reg;

	if (XHCI_IS_T210(tegra)) {
		reg = ipfs_readl(tegra, XUSB_HOST_CONFIGURATION_0);
		reg |= IPFS_EN_FPCI;
		ipfs_writel(tegra, reg, XUSB_HOST_CONFIGURATION_0);

		udelay(10);
	}

	reg_dump(tegra->dev, tegra->fpci_base, XUSB_CFG_4);
	/* Program Bar0 Space */
	reg = fpci_readl(tegra, XUSB_CFG_4);
	reg &= ~(XUSB_BASE_ADDR_MASK << XUSB_BASE_ADDR_SHIFT);
	reg |= tegra->hcd->rsrc_start & (XUSB_BASE_ADDR_MASK <<
					 XUSB_BASE_ADDR_SHIFT);
	fpci_writel(tegra, reg, XUSB_CFG_4);
	usleep_range(100, 200);
	reg_dump(tegra->dev, tegra->fpci_base, XUSB_CFG_4);

	reg_dump(tegra->dev, tegra->fpci_base, XUSB_CFG_1);
	/* Enable Bus Master */
	reg = fpci_readl(tegra, XUSB_CFG_1);
	reg |= XUSB_MEM_SPACE_EN | XUSB_BUS_MASTER_EN;
	fpci_writel(tegra, reg, XUSB_CFG_1);
	reg_dump(tegra->dev, tegra->fpci_base, XUSB_CFG_1);

	if (XHCI_IS_T210(tegra)) {
		reg = ipfs_readl(tegra, XUSB_HOST_INTR_MASK_0);
		reg |= IPFS_IP_INT_MASK;
		ipfs_writel(tegra, reg, XUSB_HOST_INTR_MASK_0);

		/* Set hysteresis */
		ipfs_writel(tegra, 0x80, XUSB_HOST_CLKGATE_HYSTERESIS_0);
	}
}

static int tegra_xhci_load_firmware(struct tegra_xhci_hcd *tegra)
{
	struct device *dev = tegra->dev;
	struct xhci_cap_regs __iomem *cap_regs;
	struct xhci_op_regs __iomem *op_regs;
	struct tegra_xhci_fw_cfgtbl *cfg_tbl;
	struct tm fw_tm;
	u32 val, code_tag_blocks, code_size_blocks;
	u64 fw_base;
	time_t fw_time;
	unsigned long timeout;

	if (csb_readl(tegra, XUSB_CSB_MP_ILOAD_BASE_LO) != 0) {
		dev_info(dev, "Firmware already loaded, Falcon state 0x%x\n",
			 csb_readl(tegra, XUSB_FALC_CPUCTL));
		return 0;
	}

	cfg_tbl = (struct tegra_xhci_fw_cfgtbl *)tegra->fw_data;

	cfg_tbl->ss_portmap = tegra->enabled_ss_ports;
	dev_dbg(dev, "%s ss_portmap 0x%x\n", __func__, cfg_tbl->ss_portmap);

	cfg_tbl->num_hsic_port = tegra->num_hsic_port;
	dev_dbg(dev, "%s num_hsic_port %d\n", __func__, cfg_tbl->num_hsic_port);

	if (cfg_tbl->build_log == LOG_MEMORY)
		fw_log_init(tegra);

	/* update the phys_log_buffer and total_entries here */
	if (test_bit(FW_LOG_CONTEXT_VALID, &tegra->log.flags)) {
		cfg_tbl->phys_addr_log_buffer = tegra->log.phys_addr;
		cfg_tbl->total_log_entries = FW_LOG_COUNT;
	}

	/* Program the size of DFI into ILOAD_ATTR. */
	csb_writel(tegra, tegra->fw_size, XUSB_CSB_MP_ILOAD_ATTR);

	/*
	 * Boot code of the firmware reads the ILOAD_BASE registers
	 * to get to the start of the DFI in system memory.
	 */
	fw_base = tegra->fw_dma_addr + sizeof(*cfg_tbl);
	csb_writel(tegra, fw_base, XUSB_CSB_MP_ILOAD_BASE_LO);
	csb_writel(tegra, fw_base >> 32, XUSB_CSB_MP_ILOAD_BASE_HI);

	/* Set BOOTPATH to 1 in APMAP. */
	csb_writel(tegra, APMAP_BOOTPATH, XUSB_CSB_MP_APMAP);

	/* Invalidate L2IMEM. */
	csb_writel(tegra, L2IMEMOP_INVALIDATE_ALL, XUSB_CSB_MP_L2IMEMOP_TRIG);

	/*
	 * Initiate fetch of bootcode from system memory into L2IMEM.
	 * Program bootcode location and size in system memory.
	 */
	code_tag_blocks = DIV_ROUND_UP(le32_to_cpu(cfg_tbl->boot_codetag),
				       IMEM_BLOCK_SIZE);
	code_size_blocks = DIV_ROUND_UP(le32_to_cpu(cfg_tbl->boot_codesize),
					IMEM_BLOCK_SIZE);
	val = ((code_tag_blocks & L2IMEMOP_SIZE_SRC_OFFSET_MASK) <<
	       L2IMEMOP_SIZE_SRC_OFFSET_SHIFT) |
	      ((code_size_blocks & L2IMEMOP_SIZE_SRC_COUNT_MASK) <<
	       L2IMEMOP_SIZE_SRC_COUNT_SHIFT);
	csb_writel(tegra, val, XUSB_CSB_MP_L2IMEMOP_SIZE);

	/* Trigger L2IMEM Load operation. */
	csb_writel(tegra, L2IMEMOP_LOAD_LOCKED_RESULT,
		   XUSB_CSB_MP_L2IMEMOP_TRIG);

	/* Setup Falcon Auto-fill. */
	csb_writel(tegra, code_size_blocks, XUSB_FALC_IMFILLCTL);

	val = ((code_tag_blocks & IMFILLRNG1_TAG_MASK) <<
	       IMFILLRNG1_TAG_LO_SHIFT) |
	      (((code_size_blocks + code_tag_blocks) & IMFILLRNG1_TAG_MASK) <<
	       IMFILLRNG1_TAG_HI_SHIFT);
	csb_writel(tegra, val, XUSB_FALC_IMFILLRNG1);

	fw_time = le32_to_cpu(cfg_tbl->fwimg_created_time);
	time_to_tm(fw_time, 0, &fw_tm);
	dev_info(dev, "Firmware timestamp: %ld-%02d-%02d %02d:%02d:%02d UTC, Version: %2x.%02x %s\n",
		fw_tm.tm_year + 1900, fw_tm.tm_mon + 1,
		fw_tm.tm_mday, fw_tm.tm_hour,
		fw_tm.tm_min, fw_tm.tm_sec,
		FW_MAJOR_VERSION(cfg_tbl->version_id),
		FW_MINOR_VERSION(cfg_tbl->version_id),
		(cfg_tbl->build_log == LOG_MEMORY) ? "debug" : "release");

	csb_writel(tegra, 0, XUSB_FALC_DMACTL);
	/* wait for RESULT_VLD to get set */
	timeout = jiffies + msecs_to_jiffies(10);
	do {
		val = csb_readl(tegra, XUSB_CSB_MEMPOOL_L2IMEMOP_RESULT);
		if (val & L2IMEMOP_RESULT_VLD)
			break;
		usleep_range(50, 60);
	} while (time_is_after_jiffies(timeout));

	if (!(val & L2IMEMOP_RESULT_VLD)) {
		dev_err(dev, "DMA controller not ready 0x08%x\n", val);
		return -EFAULT;
	}

	csb_writel(tegra, le32_to_cpu(cfg_tbl->boot_codetag),
		   XUSB_FALC_BOOTVEC);

	/* Start Falcon CPU */
	csb_writel(tegra, CPUCTL_STARTCPU, XUSB_FALC_CPUCTL);

	/* wait for USBSTS_CNR to get cleared */
	cap_regs = tegra->hcd->regs;
	op_regs = tegra->hcd->regs + HC_LENGTH(ioread32(&cap_regs->hc_capbase));
	timeout = jiffies + msecs_to_jiffies(200);
	do {
		val = ioread32(&op_regs->status);
		if (!(val & STS_CNR))
			break;
		usleep_range(1000, 2000);
	} while (time_is_after_jiffies(timeout));

	val = ioread32(&op_regs->status);
	if (val & STS_CNR) {
		dev_err(dev, "XHCI Controller not ready. Falcon state: 0x%x\n",
			csb_readl(tegra, XUSB_FALC_CPUCTL));
		return -EFAULT;
	}

	return 0;
}

static int tegra_xhci_phy_enable(struct tegra_xhci_hcd *tegra)
{
	struct device *dev = tegra->dev;
	unsigned int i, j;
	int ret;

	/* enable CDP on non-OTG port */
	for (j = 0; j < tegra->soc_config->num_phys[UTMI_PHY]; j++) {
		if (j == tegra->utmi_otg_port_base_1 - 1)
			continue;
		ret = phy_init(tegra->cdp_phys[j]);
		if (ret) {
			dev_dbg(dev, "phy_init CDP phy %d failed %d\n", j, ret);
			continue;
		}
		ret = phy_power_on(tegra->cdp_phys[j]);
		if (ret) {
			dev_dbg(dev, "phy_power_on CDP phy %d failed %d\n", j,
				 ret);
			ret = phy_exit(tegra->cdp_phys[j]);
			if (ret)
				dev_dbg(dev, "phy_exit CDP phy %d failed: %d",
					 j, ret);
		}
	}

	for (i = 0; i < ARRAY_SIZE(tegra->phys); i++) {
		for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
			ret = phy_init(tegra->phys[i][j]);
			if (ret) {
				dev_dbg(dev, "phy_init %s phy %d failed\n",
					tegra_phy_names[i], j);
				goto disable_phy;
			}
			ret = phy_power_on(tegra->phys[i][j]);
			if (ret) {
				dev_dbg(dev, "phy_power_on %s phy %d failed\n",
					tegra_phy_names[i], j);

				ret = phy_exit(tegra->phys[i][j]);
				if (ret) {
					dev_dbg(dev, "phy_exit %s phy %d failed\n",
						tegra_phy_names[i], j);
				}
				goto disable_phy;
			}
		}
	}

	return 0;
disable_phy:
	for (; j > 0; j--) {
		ret = phy_power_off(tegra->phys[i][j - 1]);
		if (ret) {
			dev_dbg(dev, "phy_power_off %s phy %d failed\n",
				tegra_phy_names[i], j);
		}
		ret = phy_exit(tegra->phys[i][j - 1]);
		if (ret) {
			dev_dbg(dev, "phy_exit %s phy %d failed\n",
				tegra_phy_names[i], j);
		}
	}
	for (; i > 0; i--) {
		for (j = 0; j < tegra->soc_config->num_phys[i - 1]; j++) {
			ret = phy_power_off(tegra->phys[i - 1][j]);
			if (ret) {
				dev_dbg(dev, "phy_power_off %s phy %d failed\n",
					tegra_phy_names[i], j);
			}
			ret = phy_exit(tegra->phys[i - 1][j]);
			if (ret) {
				dev_dbg(dev, "phy_exit %s phy %d failed\n",
					tegra_phy_names[i], j);
			}
		}
	}
	return ret;
}

static void tegra_xhci_phy_disable(struct tegra_xhci_hcd *tegra)
{
	struct device *dev = tegra->dev;
	unsigned int i, j, ret;

	for (i = 0; i < ARRAY_SIZE(tegra->phys); i++) {
		for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
			ret = phy_power_off(tegra->phys[i][j]);
			if (ret) {
				dev_dbg(dev, "phy_power_off %s phy %d failed\n",
					tegra_phy_names[i], j);
			}
			ret = phy_exit(tegra->phys[i][j]);
			if (ret) {
				dev_dbg(dev, "phy_exit %s phy %d failed\n",
					tegra_phy_names[i], j);
			}
		}
	}

	/* disable CDP on non-OTG ports */
	for (j = 0; j < tegra->soc_config->num_phys[UTMI_PHY]; j++) {
		if (j == tegra->utmi_otg_port_base_1 - 1)
			continue;
		ret = phy_power_off(tegra->cdp_phys[j]);
		if (ret)
			dev_dbg(dev, "phy_power_off CDP phy %d failed %d\n", j,
				ret);
		ret = phy_exit(tegra->cdp_phys[j]);
		if (ret)
			dev_dbg(dev, "phy_exit CDP phy %d failed %d\n", j,
				ret);
	}

}

static bool is_host_mbox_message(u32 cmd)
{
	switch (cmd) {
	case MBOX_CMD_INC_SSPI_CLOCK:
	case MBOX_CMD_DEC_SSPI_CLOCK:
	case MBOX_CMD_INC_FALC_CLOCK:
	case MBOX_CMD_DEC_FALC_CLOCK:
	case MBOX_CMD_SET_BW:
		return true;
	default:
		return false;
	}
}

static void tegra_xhci_mbox_work(struct work_struct *work)
{
	struct tegra_xhci_hcd *tegra = mbox_work_to_tegra(work);
	struct tegra_xusb_mbox_msg *msg = &tegra->mbox_req;
	struct tegra_xusb_mbox_msg resp;
	int ret;
	unsigned long freq_khz;

	dev_info(tegra->dev, "%s mailbox command %d\n", __func__, msg->cmd);
	resp.cmd = 0;
	switch (msg->cmd) {
	case MBOX_CMD_INC_SSPI_CLOCK:
	case MBOX_CMD_DEC_SSPI_CLOCK:
		/* TODO: implement dynamic SS clock */
		dev_info(tegra->dev, "%s ignore firmware %s request\n",
			__func__, (msg->cmd == MBOX_CMD_INC_SSPI_CLOCK) ?
			"MBOX_CMD_INC_SSPI_CLOCK" : "MBOX_CMD_DEC_SSPI_CLOCK");
		resp.data = msg->data;
		resp.cmd = MBOX_CMD_ACK;
		break;
	case MBOX_CMD_INC_FALC_CLOCK:
	case MBOX_CMD_DEC_FALC_CLOCK:
		/* TODO: implement dynamic falcon clock */
		dev_info(tegra->dev, "%s ignore firmware %s request\n",
			__func__, (msg->cmd == MBOX_CMD_INC_FALC_CLOCK) ?
			"MBOX_CMD_INC_FALC_CLOCK" : "MBOX_CMD_DEC_FALC_CLOCK");
		resp.data = msg->data;
		resp.cmd = MBOX_CMD_ACK;
		break;
	case MBOX_CMD_SET_BW:
		/* fw sends bw request in MByte/sec, convert to HZ */
		freq_khz = bwmgr_bw_to_freq(msg->data << 10);
		ret = tegra_bwmgr_set_emc(tegra->bwmgr_handle,
			freq_khz * 1000, TEGRA_BWMGR_SET_EMC_SHARED_BW);
		if (ret)
			dev_warn(tegra->dev,
				"failed to set EMC khz=%lu errno=%d\n",
				freq_khz, ret);
		resp.cmd = MBOX_CMD_COMPL;
		break;
	case MBOX_CMD_ACK:
	case MBOX_CMD_NAK:
		if (XHCI_IS_T210(tegra)) {
			if (msg->cmd == MBOX_CMD_ACK)
				dev_dbg(tegra->dev,
					"%s firmware responds ACK\n",
					__func__);
			else
				dev_warn(tegra->dev,
					"%s firmware responds NAK\n",
					__func__);
			/* inform processes that needs FW ACK */
			tegra->fw_ack = msg->cmd;
			wake_up_interruptible(&tegra->fw_ack_wq);
		}
		break;
	default:
		break;
	}

	if (resp.cmd)
		mbox_send_message(tegra->mbox_chan, &resp);
}

static void tegra_xhci_mbox_rx(struct mbox_client *cl, void *data)
{
	struct tegra_xhci_hcd *tegra = dev_get_drvdata(cl->dev);
	struct tegra_xusb_mbox_msg *msg = data;

	if (is_host_mbox_message(msg->cmd) || true) {
		tegra->mbox_req = *msg;
		schedule_work(&tegra->mbox_req_work);
	}
}

static void tegra_xhci_quirks(struct device *dev, struct xhci_hcd *xhci)
{
	struct tegra_xhci_hcd *tegra = dev_get_drvdata(dev);

	xhci->quirks |= XHCI_PLAT;
	if (tegra && tegra->soc_config->lpm_support)
		xhci->quirks |= XHCI_LPM_SUPPORT;

}

static int tegra_xhci_setup(struct usb_hcd *hcd)
{
	return xhci_gen_setup(hcd, tegra_xhci_quirks);
}

static const struct tegra_xhci_soc_config tegra186_soc_config = {
	.device_id = XHCI_DEVICE_ID_T186,
	.firmware_file = "tegra18x_xusb_firmware",
	.lpm_support = true,

	.num_supplies = 3,
	.supply_names[0] = "avddio_usb",
	.supply_names[1] = "avdd_pll_utmip",
	.supply_names[2] = "hvdd_usb",

	.num_phys[USB3_PHY] = 3,
	.num_phys[UTMI_PHY] = 3,
	.num_phys[HSIC_PHY] = 1,

	.utmi_port_offset = 3,
	.hsic_port_offset = 6,

	.utmi_pad_power_on = tegra18x_phy_xusb_utmi_pad_power_on,
	.utmi_pad_power_off = tegra18x_phy_xusb_utmi_pad_power_down,
	.set_protection_level =
		tegra18x_phy_xusb_utmi_pad_set_protection_level,
	.utmi_vbus_power_on = tegra18x_phy_xusb_utmi_vbus_power_on,
	.utmi_vbus_power_off = tegra18x_phy_xusb_utmi_vbus_power_off,
	.utmi_vbus_enable = tegra18x_utmi_vbus_enable,
	.overcurrent_detected = tegra18x_phy_xusb_overcurrent_detected,
	.handle_overcurrent = tegra18x_phy_xusb_handle_overcurrent,
	.set_id_override = tegra18x_phy_xusb_set_id_override,
	.clear_id_override = tegra18x_phy_xusb_clear_id_override,
	.has_otg_cap = tegra18x_phy_xusb_has_otg_cap,
	.enable_sleepwalk = tegra18x_phy_xusb_enable_sleepwalk,
	.disable_sleepwalk = tegra18x_phy_xusb_disable_sleepwalk,
	.enable_wake = tegra18x_phy_xusb_enable_wake,
	.disable_wake = tegra18x_phy_xusb_disable_wake,
	.pretend_connected = tegra18x_phy_xusb_pretend_connected,
	.remote_wake_detected = tegra18x_phy_xusb_remote_wake_detected,
	.disable_u0_ts1_detect = false,
};
MODULE_FIRMWARE("tegra18x_xusb_firmware");

static const struct tegra_xhci_soc_config tegra210b01_soc_config = {
	.device_id = XHCI_DEVICE_ID_T210,
	.firmware_file = "tegra21x_xusb_firmware",
	.lpm_support = true,

	.num_supplies = 4,
	.supply_names[0] = "hvdd_usb",
	.supply_names[1] = "avdd_pll_utmip",
	.supply_names[2] = "avddio_usb",
	.supply_names[3] = "avddio_pll_uerefe",

	.num_phys[USB3_PHY] = 4,
	.num_phys[UTMI_PHY] = 4,
	.num_phys[HSIC_PHY] = 1,

	.utmi_port_offset = 4,
	.hsic_port_offset = 8,

	.utmi_pad_power_on = tegra21x_phy_xusb_utmi_pad_power_on,
	.utmi_pad_power_off = tegra21x_phy_xusb_utmi_pad_power_down,
	.set_protection_level =
		tegra21x_phy_xusb_utmi_pad_set_protection_level,
	.utmi_vbus_power_on = tegra21x_phy_xusb_utmi_vbus_power_on,
	.utmi_vbus_power_off = tegra21x_phy_xusb_utmi_vbus_power_off,
	.utmi_vbus_enable = tegra21x_utmi_vbus_enable,
	.overcurrent_detected = tegra21x_phy_xusb_overcurrent_detected,
	.handle_overcurrent = tegra21x_phy_xusb_handle_overcurrent,
	.set_id_override = tegra21x_phy_xusb_set_id_override,
	.clear_id_override = tegra21x_phy_xusb_clear_id_override,
	.has_otg_cap = tegra21x_phy_xusb_has_otg_cap,
	.enable_sleepwalk = tegra21x_phy_xusb_enable_sleepwalk,
	.disable_sleepwalk = tegra21x_phy_xusb_disable_sleepwalk,
	.enable_wake = tegra21x_phy_xusb_enable_wake,
	.disable_wake = tegra21x_phy_xusb_disable_wake,
	.pretend_connected = tegra21x_phy_xusb_pretend_connected,
	.remote_wake_detected = tegra21x_phy_xusb_remote_wake_detected,
	.disable_u0_ts1_detect = false,
};

static const struct tegra_xhci_soc_config tegra210_soc_config = {
	.device_id = XHCI_DEVICE_ID_T210,
	.firmware_file = "tegra21x_xusb_firmware",
	.lpm_support = true,

	.num_supplies = 6,
	.supply_names[0] = "hvdd_usb",
	.supply_names[1] = "avdd_pll_utmip",
	.supply_names[2] = "vddio_hsic",
	.supply_names[3] = "avddio_usb",
	.supply_names[4] = "dvdd_sata",
	.supply_names[5] = "avddio_pll_uerefe",

	.num_phys[USB3_PHY] = 4,
	.num_phys[UTMI_PHY] = 4,
	.num_phys[HSIC_PHY] = 1,

	.utmi_port_offset = 4,
	.hsic_port_offset = 8,

	.utmi_pad_power_on = tegra21x_phy_xusb_utmi_pad_power_on,
	.utmi_pad_power_off = tegra21x_phy_xusb_utmi_pad_power_down,
	.set_protection_level =
		tegra21x_phy_xusb_utmi_pad_set_protection_level,
	.utmi_vbus_power_on = tegra21x_phy_xusb_utmi_vbus_power_on,
	.utmi_vbus_power_off = tegra21x_phy_xusb_utmi_vbus_power_off,
	.utmi_vbus_enable = tegra21x_utmi_vbus_enable,
	.overcurrent_detected = tegra21x_phy_xusb_overcurrent_detected,
	.handle_overcurrent = tegra21x_phy_xusb_handle_overcurrent,
	.set_id_override = tegra21x_phy_xusb_set_id_override,
	.clear_id_override = tegra21x_phy_xusb_clear_id_override,
	.has_otg_cap = tegra21x_phy_xusb_has_otg_cap,
	.enable_sleepwalk = tegra21x_phy_xusb_enable_sleepwalk,
	.disable_sleepwalk = tegra21x_phy_xusb_disable_sleepwalk,
	.enable_wake = tegra21x_phy_xusb_enable_wake,
	.disable_wake = tegra21x_phy_xusb_disable_wake,
	.pretend_connected = tegra21x_phy_xusb_pretend_connected,
	.remote_wake_detected = tegra21x_phy_xusb_remote_wake_detected,

	.receiver_detector = t210_receiver_detector,
	.clamp_en_early = t210_clamp_en_early,
	.disable_u0_ts1_detect = true,
};
MODULE_FIRMWARE("tegra21x_xusb_firmware");

static const struct of_device_id tegra_xhci_of_match[] = {
	{
		.compatible = "nvidia,tegra186-xhci",
		.data = &tegra186_soc_config
	},
	{
		.compatible = "nvidia,tegra210-xhci",
		.data = &tegra210_soc_config
	},
	{
		.compatible = "nvidia,tegra210b01-xhci",
		.data = &tegra210b01_soc_config
	},
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_xhci_of_match);

/*
 * ack_fw_message_send_sync - send FW message and block until receiving FW ACK
 *      This function will block until FW ack is received.
 * @return      0 if FW returns MBOX_CMD_ACK (success)
 *              -EINVAL if FW returns MBOX_CMD_NAK (failure)
 *              -EPIPE if FW returns a mbox type other than ACK or NACK
 *              -ETIMEOUT if either sending or waiting for FW ack times out
 *              -ERESTARTSYS if the wait has been interrupted by a signal
 */
static int ack_fw_message_send_sync(struct tegra_xhci_hcd *tegra,
				enum tegra_xusb_mbox_cmd type, u32 data)
{
	int ret = 0;
	struct tegra_xusb_mbox_msg msg;

	if (!XHCI_IS_T210(tegra))
		return ret;

	mutex_lock(&tegra->mbox_lock_ack);
	tegra->fw_ack = 0;

	/* send mbox message */
	msg.cmd = type;
	msg.data = data;
	ret = mbox_send_message(tegra->mbox_chan, &msg);
	if (ret)
		goto out;

	/* wait for FW ACK with 20ms timeout */
	ret = wait_event_interruptible_timeout(tegra->fw_ack_wq,
			tegra->fw_ack, msecs_to_jiffies(20));
	if (ret == 0) {
		dev_warn(tegra->dev, "%s: timeout waiting for FW msg\n",
				__func__);
		ret = -ETIMEDOUT;
		goto out;
	} else if (ret == -ERESTARTSYS) {
		dev_warn(tegra->dev, "%s: interrupted when waiting\n",
				__func__);
		goto out;
	}

	/* we have got FW ACK here, check what FW returns */
	dev_dbg(tegra->dev, "%s: FW ack type:%u\n",
			__func__, tegra->fw_ack);
	if (tegra->fw_ack == MBOX_CMD_ACK)
		ret = 0;
	else if (tegra->fw_ack == MBOX_CMD_NAK)
		ret = -EINVAL;
	else
		ret = -EPIPE; /* violation in mailbox protocol */

out:
	mutex_unlock(&tegra->mbox_lock_ack);
	return ret;
}

static void tegra_xhci_set_host_mode(struct tegra_xhci_hcd *tegra, bool on)
{
	struct xhci_hcd *xhci;
	int port = tegra->utmi_otg_port_base_1 - 1;
	struct phy *otg_phy;
	u32 status;
	int wait, ret = -1;

	if (!tegra->utmi_otg_port_base_1)
		return;

	if (!tegra->fw_loaded)
		return;

	mutex_lock(&tegra->lock);

	if (tegra->suspended) {
		mutex_unlock(&tegra->lock);
		return;
	}

	otg_phy = tegra->phys[UTMI_PHY][port];

	/* turn on off CDP phy for OTG port only when role changes */
	if ((tegra->host_mode && !on) || (!tegra->host_mode && on)) {
		if (on) {
			dev_dbg(tegra->dev, "%s: power on CDP phy %d\n",
				__func__, port);
			ret = phy_power_on(tegra->cdp_phys[port]);
			if (ret)
				dev_dbg(tegra->dev,
					"%s: power on CDP phy %d failed\n",
					__func__, port);
		} else if (tegra->otg_role_initialized) {
			dev_dbg(tegra->dev, "%s: power off CDP phy %d\n",
				__func__, port);
			ret = phy_power_off(tegra->cdp_phys[port]);
			if (ret)
				dev_dbg(tegra->dev,
					"%s: power off CDP phy %d failed\n",
					__func__, port);
		}
	}

	if (!XHCI_IS_T186(tegra)) {
		if (on) {
			if (tegra->soc_config->set_id_override)
				ret = tegra->soc_config->
					set_id_override(otg_phy);
		} else {
			if (tegra->soc_config->clear_id_override)
				ret = tegra->soc_config->
					clear_id_override(otg_phy);
		}

		if (ret)
			dev_dbg(tegra->dev, "%s ID override failed\n",
				on ? "set" : "clear");
	}

	if (!tegra->otg_role_initialized) {
		tegra->otg_role_initialized = true;
		goto role_update;
	}

	if ((tegra->host_mode && on) || (!tegra->host_mode && !on)) {
		mutex_unlock(&tegra->lock);
		return;
	}

role_update:
	tegra->host_mode = on;
	dev_dbg(tegra->dev, "host mode %s\n", on ? "on" : "off");

	mutex_unlock(&tegra->lock);

	xhci = hcd_to_xhci(tegra->hcd);
	pm_runtime_get_sync(tegra->dev);
	if (on) {
		/* switch to host mode */
		if (tegra->usb3_otg_port_base_1) {
			if (XHCI_IS_T210(tegra)) {
				/* set PP=0 */
				xhci_hub_control(xhci->shared_hcd, GetPortStatus
					, 0, tegra->usb3_otg_port_base_1
					, (char *) &status, sizeof(status));
				if (status & USB_SS_PORT_STAT_POWER) {
					xhci_hub_control(xhci->shared_hcd,
						ClearPortFeature,
						USB_PORT_FEAT_POWER,
						tegra->usb3_otg_port_base_1,
						NULL, 0);
				}

				/* reset OTG port SSPI */
				ack_fw_message_send_sync(tegra,
						MBOX_CMD_RESET_SSPI,
						tegra->usb3_otg_port_base_1);
			}

			xhci_hub_control(xhci->shared_hcd, SetPortFeature,
				USB_PORT_FEAT_POWER, tegra->usb3_otg_port_base_1
				, NULL, 0);

			wait = 10;
			do {
				xhci_hub_control(xhci->shared_hcd, GetPortStatus
					, 0, tegra->usb3_otg_port_base_1
					, (char *) &status, sizeof(status));
				if (status & USB_SS_PORT_STAT_POWER)
					break;
				usleep_range(10, 20);
			} while (--wait > 0);

			if (!(status & USB_SS_PORT_STAT_POWER))
				dev_info(tegra->dev, "failed to set SS PP\n");
		}

		xhci_hub_control(xhci->main_hcd, SetPortFeature,
			USB_PORT_FEAT_POWER, tegra->utmi_otg_port_base_1,
			NULL, 0);

		wait = 10;
		do {
			xhci_hub_control(xhci->main_hcd, GetPortStatus
				, 0, tegra->utmi_otg_port_base_1
				, (char *) &status, sizeof(status));
			if (status & USB_PORT_STAT_POWER)
				break;
			usleep_range(10, 20);
		} while (--wait > 0);

		if (!(status & USB_PORT_STAT_POWER))
			dev_info(tegra->dev, "failed to set HS PP\n");

		pm_runtime_mark_last_busy(tegra->dev);
	} else {
		if (tegra->usb3_otg_port_base_1) {
			xhci_hub_control(xhci->shared_hcd, ClearPortFeature,
				USB_PORT_FEAT_POWER, tegra->usb3_otg_port_base_1
				, NULL, 0);

			wait = 10;
			do {
				xhci_hub_control(xhci->shared_hcd, GetPortStatus
					, 0, tegra->usb3_otg_port_base_1
					, (char *) &status, sizeof(status));
				if (!(status & USB_SS_PORT_STAT_POWER))
					break;
				usleep_range(10, 20);
			} while (--wait > 0);

			if (status & USB_SS_PORT_STAT_POWER)
				dev_info(tegra->dev, "failed to clear SS PP\n");
		}

		xhci_hub_control(xhci->main_hcd, ClearPortFeature,
			USB_PORT_FEAT_POWER, tegra->utmi_otg_port_base_1,
			NULL, 0);

		wait = 10;
		do {
			xhci_hub_control(xhci->main_hcd, GetPortStatus
				, 0, tegra->utmi_otg_port_base_1
				, (char *) &status, sizeof(status));
			if (!(status & USB_PORT_STAT_POWER))
				break;
			usleep_range(10, 20);
		} while (--wait > 0);

		if (status & USB_PORT_STAT_POWER)
			dev_info(tegra->dev, "failed to clear HS PP\n");

	}
	pm_runtime_put_autosuspend(tegra->dev);

}
static void tegra_xhci_update_otg_role(struct tegra_xhci_hcd *tegra)
{
	if (IS_ERR(tegra->id_extcon))
		return;

	if (extcon_get_cable_state_(tegra->id_extcon, EXTCON_USB_HOST))
		tegra_xhci_set_host_mode(tegra, true);
	else
		tegra_xhci_set_host_mode(tegra, false);
}

static void tegra_xhci_id_extcon_work(struct work_struct *work)
{
	struct tegra_xhci_hcd *tegra = container_of(work, struct tegra_xhci_hcd,
						    id_extcon_work);

	tegra_xhci_update_otg_role(tegra);
}

static int tegra_xhci_id_notifier(struct notifier_block *nb,
					 unsigned long action, void *data)
{
	struct tegra_xhci_hcd *tegra = container_of(nb, struct tegra_xhci_hcd,
						    id_extcon_nb);

	/* nothing to do if there is no UTMI otg_cap port */
	if (tegra->utmi_otg_port_base_1 == 0)
		return NOTIFY_OK;

	schedule_work(&tegra->id_extcon_work);

	return NOTIFY_OK;
}

static void tegra_xhci_oc_work(struct work_struct *work)
{
	struct tegra_xhci_hcd *tegra = container_of(work, struct tegra_xhci_hcd,
						    oc_work);

	/* it doesn't matter which phy instance we pass to this function
	 * since it will handle all overcurrent events there
	 */
	if (tegra->soc_config->handle_overcurrent)
		tegra->soc_config->handle_overcurrent(tegra->phys[UTMI_PHY][0]);
}

static void tegra_xhci_probe_finish(const struct firmware *fw, void *context);

static void tegra_firmware_retry_work(struct work_struct *work)
{
	struct tegra_xhci_hcd *tegra;
	struct device *dev;
	int ret;
	bool uevent = true;

	tegra = container_of(to_delayed_work(work),
			struct tegra_xhci_hcd, firmware_retry_work);
	dev = tegra->dev;

	if (++tegra->fw_retry_count >= FW_RETRY_COUNT) {
		/*
		 * Last retry.  If CONFIG_FW_LOADER_USER_HELPER_FALLBACK=y and
		 * uevent=true, in udev based systems, userspace will not get
		 * any chance to provide firmware file.  This happens due to
		 * udev stubs aborting firmware requests made by kernel.
		 *
		 * Hence try now with uevent=false so that udev does not abort
		 * syfs based firmware loading interface.  User can load the
		 * firmware later at any point.
		 *
		 * NOTE: If !CONFIG_FW_LOADER_USER_HELPER &&
		 * !CONFIG_FW_LOADER_USER_HELPER_FALLBACK, last retry will
		 * be a direct try from rootfs like the previous retries.
		 */
		uevent = false;
		dev_err(dev, "Leaving it upto user to load firmware!\n");
	}

	ret = request_firmware_nowait(THIS_MODULE, uevent,
				tegra->soc_config->firmware_file,
				tegra->dev, GFP_KERNEL, tegra,
				tegra_xhci_probe_finish);
	if (ret) {
		dev_err(dev,
			"Could not submit async request for firmware load %d\n"
			, ret);
		usb_put_hcd(tegra->hcd);
		tegra->hcd = NULL;
	}
}

static int tegra_xhci_otg_hcd_start(struct usb_hcd *hcd)
{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);

	dev_dbg(tegra->dev, "OTG start(): %s\n", __func__);
	tegra_xhci_set_host_mode(tegra, true);

	return 0;
}

static int tegra_xhci_otg_hcd_stop(struct usb_hcd *hcd)
{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);

	dev_dbg(tegra->dev, "OTG stop(): %s\n", __func__);
	tegra_xhci_set_host_mode(tegra, false);

	return 0;
}

struct otg_hcd_ops tegra_xhci_otg_hcd_ops = {
	.start = tegra_xhci_otg_hcd_start,
	.stop = tegra_xhci_otg_hcd_stop,
};

static void tegra_xhci_otg_retry_work(struct work_struct *work)
{
	struct tegra_xhci_hcd *tegra;
	int ret;

	tegra = container_of(to_delayed_work(work),
			struct tegra_xhci_hcd, otg_retry_work);

	tegra->hcd->otg_dev = of_usb_get_otg(tegra->dev->of_node);
	ret = usb_otg_register_hcd(tegra->hcd,
				   &tegra_xhci_otg_hcd_ops);
	if (ret)
		dev_err(tegra->dev, "failed to register to OTG core (%d) after retry\n",
			ret);
	else
		dev_dbg(tegra->dev, "successfully registered to OTG core\n");
}

static void tegra_xhci_probe_finish(const struct firmware *fw, void *context)
{
	struct tegra_xhci_hcd *tegra = context;
	struct device *dev = tegra->dev;
	struct xhci_hcd *xhci = NULL;
	struct tegra_xhci_fw_cfgtbl *cfg_tbl;
	struct tegra_xusb_mbox_msg msg;
	int ret;
	int i;
	u32 val;

	if (!fw) {

		if (tegra->fw_retry_count >= FW_RETRY_COUNT) {
			dev_err(dev, "Giving up on firmware\n");
			goto put_usb2_hcd;
		}

		dev_err(dev, "cannot find firmware....retry after 1 second\n");
		schedule_delayed_work(&tegra->firmware_retry_work,
					msecs_to_jiffies(1000));
		return;
	}

	/* Load Falcon controller with its firmware. */
	cfg_tbl = (struct tegra_xhci_fw_cfgtbl *)fw->data;
	tegra->fw_size = le32_to_cpu(cfg_tbl->fwimg_len);
	tegra->fw_data = dma_alloc_coherent(dev, tegra->fw_size,
					    &tegra->fw_dma_addr,
					    GFP_KERNEL);

	if (!tegra->fw_data) {
		dev_warn(dev, "can't alloc memory for firmware\n");
		goto put_usb2_hcd;
	}
	memcpy(tegra->fw_data, fw->data, tegra->fw_size);

	ret = tegra_xhci_load_firmware(tegra);
	if (ret < 0) {
		dev_warn(dev, "can't load firmware (%d)\n", ret);
		goto put_usb2_hcd;
	}

	ret = usb_add_hcd(tegra->hcd, tegra->irq, IRQF_SHARED);
	if (ret < 0)
		goto put_usb2_hcd;
	device_wakeup_enable(tegra->hcd->self.controller);

	if (tegra->soc_config->utmi_vbus_enable) {
		for (i = 0; i < tegra->soc_config->num_phys[UTMI_PHY]; i++) {
			if (tegra->phys[UTMI_PHY][i]) {
				ret = tegra->soc_config->utmi_vbus_enable(
					tegra->phys[UTMI_PHY][i]);
				if (ret)
					dev_err(dev, "enable port vbus-%d failed %d\n",
						i, ret);
			}
		}
	}

	xhci = hcd_to_xhci(tegra->hcd);
	xhci->shared_hcd = usb_create_shared_hcd(&tegra_xhci_hc_driver,
						 dev, dev_name(dev),
						 tegra->hcd);
	if (!xhci->shared_hcd)
		goto dealloc_usb2_hcd;

	/*
	 * Set the xHCI pointer before xhci_plat_setup() (aka hcd_driver.reset)
	 * is called by usb_add_hcd().
	 */
	*((struct xhci_hcd **) xhci->shared_hcd->hcd_priv) = xhci;

	ret = usb_add_hcd(xhci->shared_hcd, tegra->irq, IRQF_SHARED);
	if (ret < 0)
		goto put_usb3_hcd;

	/* Enable wake for both USB2.0 and USB3.0 hub */
	device_init_wakeup(&tegra->hcd->self.root_hub->dev, true);
	device_init_wakeup(&xhci->shared_hcd->self.root_hub->dev, true);

	/* Set SS OTG port to the usb bus */
	if (tegra->usb3_otg_port_base_1)
		xhci->shared_hcd->self.otg_port = tegra->usb3_otg_port_base_1;

	/* Enable firmware messages from controller. */
	msg.cmd = MBOX_CMD_MSG_ENABLED;
	msg.data = 0;
	ret = mbox_send_message(tegra->mbox_chan, &msg);
	if (ret < 0) {
		dev_warn(dev, "can't send message to firmware (%d)\n", ret);
		goto dealloc_usb3_hcd;
	}

	tegra->fw_loaded = true;

	if (XHCI_IS_T186(tegra)) {
		/* if there is any OTG port, register to otg core */
		if (tegra->utmi_otg_port_base_1) {
			tegra->hcd->otg_dev = of_usb_get_otg(dev->of_node);
			ret = usb_otg_register_hcd(tegra->hcd,
						   &tegra_xhci_otg_hcd_ops);
			if (ret) {
				dev_err(dev, "failed to register to OTG core (%d)...retry after 1 second\n",
					ret);
				schedule_delayed_work(&tegra->otg_retry_work,
						      msecs_to_jiffies(1000));
			} else
				dev_dbg(dev, "registered to OTG core\n");
		}
	} else {
		tegra_xhci_update_otg_role(tegra);
	}

	/* set pretend connected per HSIC PHY DTB */
	for (i = 0; i < tegra->soc_config->num_phys[HSIC_PHY]; i++) {
		struct phy *hsic_phy = tegra->phys[HSIC_PHY][i];

		if (tegra->soc_config->pretend_connected)
			ret = tegra->soc_config->pretend_connected(hsic_phy);
		if (ret)
			dev_dbg(dev, "HSIC phy %d pretend connect failed\n", i);
	}

	release_firmware(fw);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 2000);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/* Enable EU3S bit of USBCMD */
	val = readl(&xhci->op_regs->command);
	val |= CMD_PM_INDEX;
	writel(val, &xhci->op_regs->command);

	return;

	/* Free up as much as we can and wait to be unbound. */
dealloc_usb3_hcd:
	usb_remove_hcd(xhci->shared_hcd);
put_usb3_hcd:
	usb_put_hcd(xhci->shared_hcd);
dealloc_usb2_hcd:
	usb_remove_hcd(tegra->hcd);
	kfree(xhci);
put_usb2_hcd:
	usb_put_hcd(tegra->hcd);
	tegra->hcd = NULL;
	release_firmware(fw);
}

static void fpga_hacks_init(struct platform_device *pdev,
				struct tegra_xhci_hcd *tegra)
{
	if (XHCI_IS_T186(tegra))
		car_base = devm_ioremap_nocache(&pdev->dev,
						0x05000000, 0x1000000);
	if (!car_base)
		dev_err(&pdev->dev, "failed to map CAR mmio\n");

	if (XHCI_IS_T186(tegra))
		padctl_base = devm_ioremap_nocache(&pdev->dev,
						0x03520000, 0x2000);
	if (!padctl_base)
		dev_err(&pdev->dev, "failed to map PADCTL mmio\n");
}

static
void fpga_hacks_partition_reset(struct platform_device *pdev,
				struct tegra_xhci_hcd *tegra, bool on)
{
	if (XHCI_IS_T186(tegra)) {
		struct device *dev = &pdev->dev;
		u32 val;

		if (!car_base) {
			dev_err(dev, "not able to access CAR mmio\n");
			return;
		}

		pr_debug("%s %sassert\n", __func__, on ? "" : "de");

		reg_dump(dev, car_base, CLK_RST_CONTROLLER_RST_DEV_XUSB_0);
		val = ioread32(car_base + CLK_RST_CONTROLLER_RST_DEV_XUSB_0);
		if (on)
			val |= (SWR_XUSB_HOST_RST | SWR_XUSB_DEV_RST |
				SWR_XUSB_PADCTL_RST | SWR_XUSB_SS_RST);
		else
			val &= ~(SWR_XUSB_HOST_RST | SWR_XUSB_DEV_RST |
				SWR_XUSB_PADCTL_RST | SWR_XUSB_SS_RST);
		iowrite32(val, car_base + CLK_RST_CONTROLLER_RST_DEV_XUSB_0);
		reg_dump(dev, car_base, CLK_RST_CONTROLLER_RST_DEV_XUSB_0);
	}
}

static void tegra_xhci_debugfs_init(struct tegra_xhci_hcd *tegra)
{
	struct device *dev = tegra->dev;

	tegra->debugfs_dir = debugfs_create_dir("tegra_xhci", NULL);
	if (IS_ERR_OR_NULL(tegra->debugfs_dir)) {
		tegra->debugfs_dir = NULL;
		dev_warn(dev, "debugfs_create_dir() for tegra_xhci failed\n");
		return;
	}

#ifdef CONFIG_USB_XHCI_HCD_DEBUGGING
	tegra->dump_ring_file = debugfs_create_file("dump_ring",
		(S_IWUSR|S_IWGRP), tegra->debugfs_dir, tegra, &dump_ring_fops);
	if (IS_ERR_OR_NULL(tegra->dump_ring_file)) {
		tegra->dump_ring_file = NULL;
		dev_warn(dev, "debugfs_create_file() for dump_ring failed\n");
		return;
	}
#endif

}

static void tegra_xhci_debugfs_deinit(struct tegra_xhci_hcd *tegra)
{
#ifdef CONFIG_USB_XHCI_HCD_DEBUGGING
	debugfs_remove(tegra->dump_ring_file);
	tegra->dump_ring_file = NULL;
#endif

	debugfs_remove(tegra->debugfs_dir);
	tegra->debugfs_dir = NULL;
}

static irqreturn_t tegra_xhci_padctl_irq(int irq, void *dev_id)
{
	struct tegra_xhci_hcd *tegra = dev_id;
	int i, j;
	bool remote_wakeup = false;
	bool oc = false;

	if (XHCI_IS_T186(tegra)) {
		for (i = 0; i < MAX_PHY_TYPES; i++) {
			for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
				if (tegra->soc_config->remote_wake_detected &&
				    tegra->soc_config->remote_wake_detected(
						tegra->phys[i][j]) > 0) {
					dev_dbg(tegra->dev,
						"%s port %d remote wake detected\n",
						tegra_phy_names[i], j);
					remote_wakeup = true;
				}
				if (i == UTMI_PHY &&
				tegra->soc_config->overcurrent_detected &&
				tegra->soc_config->overcurrent_detected(
						tegra->phys[i][j]) > 0) {
					dev_warn(tegra->dev,
						 "port %d over-current detected\n",
						 j);
					oc = true;
				}
			}
		}

		if (!remote_wakeup && !oc) {
			dev_dbg(tegra->dev,
				"padctl interrupt isn't for XHCI\n");
			return IRQ_NONE;
		}

		/* call padctl API to clear OC condition */
		if (oc)
			schedule_work(&tegra->oc_work);
	}

	pm_runtime_resume(tegra->dev);

	return IRQ_HANDLED;
}

static void tegra_xhci_parse_dt(struct platform_device *pdev,
				struct tegra_xhci_hcd *tegra)
{
	struct device_node *node = pdev->dev.of_node;

	tegra->pmc_usb_wakes_disabled = of_property_read_bool(node,
				"nvidia,pmc-usb-wakes-disabled");
	of_property_read_u32(node, "nvidia,boost_cpu_freq",
					&tegra->boost_cpu_freq);
	of_property_read_u32(node, "nvidia,boost_cpu_trigger",
					&tegra->boost_cpu_trigger);
}

static int tegra_xhci_clk_enable(struct tegra_xhci_hcd *tegra)
{
	int ret = 0;

	if (XHCI_IS_T210(tegra)) {
		/* enable clks */
		ret = clk_prepare_enable(tegra->ss_clk);
		if (ret < 0)
			return ret;
		ret = clk_prepare_enable(tegra->host_clk);
		if (ret < 0)
			goto disable_ss;
		ret = clk_prepare_enable(tegra->falc_clk);
		if (ret < 0)
			goto disable_host;
		ret = clk_prepare_enable(tegra->fs_src_clk);
		if (ret < 0)
			goto disable_falc;
		ret = clk_prepare_enable(tegra->hs_src_clk);
		if (ret < 0)
			goto disable_fs_src;

		return 0;

disable_fs_src:
		clk_disable_unprepare(tegra->fs_src_clk);
disable_falc:
		clk_disable_unprepare(tegra->falc_clk);
disable_host:
		clk_disable_unprepare(tegra->host_clk);
disable_ss:
		clk_disable_unprepare(tegra->ss_clk);
	}
	return ret;
}

static void tegra_xhci_clk_disable(struct tegra_xhci_hcd *tegra)
{
	if (XHCI_IS_T210(tegra)) {
		clk_disable_unprepare(tegra->hs_src_clk);
		clk_disable_unprepare(tegra->fs_src_clk);
		clk_disable_unprepare(tegra->falc_clk);
		clk_disable_unprepare(tegra->host_clk);
		clk_disable_unprepare(tegra->ss_clk);
	}
}

static int tegra_xhci_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct tegra_xhci_hcd *tegra;
	struct resource *res;
	struct usb_hcd *hcd;
	struct phy *phy, *cdp_phy;
	unsigned int i, j;
	int ret;
	int partition_id_xusba, partition_id_xusbc;

	BUILD_BUG_ON(sizeof(struct tegra_xhci_fw_cfgtbl) != 256);

#if IS_ENABLED(CONFIG_PM_GENERIC_DOMAINS)
	partition_id_xusbc = tegra_pd_get_powergate_id(tegra_xusbc_pd);
#else
	partition_id_xusbc = TEGRA_POWERGATE_XUSBC;
#endif
	if (partition_id_xusbc < 0)
		return -EINVAL;

#if IS_ENABLED(CONFIG_PM_GENERIC_DOMAINS)
	partition_id_xusba = tegra_pd_get_powergate_id(tegra_xusba_pd);
#else
	partition_id_xusba = TEGRA_POWERGATE_XUSBA;
#endif
	if (partition_id_xusba < 0)
		return -EINVAL;

	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	ret = dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_warn(&pdev->dev, "no suitable DMA available\n");
		return ret;
	}
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_warn(&pdev->dev, "no suitable DMA available\n");
		return ret;
	}

	tegra = devm_kzalloc(&pdev->dev, sizeof(*tegra), GFP_KERNEL);
	if (!tegra)
		return -ENOMEM;

	tegra->pgid_host = partition_id_xusbc;
	tegra->pgid_ss = partition_id_xusba;
	tegra->dev = &pdev->dev;
	tegra->lp0_exit = false;
	mutex_init(&tegra->lock);
	platform_set_drvdata(pdev, tegra);

	match = of_match_device(tegra_xhci_of_match, &pdev->dev);
	if (!match) {
		dev_warn(&pdev->dev, "of doesn't match\n");
		return -ENODEV;
	}
	tegra->soc_config = match->data;

	tegra_xhci_parse_dt(pdev, tegra);

	if (tegra->boost_cpu_freq > 0) {
		dev_dbg(&pdev->dev, "PMQOS CPU freq boost enabled\n");
		tegra->cpu_boost_enabled = true;
	}

	/* If cpu boost is enabled, XHCI_BOOST_TRIGGER_SIZE is the
	 * minimum buffer length beyond which we boost the frequency.
	 */
	if (tegra->cpu_boost_enabled &&
			tegra->boost_cpu_trigger < XHCI_BOOST_TRIGGER_SIZE)
		tegra->boost_cpu_trigger = XHCI_BOOST_TRIGGER_SIZE;

	hcd = usb_create_hcd(&tegra_xhci_hc_driver, &pdev->dev,
				    dev_name(&pdev->dev));
	if (!hcd)
		return -ENOMEM;
	tegra->hcd = hcd;

	/*
	 * USB 2.0 roothub is stored in drvdata now. Swap it with the Tegra HCD.
	 */
	dev_set_drvdata(&pdev->dev, tegra);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "failed to get host mmio resources\n");
		ret = -ENXIO;
		goto put_hcd;
	}
	hcd->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(hcd->regs)) {
		ret = PTR_ERR(hcd->regs);
		dev_warn(&pdev->dev, "can't map host mmio (%d)\n", ret);
		goto put_hcd;
	}
	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!res) {
		dev_err(&pdev->dev, "failed to get fpci mmio resources\n");
		ret = -ENXIO;
		goto put_hcd;
	}
	tegra->fpci_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(tegra->fpci_base)) {
		ret = PTR_ERR(tegra->fpci_base);
		dev_warn(&pdev->dev, "can't map fpci mmio (%d)\n", ret);
		goto put_hcd;
	}

	if (XHCI_IS_T210(tegra)) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
		if (!res) {
			dev_err(&pdev->dev,
				"failed to get ipfs mmio resources\n");
			ret = -ENXIO;
			goto put_hcd;
		}
		tegra->ipfs_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(tegra->ipfs_base)) {
			ret = PTR_ERR(tegra->ipfs_base);
			dev_warn(&pdev->dev, "can't map ipfs mmio (%d)\n", ret);
			goto put_hcd;
		}
	}
	tegra->irq = platform_get_irq(pdev, 0);
	if (tegra->irq < 0) {
		ret = tegra->irq;
		dev_warn(&pdev->dev, "can't get irq (%d)\n", ret);
		goto put_hcd;
	}

	tegra->padctl_irq = platform_get_irq(pdev, 1);
	if (tegra->padctl_irq < 0) {
		ret = tegra->padctl_irq;
		dev_warn(&pdev->dev, "can't get padctl_irq (%d)\n", ret);
		goto put_hcd;
	}
	ret = devm_request_threaded_irq(&pdev->dev, tegra->padctl_irq, NULL,
				tegra_xhci_padctl_irq,
				IRQF_ONESHOT | IRQF_TRIGGER_HIGH | IRQF_SHARED,
				dev_name(&pdev->dev), tegra);
	if (ret < 0)
		goto put_hcd;

	if (XHCI_IS_T210(tegra)) {
		/* get clks */
		tegra->host_clk = devm_clk_get(&pdev->dev, "host");
		if (IS_ERR(tegra->host_clk))
			return PTR_ERR(tegra->host_clk);

		tegra->falc_clk = devm_clk_get(&pdev->dev, "falcon_src");
		if (IS_ERR(tegra->falc_clk))
			return PTR_ERR(tegra->falc_clk);

		tegra->ss_clk = devm_clk_get(&pdev->dev, "ss");
		if (IS_ERR(tegra->ss_clk))
			return PTR_ERR(tegra->ss_clk);

		tegra->ss_src_clk = devm_clk_get(&pdev->dev, "ss_src");
		if (IS_ERR(tegra->ss_src_clk))
			return PTR_ERR(tegra->ss_src_clk);

		tegra->hs_src_clk = devm_clk_get(&pdev->dev, "hs_src");
		if (IS_ERR(tegra->hs_src_clk))
			return PTR_ERR(tegra->hs_src_clk);

		tegra->fs_src_clk = devm_clk_get(&pdev->dev, "fs_src");
		if (IS_ERR(tegra->fs_src_clk))
			return PTR_ERR(tegra->fs_src_clk);

		tegra->pll_u_480m = devm_clk_get(&pdev->dev, "pll_u_480M");
		if (IS_ERR(tegra->pll_u_480m))
			return PTR_ERR(tegra->pll_u_480m);

		tegra->clk_m = devm_clk_get(&pdev->dev, "clk_m");
		if (IS_ERR(tegra->clk_m))
			return PTR_ERR(tegra->clk_m);
	}

	ret = tegra_xhci_clk_enable(tegra);
	if (ret)
		goto put_hcd;

	if (tegra_platform_is_fpga()) {
		fpga_hacks_init(pdev, tegra);
		fpga_hacks_partition_reset(pdev, tegra, false);
		goto skip_clocks;
	}

	tegra->bwmgr_handle = tegra_bwmgr_register(TEGRA_BWMGR_CLIENT_XHCI);
	if (IS_ERR_OR_NULL(tegra->bwmgr_handle)) {
		ret = IS_ERR(tegra->bwmgr_handle) ?
				PTR_ERR(tegra->bwmgr_handle) : -ENODEV;
		dev_warn(&pdev->dev, "can't register EMC bwmgr (%d)\n", ret);
		goto disable_xhci_clk;
	}

skip_clocks:
	tegra->num_supplies = tegra->soc_config->num_supplies;
	tegra->supplies = devm_kzalloc(&pdev->dev,
		tegra->num_supplies * sizeof(*tegra->supplies), GFP_KERNEL);
	if (!tegra->supplies)
		goto disable_xhci_clk;

	for (i = 0; i < tegra->num_supplies; i++)
		tegra->supplies[i].supply = tegra->soc_config->supply_names[i];
	ret = devm_regulator_bulk_get(&pdev->dev,
				      tegra->num_supplies,
				      tegra->supplies);
	if (ret)
		goto disable_xhci_clk;
	ret = regulator_bulk_enable(tegra->num_supplies,
				    tegra->supplies);
	if (ret)
		goto disable_xhci_clk;

	INIT_WORK(&tegra->mbox_req_work, tegra_xhci_mbox_work);
	tegra->mbox_client.dev = &pdev->dev;
	tegra->mbox_client.tx_block = true;
	tegra->mbox_client.tx_tout = 0;
	tegra->mbox_client.rx_callback = tegra_xhci_mbox_rx;
	tegra->mbox_chan = mbox_request_channel(&tegra->mbox_client, 0);
	if (IS_ERR(tegra->mbox_chan)) {
		ret = PTR_ERR(tegra->mbox_chan);
		if (ret != -EPROBE_DEFER)
			dev_warn(&pdev->dev,
				"can't get mailbox channel (%d)\n", ret);
		goto disable_regulator;
	}

	/* FW mailbox ACK wait queue initialization */
	mutex_init(&tegra->mbox_lock_ack);
	init_waitqueue_head(&tegra->fw_ack_wq);

	for (i = 0; i < ARRAY_SIZE(tegra->phys); i++) {
		tegra->phys[i] = devm_kcalloc(&pdev->dev,
					      tegra->soc_config->num_phys[i],
					      sizeof(*tegra->phys[i]),
					      GFP_KERNEL);
		if (!tegra->phys[i]) {
			ret = -ENOMEM;
			goto put_mbox;
		}

		/* allocate optional cdp_phys */
		if (i == UTMI_PHY) {
			tegra->cdp_phys = devm_kcalloc(&pdev->dev,
					tegra->soc_config->num_phys[i],
					sizeof(struct phy *), GFP_KERNEL);
			if (!tegra->cdp_phys) {
				ret = -ENOMEM;
				goto put_mbox;
			}
		}

		for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
			char prop[8], cdp_prop[8];

			snprintf(prop, sizeof(prop), "%s-%d",
				 tegra_phy_names[i], j);
			phy = devm_phy_optional_get(&pdev->dev, prop);
			if (IS_ERR(phy)) {
				ret = PTR_ERR(phy);
				dev_warn(&pdev->dev, "can't get %s phy (%d)\n",
					prop, ret);
				goto put_mbox;
			} else {
				if (phy && strstr(prop, "usb3")) {
					tegra->enabled_ss_ports |= BIT(j);

					if (tegra->soc_config->has_otg_cap &&
					tegra->soc_config->has_otg_cap(phy)) {
						tegra->usb3_otg_port_base_1 =
									  j + 1;
					}
				}

				if (phy && strstr(prop, "utmi")) {
					if (tegra->soc_config->has_otg_cap &&
					tegra->soc_config->has_otg_cap(phy)) {
						tegra->utmi_otg_port_base_1 =
									  j + 1;
					}
					/* get optional CDP phy */
					snprintf(cdp_prop, sizeof(cdp_prop),
						 "cdp-%d", j);
					cdp_phy = devm_phy_optional_get(
						  &pdev->dev, cdp_prop);
					if (!IS_ERR_OR_NULL(cdp_phy)) {
						dev_info(&pdev->dev,
							"UTMI CDP phy %d registered\n",
							j);
						tegra->cdp_phys[j] = cdp_phy;
						tegra->cdp_enabled = true;
					}
				}

				if (phy && strstr(prop, "hsic"))
					tegra->num_hsic_port++;
			}

			tegra->phys[i][j] = phy;
		}
	}

	tegra->connected_utmi_ports = devm_kcalloc(&pdev->dev,
					tegra->soc_config->num_phys[UTMI_PHY],
					sizeof(u8), GFP_KERNEL);

	if (tegra->utmi_otg_port_base_1) {
		dev_info(&pdev->dev, "UTMI port %d has OTG_CAP\n",
			tegra->utmi_otg_port_base_1 - 1);
		hcd->self.otg_port = tegra->utmi_otg_port_base_1;
	} else
		dev_info(&pdev->dev, "No UTMI port has OTG_CAP\n");

	if (tegra->usb3_otg_port_base_1) {
		dev_info(&pdev->dev, "USB3 port %d has OTG_CAP\n",
			tegra->usb3_otg_port_base_1 - 1);
	} else
		dev_info(&pdev->dev, "No USB3 port has OTG_CAP\n");

	if (tegra_platform_is_silicon()) {
		ret = tegra_unpowergate_partition_with_clk_on(tegra->pgid_ss);
		if (ret) {
			dev_warn(&pdev->dev, "can't unpowergate SS partition\n");
			goto put_mbox;
		}

		ret = tegra_unpowergate_partition_with_clk_on(tegra->pgid_host);
		if (ret) {
			dev_warn(&pdev->dev, "can't unpowergate Host partition\n");
			goto powergate_ss;
		}
	}

	tegra_xhci_cfg(tegra);

	ret = tegra_xhci_phy_enable(tegra);
	if (ret < 0) {
		dev_warn(&pdev->dev, "can't enable phy (%d)\n", ret);
		goto powergate_host;
	}

	tegra_xhci_debugfs_init(tegra);

	INIT_DELAYED_WORK(&tegra->firmware_retry_work,
					tegra_firmware_retry_work);

	ret = request_firmware_nowait(THIS_MODULE, true,
				      tegra->soc_config->firmware_file,
				      tegra->dev, GFP_KERNEL, tegra,
				      tegra_xhci_probe_finish);
	if (ret < 0) {
		dev_warn(&pdev->dev, "can't request firmware(%d)\n", ret);
		goto disable_phy;
	}

	if (XHCI_IS_T186(tegra)) {
		INIT_WORK(&tegra->oc_work, tegra_xhci_oc_work);
		INIT_DELAYED_WORK(&tegra->otg_retry_work,
				  tegra_xhci_otg_retry_work);
	} else {
		INIT_WORK(&tegra->id_extcon_work, tegra_xhci_id_extcon_work);
		tegra->id_extcon = extcon_get_extcon_dev_by_cable(&pdev->dev,
								  "id");
		if (!IS_ERR(tegra->id_extcon)) {
			tegra->id_extcon_nb.notifier_call =
				tegra_xhci_id_notifier;
			extcon_register_notifier(tegra->id_extcon,
				EXTCON_USB_HOST, &tegra->id_extcon_nb);
		} else if (PTR_ERR(tegra->id_extcon) == -EPROBE_DEFER) {
			ret = -EPROBE_DEFER;
			goto disable_phy;
		} else
			dev_info(&pdev->dev, "no USB ID extcon found\n");
	}

	/* Init pm qos for cpu boost */
	if (tegra->cpu_boost_enabled)
		tegra_xusb_boost_cpu_init(tegra);

	device_init_wakeup(tegra->dev, true);
	return 0;

disable_phy:
	tegra_xhci_debugfs_deinit(tegra);
	tegra_xhci_phy_disable(tegra);
powergate_host:
	if (tegra_platform_is_silicon())
		tegra_powergate_partition_with_clk_off(tegra->pgid_host);
powergate_ss:
	if (tegra_platform_is_silicon())
		tegra_powergate_partition_with_clk_off(tegra->pgid_ss);
put_mbox:
	mbox_free_channel(tegra->mbox_chan);
disable_regulator:
	if (tegra_platform_is_silicon())
		regulator_bulk_disable(tegra->num_supplies, tegra->supplies);
disable_xhci_clk:
	tegra_xhci_clk_disable(tegra);
put_hcd:
	usb_put_hcd(hcd);
	if (tegra_platform_is_fpga())
		fpga_hacks_partition_reset(pdev, tegra, true);

	if (!IS_ERR_OR_NULL(tegra->bwmgr_handle))
		tegra_bwmgr_unregister(tegra->bwmgr_handle);

	return ret;
}

static int tegra_xhci_remove(struct platform_device *pdev)
{
	struct tegra_xhci_hcd *tegra = platform_get_drvdata(pdev);
	struct usb_hcd *hcd = tegra->hcd;
	struct xhci_hcd *xhci;

	if (XHCI_IS_T186(tegra)) {
		cancel_delayed_work_sync(&tegra->otg_retry_work);
		cancel_work_sync(&tegra->oc_work);
		if (tegra->utmi_otg_port_base_1)
			usb_otg_unregister_hcd(hcd);
	} else {
		cancel_work_sync(&tegra->id_extcon_work);
		if (!IS_ERR(tegra->id_extcon))
			extcon_unregister_notifier(tegra->id_extcon,
					EXTCON_USB_HOST, &tegra->id_extcon_nb);
	}

	cancel_delayed_work_sync(&tegra->firmware_retry_work);
	pm_runtime_get_sync(tegra->dev);

	if (tegra->fw_loaded) {
		xhci = hcd_to_xhci(hcd);
		usb_remove_hcd(xhci->shared_hcd);
		usb_put_hcd(xhci->shared_hcd);
		usb_remove_hcd(hcd);
		usb_put_hcd(hcd);
		kfree(xhci);
	} else if (hcd) {
		/* Unbound after probe(), but before firmware loading. */
		usb_put_hcd(hcd);
	}

	if (tegra->cpu_boost_enabled)
		tegra_xusb_boost_cpu_deinit(tegra);
	cancel_work_sync(&tegra->mbox_req_work);
	mbox_free_channel(tegra->mbox_chan);
	tegra_xhci_phy_disable(tegra);
	regulator_bulk_disable(tegra->num_supplies, tegra->supplies);
	tegra_xhci_clk_disable(tegra);
	tegra_powergate_partition_with_clk_off(tegra->pgid_host);
	tegra_powergate_partition_with_clk_off(tegra->pgid_ss);

	tegra_bwmgr_set_emc(tegra->bwmgr_handle, 0,
		TEGRA_BWMGR_SET_EMC_SHARED_BW);
	tegra_bwmgr_unregister(tegra->bwmgr_handle);

	if (tegra->fw_data)
		dma_free_coherent(tegra->dev, tegra->fw_size, tegra->fw_data,
				  tegra->fw_dma_addr);
	fw_log_deinit(tegra);

	pm_runtime_disable(tegra->dev);
	pm_runtime_put(tegra->dev);

	tegra_xhci_debugfs_deinit(tegra);

	return 0;
}

static inline u32 read_portsc(struct tegra_xhci_hcd *tegra, unsigned int port)
{
	struct xhci_hcd *xhci = hcd_to_xhci(tegra->hcd);

	return readl(&xhci->op_regs->port_status_base + NUM_PORT_REGS * port);
}

static void get_rootport_name(struct tegra_xhci_hcd *tegra, int port_id,
		char *name, int size)
{
	struct xhci_hcd *xhci = hcd_to_xhci(tegra->hcd);
	struct usb_hcd *hcd;
	int i, num;
	__le32 __iomem **ports;
	__le32 __iomem *addr;
	u32 portsc;

	portsc = read_portsc(tegra, port_id);
	if (DEV_SUPERSPEED(portsc)) {
		hcd = xhci->shared_hcd;
		ports = xhci->usb3_ports;
		num = xhci->num_usb3_ports;
	} else {
		hcd = xhci->main_hcd;
		ports = xhci->usb2_ports;
		num = xhci->num_usb2_ports;
	}

	addr = &xhci->op_regs->port_status_base + NUM_PORT_REGS * port_id;
	for (i = 0; i < num; i++) {
		if (ports[i] == addr)
			break;
	}

	memset(name, 0, size);
	snprintf(name, size, "%d-%d", hcd->self.busnum, i + 1);
}

static enum usb_device_speed port_speed(struct tegra_xhci_hcd *tegra,
					unsigned int port)
{
	u32 portsc = read_portsc(tegra, port);

	if (DEV_FULLSPEED(portsc))
		return USB_SPEED_FULL;
	else if (DEV_HIGHSPEED(portsc))
		return USB_SPEED_HIGH;
	else if (DEV_LOWSPEED(portsc))
		return USB_SPEED_LOW;
	else if (DEV_SUPERSPEED(portsc))
		return USB_SPEED_SUPER;
	else
		return USB_SPEED_UNKNOWN;
}

static bool port_connected(struct tegra_xhci_hcd *tegra, unsigned int port)
{
	u32 portsc = read_portsc(tegra, port);

	return !!(portsc & PORT_CONNECT);
}

#if IS_ENABLED(CONFIG_PM_SLEEP) || IS_ENABLED(CONFIG_PM)
static void tegra_xhci_save_context(struct tegra_xhci_hcd *tegra)
{
	if (XHCI_IS_T210(tegra)) {
		/* Save IPFS registers */
		tegra->ipfs_ctx.msi_bar_sz =
				ipfs_readl(tegra, XUSB_HOST_MSI_BAR_SZ_0);
		tegra->ipfs_ctx.msi_axi_barst =
				ipfs_readl(tegra, XUSB_HOST_MSI_AXI_BAR_ST_0);
		tegra->ipfs_ctx.msi_fpci_barst =
				ipfs_readl(tegra, XUSB_HOST_MSI_FPCI_BAR_ST_0);
		tegra->ipfs_ctx.msi_vec0 =
				ipfs_readl(tegra, XUSB_HOST_MSI_VEC0_0);
		tegra->ipfs_ctx.msi_en_vec0 =
				ipfs_readl(tegra, XUSB_HOST_MSI_EN_VEC0_0);
		tegra->ipfs_ctx.fpci_error_masks =
				ipfs_readl(tegra, XUSB_HOST_FPCI_ERROR_MASKS_0);
		tegra->ipfs_ctx.intr_mask =
				ipfs_readl(tegra, XUSB_HOST_INTR_MASK_0);
		tegra->ipfs_ctx.ipfs_intr_enable =
				ipfs_readl(tegra, XUSB_HOST_IPFS_INTR_ENABLE_0);
		tegra->ipfs_ctx.ufpci_config =
				ipfs_readl(tegra, XUSB_HOST_UFPCI_CONFIG_0);
		tegra->ipfs_ctx.clkgate_hysteresis =
				ipfs_readl(tegra,
					XUSB_HOST_CLKGATE_HYSTERESIS_0);
		tegra->ipfs_ctx.xusb_host_mccif_fifo_cntrl =
				ipfs_readl(tegra, XUSB_HOST_MCCIF_FIFOCTRL_0);
	}
	/* Save FPCI registers */
	tegra->fpci_ctx.hs_pls =
		fpci_readl(tegra, XUSB_CFG_ARU_CONTEXT_HS_PLS);
	tegra->fpci_ctx.fs_pls =
		fpci_readl(tegra, XUSB_CFG_ARU_CONTEXT_FS_PLS);
	tegra->fpci_ctx.hsfs_speed =
		fpci_readl(tegra, XUSB_CFG_ARU_CONTEXT_HSFS_SPEED);
	tegra->fpci_ctx.hsfs_pp =
		fpci_readl(tegra, XUSB_CFG_ARU_CONTEXT_HSFS_PP);
	tegra->fpci_ctx.cfg_aru = fpci_readl(tegra, XUSB_CFG_ARU_CONTEXT);
	tegra->fpci_ctx.cfg_order = fpci_readl(tegra, XUSB_CFG_AXI_CFG);
	tegra->fpci_ctx.cfg_fladj = fpci_readl(tegra, XUSB_CFG_24);
	tegra->fpci_ctx.cfg_sid = fpci_readl(tegra, XUSB_CFG_16);
}

static bool is_host_mode_phy(struct tegra_xhci_hcd *tegra,
			     enum tegra_xhci_phy_type type, int index)
{
	if (!tegra->phys[type][index])
		return false;

	if (type == HSIC_PHY)
		return true;

	if (tegra->host_mode)
		return true;

	if ((type == UTMI_PHY) && (index != (tegra->utmi_otg_port_base_1 - 1)))
		return true;

	if ((type == USB3_PHY) && (index != (tegra->usb3_otg_port_base_1 - 1)))
		return true;

	return false;
}

static void tegra_xhci_restore_context(struct tegra_xhci_hcd *tegra)
{
	/* Restore FPCI registers */
	fpci_writel(tegra, tegra->fpci_ctx.hs_pls, XUSB_CFG_ARU_CONTEXT_HS_PLS);
	fpci_writel(tegra, tegra->fpci_ctx.fs_pls, XUSB_CFG_ARU_CONTEXT_FS_PLS);
	fpci_writel(tegra, tegra->fpci_ctx.hsfs_speed,
		    XUSB_CFG_ARU_CONTEXT_HSFS_SPEED);
	fpci_writel(tegra, tegra->fpci_ctx.hsfs_pp,
		    XUSB_CFG_ARU_CONTEXT_HSFS_PP);
	fpci_writel(tegra, tegra->fpci_ctx.cfg_aru, XUSB_CFG_ARU_CONTEXT);
	fpci_writel(tegra, tegra->fpci_ctx.cfg_order, XUSB_CFG_AXI_CFG);
	fpci_writel(tegra, tegra->fpci_ctx.cfg_fladj, XUSB_CFG_24);
	fpci_writel(tegra, tegra->fpci_ctx.cfg_sid, XUSB_CFG_16);

	if (XHCI_IS_T210(tegra)) {
		/* Restore IPFS registers */
		ipfs_writel(tegra, tegra->ipfs_ctx.msi_bar_sz,
				XUSB_HOST_MSI_BAR_SZ_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.msi_axi_barst,
				XUSB_HOST_MSI_AXI_BAR_ST_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.msi_fpci_barst,
				XUSB_HOST_MSI_FPCI_BAR_ST_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.msi_vec0,
				XUSB_HOST_MSI_VEC0_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.msi_en_vec0,
				XUSB_HOST_MSI_EN_VEC0_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.fpci_error_masks,
				XUSB_HOST_FPCI_ERROR_MASKS_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.intr_mask,
				XUSB_HOST_INTR_MASK_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.ipfs_intr_enable,
				XUSB_HOST_IPFS_INTR_ENABLE_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.ufpci_config,
				XUSB_HOST_UFPCI_CONFIG_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.clkgate_hysteresis,
				XUSB_HOST_CLKGATE_HYSTERESIS_0);
		ipfs_writel(tegra, tegra->ipfs_ctx.xusb_host_mccif_fifo_cntrl,
				XUSB_HOST_MCCIF_FIFOCTRL_0);
	}
}

static void tegra_xhci_program_utmi_power_lp0_exit(
	struct tegra_xhci_hcd *tegra)
{
	u8 hs_pls;
	int i;

	for (i = 0; i < tegra->soc_config->num_phys[UTMI_PHY]; i++) {
		if (i == (tegra->utmi_otg_port_base_1 - 1) && !tegra->host_mode)
			continue;

		hs_pls = FPCI_CTX_HS_PLS(tegra->fpci_ctx.hs_pls, i);

		if (hs_pls == ARU_CONTEXT_HS_PLS_SUSPEND ||
			hs_pls == ARU_CONTEXT_HS_PLS_FS_MODE) {
			if (tegra->soc_config->utmi_pad_power_on)
				tegra->soc_config->utmi_pad_power_on(
					tegra->phys[UTMI_PHY][i]);
		} else {
			if (tegra->soc_config->utmi_pad_power_off)
				tegra->soc_config->utmi_pad_power_off(
					tegra->phys[UTMI_PHY][i]);
		}
	}
}

static int tegra_xhci_wait_for_ports_enter_u3(struct tegra_xhci_hcd *tegra)
{
	struct xhci_hcd *xhci = hcd_to_xhci(tegra->hcd);
	struct device *dev = tegra->dev;
	u32 usbcmd;
	int i;
	int num_ports = HCS_MAX_PORTS(xhci->hcs_params1);
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&xhci->lock, flags);

	usbcmd = readl(&xhci->op_regs->command);
	usbcmd &= ~CMD_EIE;
	writel(usbcmd, &xhci->op_regs->command);

	for (i = 0; i < num_ports; i++) {
		bool is_busy = true;
		char devname[16];
		u32 portsc = read_portsc(tegra, i);

		if (!(portsc & PORT_PE))
			continue;

		if ((portsc & PORT_PLS_MASK) == XDEV_U3)
			continue;

		get_rootport_name(tegra, i, devname, sizeof(devname));

		if (XHCI_IS_T210(tegra) && DEV_SUPERSPEED(portsc)) {
			unsigned long end = jiffies + msecs_to_jiffies(200);
			while (time_before(jiffies, end)) {
				if ((portsc & PORT_PLS_MASK) == XDEV_RESUME)
					break;
				spin_unlock_irqrestore(&xhci->lock, flags);
				msleep(20);
				spin_lock_irqsave(&xhci->lock, flags);

				portsc = read_portsc(tegra, i);
				if ((portsc & PORT_PLS_MASK) == XDEV_U3) {
					dev_info(dev, "%s is suspended\n",
						devname);
					is_busy = false;
					break;
				}
			}
		}

		if (is_busy) {
			dev_info(dev, "%s is not suspended: %08x\n", devname,
				portsc);
			ret = -EBUSY;
			break;
		}
	}

	spin_unlock_irqrestore(&xhci->lock, flags);

	return ret;
}

/* caller must hold tegra->lock */
static int tegra_xhci_powergate(struct tegra_xhci_hcd *tegra, bool runtime)
{
	struct xhci_hcd *xhci = hcd_to_xhci(tegra->hcd);
	struct device *dev = tegra->dev;
	unsigned int i, j;
	int ret;

	dev_info(dev, "entering ELPG\n");

	ret = tegra_xhci_wait_for_ports_enter_u3(tegra);
	if (ret < 0)
		goto out;

	ret = xhci_suspend(xhci, runtime ? true :
			   device_may_wakeup(tegra->dev));
	if (ret) {
		dev_warn(dev, "xhci_suspend() failed %d\n", ret);
		goto out;
	}

	/* Save host context. */
	tegra_xhci_save_context(tegra);

	for (i = 0; i < MAX_PHY_TYPES; i++) {
		for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
			struct phy *phy = tegra->phys[i][j];
			enum usb_device_speed speed;
			int offset;

			if (!phy)
				continue;

			if (!is_host_mode_phy(tegra, i, j))
				continue;

			if (i == HSIC_PHY) {
				offset = tegra->soc_config->hsic_port_offset;
				if (!port_connected(tegra, offset + j))
					continue;
			}

			if (i == UTMI_PHY) {
				offset = tegra->soc_config->utmi_port_offset;
				speed = port_speed(tegra, offset + j);
				if (tegra->soc_config->utmi_pad_power_off)
					tegra->soc_config->utmi_pad_power_off(phy);
			}

			if (i == HSIC_PHY)
				speed = USB_SPEED_HIGH;

			if (i == USB3_PHY)
				speed = USB_SPEED_SUPER;

			/*
			 * Should not enable UTMI sleepwalk if VBUS will be OFF,
			 * here assuming remote wakeup disalbing also implies
			 * VBUS turns OFF in system suspend.
			 */
			if (i == UTMI_PHY && !runtime
				&& tegra->pmc_usb_wakes_disabled)
				continue;

			if (tegra->soc_config->enable_sleepwalk &&
				tegra->soc_config->enable_sleepwalk(phy, speed))
				dev_info(dev, "failed to enable sleepwalk for %s phy %d\n"
					, tegra_phy_names[i], j);

			if (tegra->soc_config->enable_wake &&
				tegra->soc_config->enable_wake(phy)) {
				dev_info(dev, "failed to enable wake for %s phy %d\n"
					, tegra_phy_names[i], j);
			}
		}
	}

	/* Power off XUSB_HOST partition. */
	ret = tegra_powergate_partition_with_clk_off(tegra->pgid_host);
	if (ret) {
		dev_warn(dev, "failed to powergate Host partition %d\n", ret);
		goto out;
	}

	/* Power off XUSB_SS partition. */
	ret = tegra_powergate_partition_with_clk_off(tegra->pgid_ss);
	if (ret) {
		dev_warn(dev, "failed to powergate SS partition %d\n", ret);
		goto out;
	}

	for (i = 0; i < MAX_PHY_TYPES; i++) {
		for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
			struct phy *phy = tegra->phys[i][j];

			phy_power_off(phy);
		}
	}

	/* In ELPG, firmware log context is gone. Rewind shared log buffer. */
	if (test_bit(FW_LOG_CONTEXT_VALID, &tegra->log.flags)) {
		if (!circ_buffer_full(&tegra->log.circ)) {
			if (fw_log_wait_empty_timeout(tegra, 500))
				dev_info(dev, "%s still has logs\n", __func__);
		}

		tegra->log.dequeue = tegra->log.virt_addr;
		tegra->log.seq = 0;
	}

	/* Clear EMC bandwidth request */
	tegra_bwmgr_set_emc(tegra->bwmgr_handle, 0,
		TEGRA_BWMGR_SET_EMC_SHARED_BW);

	tegra_xhci_clk_disable(tegra);

out:
	if (!ret)
		dev_info(tegra->dev, "entering ELPG done\n");
	else {
		u32 usbcmd;

		usbcmd = readl(&xhci->op_regs->command);
		usbcmd |= CMD_EIE;
		writel(usbcmd, &xhci->op_regs->command);

		dev_info(tegra->dev, "entering ELPG failed\n");
	}

	return ret;
}

/* caller must hold tegra->lock */
static int tegra_xhci_unpowergate(struct tegra_xhci_hcd *tegra)
{
	struct xhci_hcd *xhci = hcd_to_xhci(tegra->hcd);
	struct tegra_xusb_mbox_msg msg;
	struct device *dev = tegra->dev;
	unsigned int i, j;
	int ret;
	bool runtime = true;
	u32 usbcmd;

	dev_info(dev, "exiting ELPG\n");

	ret = tegra_xhci_clk_enable(tegra);
	if (ret) {
		dev_warn(dev, "failed to enable xhci clocks %d\n", ret);
		goto out;
	}

	ret = tegra_unpowergate_partition_with_clk_on(tegra->pgid_host);
	if (ret < 0) {
		dev_warn(dev, "failed to unpowergate Host partition %d\n", ret);
		goto out;
	}

	/* Power on XUSB_SS partition. */
	ret = tegra_unpowergate_partition_with_clk_on(tegra->pgid_ss);
	if (ret < 0) {
		dev_warn(dev, "failed to unpowergate SS partition %d\n", ret);
		goto out;
	}

	/* Clear XUSB wake events and disable wake interrupts. */
	for (i = 0; i < MAX_PHY_TYPES; i++) {
		for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
			struct phy *phy = tegra->phys[i][j];

			if (tegra->soc_config->remote_wake_detected &&
			(tegra->soc_config->remote_wake_detected(phy) > 0)) {
				dev_dbg(dev, "%s port %d remote wake detected\n"
					, tegra_phy_names[i], j);
				if (i == UTMI_PHY &&
					tegra->soc_config->utmi_pad_power_on)
					tegra->soc_config->utmi_pad_power_on(phy);
			}
			if (tegra->soc_config->disable_wake &&
				tegra->soc_config->disable_wake(phy))
				dev_dbg(dev, "%s port %d disable wake failed\n",
					tegra_phy_names[i], j);

			/* power on SS phy after unpowergating SS partition */
			if (i == USB3_PHY)
				continue;

			ret = phy_power_on(phy);
			if (ret) {
				dev_dbg(dev, "%s port %d power on failed\n",
					tegra_phy_names[i], j);
			}
		}
	}

	if (tegra->lp0_exit) {
		tegra_xhci_program_utmi_power_lp0_exit(tegra);
		tegra->lp0_exit = false;
		runtime = false;
	}

	tegra_xhci_cfg(tegra);

	/* Restore host context. */
	tegra_xhci_restore_context(tegra);

	/* TODO HSIC restore */

	/* Load firmware and restart controller. */
	ret = tegra_xhci_load_firmware(tegra);
	if (ret < 0)
		goto out;

	/* Enable firmware messages. */
	msg.cmd = MBOX_CMD_MSG_ENABLED;
	msg.data = 0;
	ret = mbox_send_message(tegra->mbox_chan, &msg);
	if (ret < 0)
		goto out;

	for (i = 0; i < MAX_PHY_TYPES; i++) {
		for (j = 0; j < tegra->soc_config->num_phys[i]; j++) {
			struct phy *phy = tegra->phys[i][j];

			if (!phy)
				continue;

			if (i == USB3_PHY) {
				ret = phy_power_on(phy);
				if (ret) {
					dev_dbg(dev, "%s port %d power on failed\n",
						tegra_phy_names[i], j);
				}
			}

			if (i == UTMI_PHY && !runtime
				&& tegra->pmc_usb_wakes_disabled)
				continue;

			if (tegra->soc_config->disable_sleepwalk &&
				tegra->soc_config->disable_sleepwalk(phy))
				dev_dbg(dev, "%s port %d disable sleepwalk failed\n",
					tegra_phy_names[i], j);
		}
	}

	ret = xhci_resume(xhci, 0);

	usbcmd = readl(&xhci->op_regs->command);
	usbcmd |= CMD_EIE;
	writel(usbcmd, &xhci->op_regs->command);

	pm_runtime_mark_last_busy(tegra->dev);

out:
	if (!ret)
		dev_info(dev, "exiting ELPG done\n");
	else
		dev_info(dev, "exiting ELPG failed\n");

	return ret;
}
#endif

#ifdef CONFIG_PM_SLEEP
static int tegra_xhci_suspend(struct device *dev)
{
	struct tegra_xhci_hcd *tegra = dev_get_drvdata(dev);
	int ret = 0;
	unsigned int j;

	if (!tegra->fw_loaded)
		return 0;

	if (XHCI_IS_T186(tegra))
		flush_work(&tegra->oc_work);
	else
		flush_work(&tegra->id_extcon_work);

	mutex_lock(&tegra->lock);

	if (pm_runtime_suspended(dev)) {
		ret = tegra_xhci_unpowergate(tegra);
		if (ret < 0)
			goto out;
	}

	ret = tegra_xhci_powergate(tegra, false);
	if (ret < 0)
		goto out;

	/* disable CDP for all ports */
	if (tegra->cdp_enabled) {
		for (j = 0; j < tegra->soc_config->num_phys[UTMI_PHY]; j++) {
			dev_dbg(dev, "%s: power off CDP phy %d\n", __func__, j);
			ret = phy_power_off(tegra->cdp_phys[j]);
			if (ret)
				dev_dbg(dev, "%s: power off CDP phy %d failed\n",
					__func__, j);

			/*
			 * turn off VBUS for CDP enabled case.
			 * skip vbus off for OTG port
			 */
			if ((j != tegra->utmi_otg_port_base_1 - 1) &&
				tegra->soc_config->utmi_vbus_power_off)
				tegra->soc_config->utmi_vbus_power_off(
						tegra->phys[UTMI_PHY][j]);

		}
	}

	pm_runtime_disable(dev);

	if (!tegra->pmc_usb_wakes_disabled && device_may_wakeup(dev)) {
		ret = enable_irq_wake(tegra->padctl_irq);
		if (ret)
			dev_err(tegra->dev, "failed to enable padctl wakes %d\n",
				ret);
	}
out:
	if (!ret)
		tegra->suspended = true;

	mutex_unlock(&tegra->lock);
	return ret;
}

static int tegra_xhci_resume_common(struct device *dev)
{
	struct tegra_xhci_hcd *tegra = dev_get_drvdata(dev);
	int ret;
	unsigned int j;

	mutex_lock(&tegra->lock);
	if (!tegra->suspended) {
		mutex_unlock(&tegra->lock);
		return 0;
	}

	tegra->lp0_exit = true;

	/* enable CDP for all ports */
	if (tegra->cdp_enabled) {
		for (j = 0; j < tegra->soc_config->num_phys[UTMI_PHY]; j++) {
			dev_dbg(dev, "%s: power on CDP phy %d\n", __func__, j);
			ret = phy_power_on(tegra->cdp_phys[j]);
			if (ret)
				dev_dbg(dev, "%s: power on CDP phy %d failed\n",
					__func__, j);

			/* skip vbus on for OTG port */
			if ((j != tegra->utmi_otg_port_base_1 - 1) &&
				tegra->soc_config->utmi_vbus_power_on)
				tegra->soc_config->utmi_vbus_power_on(
						tegra->phys[UTMI_PHY][j]);
		}
	}

	ret = tegra_xhci_unpowergate(tegra);
	if (ret < 0) {
		mutex_unlock(&tegra->lock);
		return ret;
	}

	tegra->suspended = false;
	mutex_unlock(&tegra->lock);

	if (!XHCI_IS_T186(tegra))
		tegra_xhci_update_otg_role(tegra);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	return 0;
}

static int tegra_xhci_resume(struct device *dev)
{

	struct tegra_xhci_hcd *tegra = dev_get_drvdata(dev);
	int ret = 0;

	if (!tegra->fw_loaded)
		return 0;

	ret = tegra_xhci_resume_common(dev);

	if (!tegra->pmc_usb_wakes_disabled && device_may_wakeup(dev))
		disable_irq_wake(tegra->padctl_irq);

	return ret;
}

static int tegra_xhci_resume_noirq(struct device *dev)
{
	struct tegra_xhci_hcd *tegra = dev_get_drvdata(dev);

	if (!tegra->fw_loaded)
		return 0;

	/*
	 * when CDP is enabled, VBUS will be off in SC7 and re-enabled in SC7
	 * resume. We don't have to resume earlier in noirq callback here
	 * because device will disconnect and re-connected in resume. Also,
	 * turning VBUS on in resume_noirq callback is not working if the
	 * gpio is in GPIO expander controlled through I2C bus.
	 */
	if (tegra->cdp_enabled)
		return 0;

	return tegra_xhci_resume_common(dev);
}
#endif

#ifdef CONFIG_PM
static int tegra_xhci_runtime_suspend(struct device *dev)
{
	struct tegra_xhci_hcd *tegra = dev_get_drvdata(dev);
	int rc;

	if (!tegra->fw_loaded)
		return 0;

	mutex_lock(&tegra->lock);
	rc = tegra_xhci_powergate(tegra, true);
	mutex_unlock(&tegra->lock);

	return rc;
}

static int tegra_xhci_runtime_resume(struct device *dev)
{
	struct tegra_xhci_hcd *tegra = dev_get_drvdata(dev);
	int rc;

	if (!tegra->fw_loaded)
		return 0;

	mutex_lock(&tegra->lock);
	rc = tegra_xhci_unpowergate(tegra);
	mutex_unlock(&tegra->lock);

	return rc;
}
#endif
static const struct dev_pm_ops tegra_xhci_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(tegra_xhci_suspend, tegra_xhci_resume)
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, tegra_xhci_resume_noirq)
	SET_RUNTIME_PM_OPS(tegra_xhci_runtime_suspend,
				tegra_xhci_runtime_resume, NULL)
};

static struct platform_driver tegra_xhci_driver = {
	.probe = tegra_xhci_probe,
	.remove = tegra_xhci_remove,
	.driver = {
		.name = "xhci-tegra",
		.pm = &tegra_xhci_pm_ops,
		.of_match_table = of_match_ptr(tegra_xhci_of_match),
	},
};

static irqreturn_t tegra_xhci_irq(struct usb_hcd *hcd)
{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);

	if (test_bit(FW_LOG_CONTEXT_VALID, &tegra->log.flags))
		wake_up_interruptible(&tegra->log.intr_wait);

	return xhci_irq(hcd);
}

static int tegra_xhci_urb_enqueue(struct usb_hcd *hcd, struct urb *urb,
						gfp_t mem_flags)
{
	int xfertype;
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);

	xfertype = usb_endpoint_type(&urb->ep->desc);
	switch (xfertype) {
	case USB_ENDPOINT_XFER_ISOC:
	case USB_ENDPOINT_XFER_BULK:
		if (!tegra->cpu_boost_enabled)
			break;
		if (urb->transfer_buffer_length > tegra->boost_cpu_trigger) {
			/* break, if last boost was done within 1 sec back,
			 * because previous boost lasts for XHCI_BOOST_TIMEOUT
			 * i.e 2 sec and no need to schedule work for every
			 * transaction.
			 */
			if (time_is_after_jiffies(tegra->cpufreq_last_boosted +
				(msecs_to_jiffies(XHCI_BOOST_TIMEOUT/2))))
				break;
			schedule_work(&tegra->boost_cpufreq_work);
		}
		break;
	case USB_ENDPOINT_XFER_INT:
	case USB_ENDPOINT_XFER_CONTROL:
	default:
		/* Do nothing special here */
		break;
	}
	return xhci_urb_enqueue(hcd, urb, mem_flags);
}

static int tegra_xhci_hub_control(struct usb_hcd *hcd, u16 type_req,
		u16 value, u16 index, char *buf, u16 length)

{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);
	int port = (index & 0xff) - 1;
	int ret;

	if (hcd->speed == HCD_USB2) {
		if ((type_req == ClearPortFeature) &&
			(value == USB_PORT_FEAT_SUSPEND) &&
			tegra->soc_config->utmi_pad_power_on)
			tegra->soc_config->utmi_pad_power_on(
				tegra->phys[UTMI_PHY][port]);
	}

	ret = xhci_hub_control(hcd, type_req, value, index, buf, length);

	if ((hcd->speed == HCD_USB2) && (ret == 0)) {
		if ((type_req == SetPortFeature) &&
			(value == USB_PORT_FEAT_SUSPEND))
			/* We dont suspend the PAD while HNP role swap happens
			 * on the OTG port
			 */
			if (!((hcd->self.otg_port == (port + 1)) &&
				(hcd->self.b_hnp_enable ||
				hcd->self.otg_quick_hnp)) &&
				tegra->soc_config->utmi_pad_power_off)
				tegra->soc_config->utmi_pad_power_off(
					tegra->phys[UTMI_PHY][port]);

		if ((type_req == ClearPortFeature) &&
			(value == USB_PORT_FEAT_C_CONNECTION)) {
			struct xhci_hcd *xhci = hcd_to_xhci(hcd);
			u32 portsc = readl(xhci->usb2_ports[port]);

			if ((portsc & PORT_CONNECT)
				&& tegra->soc_config->utmi_pad_power_on)
				tegra->soc_config->utmi_pad_power_on(
						tegra->phys[UTMI_PHY][port]);
			else {
				/* We dont suspend the PAD while HNP
				 * role swap happens on the OTG port
				 */
				if (!((hcd->self.otg_port == (port + 1))
					&& (hcd->self.b_hnp_enable ||
					hcd->self.otg_quick_hnp)) &&
					tegra->soc_config->utmi_pad_power_off)
					tegra->soc_config->utmi_pad_power_off(
						tegra->phys[UTMI_PHY][port]);
			}
		}
	}

	return ret;
}

static int tegra_xhci_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct xhci_hcd	*xhci = hcd_to_xhci(hcd);
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);
	struct xhci_bus_state *bus_state = &xhci->bus_state[hcd_index(hcd)];
	unsigned long flags;
	int port;

	if (bus_state->resuming_ports) {
		__le32 __iomem **port_array;
		int max_ports;
		u32 portsc;
		int i;

		max_ports = xhci_get_ports(hcd, &port_array);
		for (i = 0; i < max_ports; i++) {
			const char *hub;

			if (hcd->speed == HCD_USB3)
				hub = "SS";
			else
				hub = "HS";

			if (!test_bit(i , &bus_state->resuming_ports))
				continue;

			portsc = readl(port_array[i]);

			if ((hcd->speed == HCD_USB2) &&
				(i < tegra->soc_config->num_phys[UTMI_PHY]) &&
				((portsc & PORT_PLS_MASK) == XDEV_RESUME) &&
				tegra->soc_config->utmi_pad_power_on)
				tegra->soc_config->utmi_pad_power_on(
					tegra->phys[UTMI_PHY][i]);

			if (!(portsc & PORT_CONNECT)) {
				bus_state->resume_done[i] = 0;
				clear_bit(i, &bus_state->resuming_ports);
				pr_debug("%s port disconnected, clear %s hub resuming_ports for port %d\n",
					__func__, hub, i);
			}
		}
	}

	if ((hcd->speed != HCD_USB3) ||
			!tegra->soc_config->disable_u0_ts1_detect)
		goto no_rx_control;

	for (port = 0; port < tegra->soc_config->num_phys[USB3_PHY]; port++) {
		u32 portsc = read_portsc(tegra, port);
		struct phy *phy;

		if (portsc == 0xffffffff)
			break;

		spin_lock_irqsave(&xhci->lock, flags);
		phy = tegra->phys[USB3_PHY][port];
		if ((portsc & PORT_PLS_MASK) == XDEV_U0)
			tegra_xhci_disable_receiver_detector(tegra, phy);
		else {
			if ((portsc & PORT_PLS_MASK) == XDEV_RXDETECT)
				tegra_xhci_disable_clamp_en_early(tegra, phy);

			tegra_xhci_enable_receiver_detector(tegra, phy);
		}
		spin_unlock_irqrestore(&xhci->lock, flags);
	}

no_rx_control:
	return xhci_hub_status_data(hcd, buf);
}

static bool tegra_xhci_is_u0_ts1_detect_disabled(struct usb_hcd *hcd)
{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);

	return tegra->soc_config->disable_u0_ts1_detect;
}

static void tegra_xhci_endpoint_soft_retry(struct usb_hcd *hcd,
		struct usb_host_endpoint *ep, bool on)
{
	struct usb_device *udev = (struct usb_device *) ep->hcpriv;
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);
	struct phy *phy;
	int port = -1;
	int delay = 0;
	u32 portsc;

	if (!udev || udev->speed != USB_SPEED_SUPER ||
			!tegra->soc_config->disable_u0_ts1_detect)
		return;

	/* trace back to roothub port */
	while (udev->parent) {
		if (udev->parent == udev->bus->root_hub) {
			port = udev->portnum - 1;
			break;
		}
		udev = udev->parent;
	}

	if (port < 0 || port >= tegra->soc_config->num_phys[USB3_PHY])
		return;

	portsc = read_portsc(tegra, port);
	phy = tegra->phys[USB3_PHY][port];

	if (on) {
		while ((portsc & PORT_PLS_MASK) != XDEV_U0 && delay++ < 6) {
			udelay(50);
			portsc = read_portsc(tegra, port);
		}

		if ((portsc & PORT_PLS_MASK) != XDEV_U0) {
			dev_info(tegra->dev, "%s port %d doesn't reach U0 in 300us, portsc 0x%x\n",
				__func__, port, portsc);
		}
		tegra_xhci_disable_receiver_detector(tegra, phy);
		tegra_xhci_enable_clamp_en_early(tegra, phy);
	} else
		tegra_xhci_disable_clamp_en_early(tegra, phy);
}

static int tegra_xhci_update_device(struct usb_hcd *hcd,
				    struct usb_device *udev)
{
	const struct usb_device_id *id;

	for (id = disable_usb_persist_quirk_list; id->match_flags; id++) {
		if (usb_match_device(udev, id) && usb_match_speed(udev, id)) {
			udev->persist_enabled = 0;
			break;
		}
	}

	return xhci_update_device(hcd, udev);
}

static bool device_has_isoch_ep_and_interval_one(struct usb_device *udev)
{
	struct usb_host_config *config;
	struct usb_host_interface *alt;
	struct usb_endpoint_descriptor *desc;
	int i, j;

	config = udev->actconfig;
	if (!config)
		return false;

	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		alt = config->interface[i]->cur_altsetting;

		if (!alt)
			continue;

		for (j = 0; j < alt->desc.bNumEndpoints; j++) {
			desc = &alt->endpoint[j].desc;
			if (usb_endpoint_xfer_isoc(desc) &&
				desc->bInterval == 1)
				return true;
		}
	}

	return false;
}

static int tegra_xhci_enable_usb3_lpm_timeout(struct usb_hcd *hcd,
			struct usb_device *udev, enum usb3_link_state state)
{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);

	if (tegra->soc_config->disable_u0_ts1_detect)
		return USB3_LPM_DISABLED;

	if (state == USB3_LPM_U1 &&
		device_has_isoch_ep_and_interval_one(udev))
		return USB3_LPM_DISABLED;

	return xhci_enable_usb3_lpm_timeout(hcd, udev, state);
}

static const struct xhci_driver_overrides tegra_xhci_overrides __initconst = {
	.extra_priv_size = sizeof(struct xhci_hcd),
	.reset = tegra_xhci_setup,
};

static int tegra_xhci_alloc_dev(struct usb_hcd *hcd, struct usb_device *udev)
{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);
	int ret;
	int i;

	/* disable CDP for newly connected USB2.0 root hub port */
	if (tegra->cdp_enabled && usb_hcd_is_primary_hcd(hcd)) {
		for (i = 0; i < tegra->soc_config->num_phys[UTMI_PHY]; i++) {
			int port_array_offset =
				tegra->soc_config->utmi_port_offset;

			/* find newly connected ports only */
			if (!tegra->connected_utmi_ports[i] &&
			    port_connected(tegra, port_array_offset + i)) {
				dev_dbg(tegra->dev,
					"%s: power off CDP phy %d\n",
					__func__, i);
				ret = phy_power_off(tegra->cdp_phys[i]);
				if (ret)
					dev_dbg(tegra->dev,
						"%s: power off CDP phy %d failed\n",
						__func__, i);
				tegra->connected_utmi_ports[i] = 1;

				/*
				 * set pad protection circuit to >= 2A
				 * CDP current range: 1.5A~5A see [BC1.2] p36
				 */
				if (tegra->soc_config->set_protection_level)
					tegra->soc_config->set_protection_level(
						tegra->phys[UTMI_PHY][i], 3,
						TEGRA_VBUS_SOURCE);
			}
		}
	}

	return xhci_alloc_dev(hcd, udev);
}

static void tegra_xhci_free_dev(struct usb_hcd *hcd, struct usb_device *udev)
{
	struct tegra_xhci_hcd *tegra = hcd_to_tegra_xhci(hcd);
	struct device *dev = tegra->dev;
	u8 port = udev->portnum - 1;
	int ret;
	bool is_roothub_port = udev->parent == udev->bus->root_hub;

	xhci_free_dev(hcd, udev);

	/* make sure CDP is enabled for for USB2.0 root hub ports */
	if (is_roothub_port && tegra->connected_utmi_ports[port] &&
			tegra->cdp_enabled && usb_hcd_is_primary_hcd(hcd)) {
		/* we have to make sure CDP is turned on before VBUS on */
		dev_dbg(dev, "%s: power on CDP phy %d\n", __func__, port);
		ret = phy_power_on(tegra->cdp_phys[port]);
		if (ret)
			dev_dbg(dev, "%s: power on CDP phy %d failed\n",
				__func__, port);
		tegra->connected_utmi_ports[port] = 0;

		if (tegra->soc_config->set_protection_level)
			/* disable pad protection circuit on this UTMI pad */
			tegra->soc_config->set_protection_level(
				tegra->phys[UTMI_PHY][port], -1,
				TEGRA_VBUS_DEFAULT);
	}
}

static int __init tegra_xhci_init(void)
{
	xhci_init_driver(&tegra_xhci_hc_driver, &tegra_xhci_overrides);
	tegra_xhci_hc_driver.irq = tegra_xhci_irq;
	tegra_xhci_hc_driver.hub_control = tegra_xhci_hub_control;
	tegra_xhci_hc_driver.hub_status_data = tegra_xhci_hub_status_data;
	tegra_xhci_hc_driver.update_device = tegra_xhci_update_device;
	tegra_xhci_hc_driver.enable_usb3_lpm_timeout =
			tegra_xhci_enable_usb3_lpm_timeout;
	tegra_xhci_hc_driver.alloc_dev = tegra_xhci_alloc_dev;
	tegra_xhci_hc_driver.free_dev = tegra_xhci_free_dev;
	tegra_xhci_hc_driver.urb_enqueue = tegra_xhci_urb_enqueue;
	tegra_xhci_hc_driver.endpoint_soft_retry =
			tegra_xhci_endpoint_soft_retry;
	tegra_xhci_hc_driver.is_u0_ts1_detect_disabled =
			tegra_xhci_is_u0_ts1_detect_disabled;
	return platform_driver_register(&tegra_xhci_driver);
}
fs_initcall(tegra_xhci_init);

static void __exit tegra_xhci_exit(void)
{
	platform_driver_unregister(&tegra_xhci_driver);
}
module_exit(tegra_xhci_exit);

MODULE_AUTHOR("Andrew Bresticker <abrestic@chromium.org>");
MODULE_AUTHOR("JC Kuo <jckuo@nvidia.com>");
MODULE_DESCRIPTION("NVIDIA Tegra xHCI host-controller driver");
MODULE_LICENSE("GPL v2");
