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

#ifndef __LINUX_CLK_TEGRA_H_
#define __LINUX_CLK_TEGRA_H_

#include <linux/types.h>
#include <linux/bug.h>
#include <linux/clk.h>

/*
 * Tegra CPU clock and reset control ops
 *
 * wait_for_reset:
 *	keep waiting until the CPU in reset state
 * put_in_reset:
 *	put the CPU in reset state
 * out_of_reset:
 *	release the CPU from reset state
 * enable_clock:
 *	CPU clock un-gate
 * disable_clock:
 *	CPU clock gate
 * rail_off_ready:
 *	CPU is ready for rail off
 * suspend:
 *	save the clock settings when CPU go into low-power state
 * resume:
 *	restore the clock settings when CPU exit low-power state
 */
struct tegra_cpu_car_ops {
	void (*wait_for_reset)(u32 cpu);
	void (*put_in_reset)(u32 cpu);
	void (*out_of_reset)(u32 cpu);
	void (*enable_clock)(u32 cpu);
	void (*disable_clock)(u32 cpu);
#ifdef CONFIG_PM_SLEEP
	bool (*rail_off_ready)(void);
	void (*suspend)(void);
	void (*resume)(void);
#endif
};

extern struct tegra_cpu_car_ops *tegra_cpu_car_ops;

static inline void tegra_wait_cpu_in_reset(u32 cpu)
{
	if (WARN_ON(!tegra_cpu_car_ops->wait_for_reset))
		return;

	tegra_cpu_car_ops->wait_for_reset(cpu);
}

static inline void tegra_put_cpu_in_reset(u32 cpu)
{
	if (WARN_ON(!tegra_cpu_car_ops->put_in_reset))
		return;

	tegra_cpu_car_ops->put_in_reset(cpu);
}

static inline void tegra_cpu_out_of_reset(u32 cpu)
{
	if (WARN_ON(!tegra_cpu_car_ops->out_of_reset))
		return;

	tegra_cpu_car_ops->out_of_reset(cpu);
}

static inline void tegra_enable_cpu_clock(u32 cpu)
{
	if (WARN_ON(!tegra_cpu_car_ops->enable_clock))
		return;

	tegra_cpu_car_ops->enable_clock(cpu);
}

static inline void tegra_disable_cpu_clock(u32 cpu)
{
	if (WARN_ON(!tegra_cpu_car_ops->disable_clock))
		return;

	tegra_cpu_car_ops->disable_clock(cpu);
}

#ifdef CONFIG_PM_SLEEP
static inline bool tegra_cpu_rail_off_ready(void)
{
	if (WARN_ON(!tegra_cpu_car_ops->rail_off_ready))
		return false;

	return tegra_cpu_car_ops->rail_off_ready();
}

static inline void tegra_cpu_clock_suspend(void)
{
	if (WARN_ON(!tegra_cpu_car_ops->suspend))
		return;

	tegra_cpu_car_ops->suspend();
}

static inline void tegra_cpu_clock_resume(void)
{
	if (WARN_ON(!tegra_cpu_car_ops->resume))
		return;

	tegra_cpu_car_ops->resume();
}
#endif

enum tegra_clk_ex_param {
	TEGRA_CLK_VI_INP_SEL,
	TEGRA_CLK_DTV_INVERT,
	TEGRA_CLK_NAND_PAD_DIV2_ENB,
	TEGRA_CLK_PLLD_CSI_OUT_ENB,
	TEGRA_CLK_PLLD_DSI_OUT_ENB,
	TEGRA_CLK_PLLD_MIPI_MUX_SEL,
	TEGRA_CLK_SOR_CLK_SEL,
	TEGRA_CLK_MIPI_CSI_OUT_ENB,
};

static inline void tegra_periph_reset_deassert(struct clk *c) { }
static inline void tegra_periph_reset_assert(struct clk *c) { }

#ifdef CONFIG_COMMON_CLK
/* Keep using these functions until the replacement in place */
int tegra_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting);
/* To be implemented for COMMON CLK framework */
/* Get max rate safe at min voltage in all t-ranges; return zero if unknown */
static inline long tegra_emc_round_rate_updown(unsigned long rate, bool up)
{
       return 0;
}
#else

#ifdef CONFIG_ARCH_TEGRA_HAS_CL_DVFS
int tegra_dvfs_clamp_dfll_at_vmin(struct clk *dfll_clk, bool clamp);
int tegra_dvfs_cmp_dfll_vmin_tfloor(struct clk *dfll_clk, int *tfloor);
#else
static inline int tegra_dvfs_clamp_dfll_at_vmin(struct clk *dfll_clk,
		                                                bool clamp)
{ return -ENOSYS; }
static inline int tegra_dvfs_cmp_dfll_vmin_tfloor(struct clk *dfll_clk,
		                                                int *tfloor)
{ return 0; }
#endif

int tegra_is_clk_enabled(struct clk *clk);

unsigned long tegra_dvfs_predict_hz_at_mv_max_tfloor(struct clk *c, int mv);
int tegra_dvfs_predict_mv_at_hz_no_tfloor(struct clk *c, unsigned long rate);
int tegra_dvfs_predict_mv_at_hz_cur_tfloor(struct clk *c, unsigned long rate);
int tegra_dvfs_set_fmax_at_vmin(struct clk *c, unsigned long f_max, int v_min);

static inline void tegra_clocks_apply_init_table(void)
{}

unsigned long clk_get_rate_all_locked(struct clk *c);
int tegra_clk_cfg_ex(struct clk *c, enum tegra_clk_ex_param p, u32 setting);
#endif

#ifdef CONFIG_ARCH_TEGRA_210_SOC
extern bool tegra210_xusb_pll_hw_sequence_is_enabled(void);
extern void tegra210_xusb_pll_hw_control_enable(void);
extern void tegra210_xusb_pll_hw_sequence_start(void);
extern bool tegra210_sata_pll_hw_sequence_is_enabled(void);
extern void tegra210_sata_pll_hw_control_enable(void);
extern void tegra210_sata_pll_hw_sequence_start(void);
extern void tegra210_set_sata_pll_seq_sw(bool state);
extern void tegra210_put_utmipll_in_iddq(void);
extern void tegra210_put_utmipll_out_iddq(void);
extern bool tegra210_plle_hw_sequence_is_enabled(void);
extern void tegra210_plle_hw_sequence_start(void);
extern void tegra210_csi_source_from_brick(void);
extern void tegra210_csi_source_from_plld(void);
#else
static inline bool tegra210_xusb_pll_hw_sequence_is_enabled(void)
{
	return false;
}
static inline void tegra210_xusb_pll_hw_control_enable(void)
{
}
static inline void tegra210_xusb_pll_hw_sequence_start(void)
{
}
static inline bool tegra210_sata_pll_hw_sequence_is_enabled(void)
{
	return false;
}
static inline void tegra210_sata_pll_hw_control_enable(void)
{
}
static inline void tegra210_sata_pll_hw_sequence_start(void)
{
}
static inline void tegra210_set_sata_pll_seq_sw(bool state)
{
}
static inline void tegra210_put_utmipll_in_iddq(void)
{
}
static inline void tegra210_put_utmipll_out_iddq(void)
{
}
static inline bool tegra210_plle_hw_sequence_is_enabled(void)
{
	return false;
}
static inline void tegra210_plle_hw_sequence_start(void)
{
}

static inline void tegra210_csi_source_from_brick(void)
{
}

static inline void tegra210_csi_source_from_plld(void)
{
}
#endif

int tegra_super_cdiv_use_therm_controls(bool);

#endif /* __LINUX_CLK_TEGRA_H_ */
