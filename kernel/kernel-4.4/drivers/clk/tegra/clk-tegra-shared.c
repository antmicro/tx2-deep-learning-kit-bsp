/*
 * Copyright (c) 2012-2017, NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <soc/tegra/tegra_emc.h>

#include "clk.h"
#include "clk-id.h"

struct tegra_shared_clk {
	char *name;
	char *client;
	union {
		const char **parents;
		const char *parent;
	} p;
	int num_parents;
	enum shared_bus_users_mode mode;
	int flags;
	unsigned long usages;
	int clk_id;
};

#define SHARED_CLK(_name, _parent, _mode, _flags, _usages, _client, _id)\
	{\
		.name = _name,\
		.p.parent = _parent,\
		.num_parents = 1,\
		.mode = _mode, \
		.flags = _flags, \
		.usages = _usages, \
		.client = _client,\
		.clk_id = _id,\
	}

#define SHARED_LIMIT(_name, _parent, _mode, _flags, _usages, _client, _id)\
	{\
		.name = _name,\
		.p.parent = _parent,\
		.num_parents = 1,\
		.mode = _mode, \
		.flags = _flags | TEGRA_SHARED_BUS_RATE_LIMIT, \
		.usages = _usages, \
		.client = _client,\
		.clk_id = _id,\
	}

static struct tegra_shared_clk shared_clks[] = {
	SHARED_LIMIT("cap.c2bus", "c2bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_c2bus),
	SHARED_LIMIT("cap.throttle.c2bus", "c2bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_throttle_c2bus),
	SHARED_LIMIT("floor.c2bus", "c2bus", 0, 0, 0, NULL, tegra_clk_floor_c2bus),
	SHARED_CLK("override.c2bus", "c2bus", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_c2bus),
	SHARED_LIMIT("edp.c2bus", "c2bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_edp_c2bus),
	SHARED_LIMIT("battery.c2bus", "c2bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_battery_c2bus),
	SHARED_LIMIT("cap.profile.c2bus", "c2bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_profile_c2bus),
	SHARED_LIMIT("cap.c3bus", "c3bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_c3bus),
	SHARED_LIMIT("cap.throttle.c3bus", "c3bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_throttle_c3bus),
	SHARED_CLK("override.c3bus", "c3bus", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_c3bus),
	SHARED_LIMIT("floor.c3bus", "c3bus", 0, 0, 0, NULL, tegra_clk_floor_c3bus),
	SHARED_LIMIT("cap.sclk", "sbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_sclk),
	SHARED_LIMIT("cap.throttle.sclk", "sbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_throttle_sclk),
	SHARED_LIMIT("floor.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_floor_sclk),
	SHARED_CLK("override.sclk", "sbus", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_sclk),
	SHARED_CLK("avp.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_avp_sclk),
	SHARED_CLK("bsea.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_bsea_sclk),
	SHARED_CLK("usbd.sclk", "ahb.sclk", 0, 0, 0, NULL, tegra_clk_usbd_sclk),
	SHARED_CLK("usb1.sclk", "ahb.sclk", 0, 0, 0, NULL, tegra_clk_usb1_sclk),
	SHARED_CLK("usb2.sclk", "ahb.sclk", 0, 0, 0, NULL, tegra_clk_usb2_sclk),
	SHARED_CLK("usb3.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_usb3_sclk),
	SHARED_CLK("wake.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_wake_sclk),
	SHARED_CLK("sbc1.sclk", "apb.sclk", 0, 0, 0, NULL, tegra_clk_sbc1_sclk),
	SHARED_CLK("sbc2.sclk", "apb.sclk", 0, 0, 0, NULL, tegra_clk_sbc2_sclk),
	SHARED_CLK("sbc3.sclk", "apb.sclk", 0, 0, 0, NULL, tegra_clk_sbc3_sclk),
	SHARED_CLK("sbc4.sclk", "apb.sclk", 0, 0, 0, NULL, tegra_clk_sbc4_sclk),
	SHARED_CLK("sbc5.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_sbc5_sclk),
	SHARED_CLK("sbc6.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_sbc6_sclk),
	SHARED_CLK("mon.avp", "sbus", 0, 0, 0, NULL, tegra_clk_mon_avp),
	SHARED_CLK("avp.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_avp_emc),
	SHARED_CLK("cpu.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_cpu_emc),
	SHARED_CLK("disp1.emc", "emc_master", SHARED_ISO_BW, 0, BIT(EMC_USER_DC1), NULL, tegra_clk_disp1_emc),
	SHARED_CLK("disp2.emc", "emc_master", SHARED_ISO_BW, 0, BIT(EMC_USER_DC2), NULL, tegra_clk_disp2_emc),
	SHARED_CLK("hdmi.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_hdmi_emc),
	SHARED_CLK("usbd.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_usbd_emc),
	SHARED_CLK("usb1.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_usb1_emc),
	SHARED_CLK("usb2.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_usb2_emc),
	SHARED_CLK("usb3.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_usb3_emc),
	SHARED_CLK("mon.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_mon_emc),
	SHARED_CLK("msenc.emc", "emc_master", SHARED_BW, 0, BIT(EMC_USER_MSENC), NULL, tegra_clk_msenc_emc),
	SHARED_CLK("tsec.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_tsec_emc),
	SHARED_CLK("sdmmc4.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_sdmmc4_emc),
	SHARED_CLK("camera.emc", "emc_master", SHARED_ISO_BW, 0, BIT(EMC_USER_VI), NULL, tegra_clk_camera_emc),
	SHARED_CLK("iso.emc", "emc_master", SHARED_BW, 0, 0, NULL, tegra_clk_iso_emc),
	SHARED_LIMIT("cap.emc", "emc_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_emc),
	SHARED_LIMIT("cap.throttle.emc", "emc_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_throttle_emc),
	SHARED_LIMIT("floor.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_floor_emc),
	SHARED_CLK("override.emc", "emc_master", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_emc),
	SHARED_LIMIT("edp.emc", "emc_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_edp_emc),
	SHARED_LIMIT("battery.emc", "emc_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_battery_emc),
	SHARED_CLK("gk20a.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_gk20a_emc),
	SHARED_CLK("vic03.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_vic03_emc),
	SHARED_CLK("ispa.emc", "emc_master", SHARED_ISO_BW, 0, BIT(EMC_USER_ISPA), NULL, tegra_clk_ispa_emc),
	SHARED_CLK("ispb.emc", "emc_master", SHARED_ISO_BW, 0, BIT(EMC_USER_ISPB), NULL, tegra_clk_ispb_emc),
	SHARED_CLK("xusb.emc", "emc_master", SHARED_BW, 0, 0, NULL, tegra_clk_xusb_emc),
	SHARED_LIMIT("cap.vcore.c2bus", "c2bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_c2bus),
	SHARED_LIMIT("cap.vcore.c3bus", "c3bus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_c3bus),
	SHARED_CLK("camera.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_camera_sclk),
	SHARED_LIMIT("cap.vcore.sclk", "sbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_sclk),
	SHARED_CLK("qspi.sclk", "apb.sclk", 0, 0, 0, NULL, tegra_clk_qspi_sclk),
	SHARED_CLK("boot.apb.sclk", "apb.sclk", 0, 0, 0, NULL, tegra_clk_boot_apb_sclk),
	SHARED_CLK("disp1.la.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_disp1_la_emc),
	SHARED_CLK("disp2.la.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_disp2_la_emc),
	SHARED_CLK("sdmmc3.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_sdmmc3_emc),
	SHARED_LIMIT("cap.vcore.emc", "emc_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_emc),
	SHARED_CLK("gr3d.emc", "emc_master", 0, 0, BIT(EMC_USER_3D), NULL, tegra_clk_gr3d_emc),
	SHARED_CLK("nvjpg.emc", "emc_master", SHARED_BW, 0, BIT(EMC_USER_NVJPG), NULL, tegra_clk_nvjpg_emc),
	SHARED_CLK("nvdec.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_nvdec_emc),
	SHARED_CLK("tsecb.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_tsecb_emc),
	SHARED_CLK("via.emc", "emc_master", SHARED_ISO_BW, 0, BIT(EMC_USER_VI), NULL, tegra_clk_via_emc),
	SHARED_CLK("vib.emc", "emc_master", SHARED_ISO_BW, 0, BIT(EMC_USER_VI2), NULL, tegra_clk_vib_emc),
	SHARED_CLK("vic.emc", "emc_master", SHARED_BW, 0, 0, NULL, tegra_clk_vic_emc),
	SHARED_CLK("vic.shared_emc", "emc_master", SHARED_BW, 0, 0, NULL, tegra_clk_vic_shared_emc),
	SHARED_CLK("ape.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_ape_emc),
	SHARED_CLK("pcie.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_pcie_emc),
	SHARED_CLK("gm20b.gbus", "gbus", 0, 0, 0, NULL, tegra_clk_gm20b_gbus),
	SHARED_LIMIT("cap.gbus", "gbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_gbus),
	SHARED_LIMIT("edp.gbus", "gbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_edp_gbus),
	SHARED_LIMIT("cap.vgpu.gbus", "gbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vgpu_gbus),
	SHARED_LIMIT("cap.throttle_gbus", "gbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_throttle_gbus),
	SHARED_LIMIT("cap.profile_gbus", "gbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_profile_gbus),
	SHARED_CLK("override.gbus", "gbus", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_gbus),
	SHARED_LIMIT("floor.gbus", "gbus", 0, 0, 0, NULL, tegra_clk_floor_gbus),
	SHARED_LIMIT("floor.profile_gbus", "gbus", 0, 0, 0, NULL, tegra_clk_floor_profile_gbus),
	SHARED_CLK("nv.host1x", "host1x_master", 0, 0, 0, NULL, tegra_clk_nv_host1x),
	SHARED_CLK("vi.host1x", "host1x_master", 0, 0, 0, NULL, tegra_clk_vi_host1x),
	SHARED_CLK("vii2c.host1x", "host1x_master", 0, 0, 0, NULL, tegra_clk_vii2c_host1x),
	SHARED_LIMIT("cap.host1x", "host1x_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_host1x),
	SHARED_LIMIT("cap.vcore.host1x", "host1x_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_host1x),
	SHARED_LIMIT("floor.host1x", "host1x_master", 0, 0, 0, NULL, tegra_clk_floor_host1x),
	SHARED_CLK("override.host1x", "host1x_master", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_host1x),
	SHARED_CLK("cpu.mselect", "mselect_master", 0, 0, 0, NULL, tegra_clk_cpu_mselect),
	SHARED_CLK("pcie.mselect", "mselect_master", 0, 0, 0, NULL, tegra_clk_pcie_mselect),
	SHARED_LIMIT("cap.vcore.mselect", "mselect_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_mselect),
	SHARED_CLK("override.mselect", "mselect_master", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_mselect),
	SHARED_CLK("adma.ape", "ape_master", 0, 0, 0, NULL, tegra_clk_adma_ape),
	SHARED_CLK("adsp.ape", "ape_master", 0, 0, 0, NULL, tegra_clk_adsp_ape),
	SHARED_CLK("xbar.ape", "ape_master", 0, 0, 0, NULL, tegra_clk_xbar_ape),
	SHARED_LIMIT("cap.vcore.ape", "ape_master", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_ape),
	SHARED_CLK("override.ape", "ape_master", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_ape),
	SHARED_LIMIT("cap.vcore.abus", "abus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_abus),
	SHARED_CLK("override.abus", "abus", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_abus),
	SHARED_CLK("vcm.sclk", "sbus", 0, 0, 0, NULL, tegra_clk_vcm_sclk),
	SHARED_CLK("vcm.ahb.sclk", "ahb.sclk", 0, 0, 0, NULL, tegra_clk_vcm_ahb_sclk),
	SHARED_CLK("vcm.apb.sclk", "apb.sclk", 0, 0, 0, NULL, tegra_clk_vcm_apb_sclk),
	SHARED_CLK("sdmmc4.sclk", "ahb.sclk", 0, 0, 0, NULL, tegra_clk_sdmmc4_ahb_sclk),
	SHARED_LIMIT("cap.vcore.cbus", "cbus", SHARED_CEILING, 0, 0, NULL, tegra_clk_cap_vcore_cbus),
	SHARED_CLK("override.cbus", "cbus", SHARED_OVERRIDE, 0, 0, NULL, tegra_clk_override_cbus),
	SHARED_CLK("vic.floor.cbus", "c2bus", 0, 0, 0, NULL, tegra_clk_vic_floor_cbus),
	SHARED_CLK("bwmgr.emc", "emc_master", 0, 0, 0, NULL, tegra_clk_bwmgr_emc),
};

void __init tegra_shared_clk_init(struct tegra_clk *tegra_clks)
{
	int i;
	const char **parents;
	struct tegra_shared_clk *data;
	struct clk *clk;
	struct clk **dt_clk;

	for (i = 0; i < ARRAY_SIZE(shared_clks); i++) {
		data = &shared_clks[i];
		if (data->num_parents == 1)
			parents = &data->p.parent;
		else
			parents = data->p.parents;

		dt_clk = tegra_lookup_dt_id(data->clk_id, tegra_clks);
		if (!dt_clk)
			continue;

		clk = tegra_clk_register_shared(data->name, parents,
				data->num_parents, data->flags, data->usages,
				data->mode, data->client);
		*dt_clk = clk;
	}
}
