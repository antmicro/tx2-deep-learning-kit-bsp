/*
 * Copyright (c) 2012-2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef __LINUX_TEGRA_COMMON_H_
#define __LINUX_TEGRA_COMMON_H_

struct board_info {
	u16 board_id;
	u16 sku;
	u8  fab;
	u8  major_revision;
	u8  minor_revision;
};

int tegra_get_sku_override(void);
int tegra_get_chip_personality(void);
enum panel_type get_panel_type(void);
int tegra_get_board_panel_id(void);
bool tegra_is_bl_display_initialized(int instance);
int tegra_get_touch_vendor_id(void);
int tegra_get_touch_panel_id(void);
u8 get_power_config(void);
u8 get_display_config(void);
int get_core_edp(void);
int get_maximum_core_current_supported(void);
int get_emc_max_dvfs(void);
int tegra_get_memory_type(void);
bool is_uart_over_sd_enabled(void);
enum image_type get_tegra_image_type(void);
enum audio_codec_type get_audio_codec_type(void);
int get_pwr_i2c_clk_rate(void);
void tegra_get_board_info(struct board_info *bi);
void tegra_get_pmu_board_info(struct board_info *bi);
void tegra_get_display_board_info(struct board_info *bi);
void tegra_get_camera_board_info(struct board_info *bi);
void tegra_get_leftspeaker_board_info(struct board_info *bi);
void tegra_get_rightspeaker_board_info(struct board_info *bi);
void tegra_get_joystick_board_info(struct board_info *bi);
void tegra_get_button_board_info(struct board_info *bi);
void tegra_get_io_board_info(struct board_info *bi);
int tegra_get_modem_id(void);
int tegra_get_usb_port_owner_info(void);
int tegra_get_lane_owner_info(void);
int tegra_get_commchip_id(void);
#ifdef CONFIG_ANDROID
bool get_androidboot_mode_charger(void);
#endif
int tegra_split_mem_active(void);
unsigned long int tegra_dfll_boot_req_khz(void);
void __init display_tegra_dt_info(void);
int tegra_state_idx_from_name(char *state_name);

#endif
