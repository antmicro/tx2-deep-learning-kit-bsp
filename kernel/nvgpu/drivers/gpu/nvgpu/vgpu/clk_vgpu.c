/*
 * Virtualized GPU Clock Interface
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
 */

#include "vgpu/vgpu.h"
#include "vgpu/clk_vgpu.h"

static unsigned long
vgpu_freq_table[TEGRA_VGPU_GPU_FREQ_TABLE_SIZE];

unsigned long vgpu_clk_get_rate(struct device *dev)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);
	struct gk20a *g = platform->g;
	struct tegra_vgpu_cmd_msg msg = {};
	struct tegra_vgpu_gpu_clk_rate_params *p = &msg.params.gpu_clk_rate;

	int err;

	gk20a_dbg_fn("");

	msg.cmd = TEGRA_VGPU_CMD_GET_GPU_CLK_RATE;
	msg.handle = vgpu_get_handle(g);
	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	if (err) {
		gk20a_err(dev_from_gk20a(g),
			"%s failed - %d", __func__, err);
		return 0;
	}

	/* return frequency in Hz */
	return p->rate * 1000;
}

long vgpu_clk_round_rate(struct device *dev, unsigned long rate)
{
	/* server will handle frequency rounding */
	return rate;
}

int vgpu_clk_set_rate(struct device *dev, unsigned long rate)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);
	struct gk20a *g = platform->g;
	struct tegra_vgpu_cmd_msg msg = {};
	struct tegra_vgpu_gpu_clk_rate_params *p = &msg.params.gpu_clk_rate;

	int err = 0;

	gk20a_dbg_fn("");

	msg.cmd = TEGRA_VGPU_CMD_SET_GPU_CLK_RATE;
	msg.handle = vgpu_get_handle(g);

	/* server dvfs framework requires frequency in kHz */
	p->rate = (u32)(rate / 1000);
	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	if (err) {
		gk20a_err(dev_from_gk20a(g),
				"%s failed - %d", __func__, err);
		return err;
	}

	return 0;
}

int vgpu_clk_get_freqs(struct device *dev,
		unsigned long **freqs, int *num_freqs)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);
	struct gk20a *g = platform->g;
	struct tegra_vgpu_cmd_msg msg = {};
	struct tegra_vgpu_get_gpu_freq_table_params *p =
					&msg.params.get_gpu_freq_table;
	unsigned int i;
	int err;

	gk20a_dbg_fn("");

	msg.cmd = TEGRA_VGPU_CMD_GET_GPU_FREQ_TABLE;
	msg.handle = vgpu_get_handle(g);

	p->num_freqs = TEGRA_VGPU_GPU_FREQ_TABLE_SIZE;
	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	if (err) {
		gk20a_err(dev_from_gk20a(g),
				"%s failed - %d", __func__, err);
		return err;
	}

	/* return frequency in Hz */
	for (i = 0; i < p->num_freqs; i++)
		vgpu_freq_table[i] = p->freqs[i] * 1000;

	*freqs = vgpu_freq_table;
	*num_freqs = p->num_freqs;

	return 0;
}

int vgpu_clk_cap_rate(struct device *dev, unsigned long rate)
{
	struct gk20a_platform *platform = gk20a_get_platform(dev);
	struct gk20a *g = platform->g;
	struct tegra_vgpu_cmd_msg msg = {};
	struct tegra_vgpu_gpu_clk_rate_params *p = &msg.params.gpu_clk_rate;

	int err = 0;

	gk20a_dbg_fn("");

	msg.cmd = TEGRA_VGPU_CMD_CAP_GPU_CLK_RATE;
	msg.handle = vgpu_get_handle(g);
	p->rate = (u32)rate;
	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	if (err) {
		gk20a_err(dev_from_gk20a(g),
			"%s failed - %d", __func__, err);
		return err;
	}

	return 0;
}
