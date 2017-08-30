/*
 * Copyright (c) 2014-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __TEGRA_EDP_H
#define __TEGRA_EDP_H

struct tegra_system_edp_entry {
	char speedo_id;
	char power_limit_100mW;
	unsigned int freq_limits[4];
};

struct tegra_sysedp_devcap {
	unsigned int cpu_power;
	unsigned int gpu_cap;
	unsigned int emcfreq;
	unsigned int gpu_supp_freq;
};

struct tegra_sysedp_corecap {
	unsigned int power;
	struct tegra_sysedp_devcap cpupri;
	struct tegra_sysedp_devcap gpupri;
	unsigned int pthrot;
};

struct tegra_sysedp_platform_data {
	struct tegra_sysedp_corecap *corecap;
	unsigned int corecap_size;
	unsigned int core_gain;
	unsigned int init_req_watts;
	unsigned int pthrot_ratio;
	unsigned int cap_method;
	bool gpu_supplement;
};

#ifdef CONFIG_TEGRA_CPU_EDP
extern int tegra_cpu_edp_get_thermal_index(struct platform_device *pdev);
extern int tegra_cpu_edp_count_therm_floors(struct platform_device *pdev);
extern int tegra_cpu_edp_update_thermal_index(struct platform_device *pdev,
					      unsigned long new_idx);
extern bool tegra_cpu_edp_ready(void);
#else
static inline int
tegra_cpu_edp_get_thermal_index(struct platform_device *pdev)
{ return 0; }
static inline int
tegra_cpu_edp_count_therm_floors(struct platform_device *pdev)
{ return 0; }
static inline int
tegra_cpu_edp_update_thermal_index(struct platform_device *pdev,
				   unsigned long new_idx)
{ return 0; }
static inline bool tegra_cpu_edp_ready(void)
{ return false; }
#endif

#endif	/* __TEGRA_EDP_H */
