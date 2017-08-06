/*
 * Copyright (c) 2011-2017, NVIDIA CORPORATION. All Rights Reserved.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_qos.h>
#include <linux/debugfs.h>
#include <soc/tegra/fuse.h>
#include <linux/thermal.h>

#include <soc/tegra/tegra-dvfs.h>
#include <soc/tegra/tegra-ppm.h>

struct gpu_edp_platform_data {
	char *name;
	int freq_step;
	int imax;
};

struct edp_attrs {
	struct gpu_edp *ctx;
	int *var;
	const char *name;
};

struct gpu_edp {
	struct tegra_ppm *ppm;
	struct mutex edp_lock;
	struct clk *cap_clk;
	struct thermal_cooling_device *cdev;

	int pmax;
	int imax;
	int temperature_now;

	struct edp_attrs *debugfs_attrs;

};

static int tegra_gpu_edp_predict_millivolts(void *p, unsigned long rate)
{
	struct clk *clk = (struct clk *)p;

	return tegra_dvfs_predict_millivolts(clk, rate);
}

static void edp_update_cap(struct gpu_edp *ctx, int temperature,
			  int imax, int pmax)
{
	unsigned clk_rate;

	mutex_lock(&ctx->edp_lock);

	ctx->temperature_now = temperature;
	ctx->imax = imax;
	ctx->pmax = pmax;
	clk_rate = min(tegra_ppm_get_maxf(ctx->ppm, ctx->imax,
					  TEGRA_PPM_UNITS_MILLIAMPS,
					  ctx->temperature_now, 1),
		       tegra_ppm_get_maxf(ctx->ppm, ctx->pmax,
					  TEGRA_PPM_UNITS_MILLIWATTS,
					  ctx->temperature_now, 1));

	clk_set_rate(ctx->cap_clk, clk_rate * 1000);

	mutex_unlock(&ctx->edp_lock);
}

#define C_TO_K(c) (c+273)
#define K_TO_C(k) (k-273)
#define MELT_SILICON_K 1687 /* melting point of silicon */

static int edp_get_cdev_max_state(struct thermal_cooling_device *cdev,
				       unsigned long *max_state)
{
	*max_state = MELT_SILICON_K;
	return 0;
}

static int edp_get_cdev_cur_state(struct thermal_cooling_device *cdev,
				       unsigned long *cur_state)
{
	struct gpu_edp *ctx = cdev->devdata;
	*cur_state = C_TO_K(ctx->temperature_now);
	return 0;
}

static int edp_set_cdev_state(struct thermal_cooling_device *cdev,
				   unsigned long cur_state)
{
	struct gpu_edp *ctx = cdev->devdata;

	if (cur_state == 0)
		return 0;

	BUG_ON(cur_state >= MELT_SILICON_K);

	edp_update_cap(ctx, K_TO_C(cur_state),
		       ctx->imax, ctx->pmax);
	return 0;
}

/* cooling devices using these ops interpret the "cur_state" as
 * a temperature in Kelvin
 */
static struct thermal_cooling_device_ops edp_cooling_ops = {
	.get_max_state = edp_get_cdev_max_state,
	.get_cur_state = edp_get_cdev_cur_state,
	.set_cur_state = edp_set_cdev_state,
};

static struct gpu_edp *s_gpu;
static int max_gpu_power_notify(struct notifier_block *b,
				unsigned long max_gpu_pwr, void *v)
{
	/* XXX there's no private data for a pm_qos notifier call */
	struct gpu_edp *ctx = s_gpu;

	edp_update_cap(ctx, ctx->temperature_now,
		       ctx->imax, max_gpu_pwr);

	return NOTIFY_OK;
}
static struct notifier_block max_gpu_pwr_notifier = {
	.notifier_call = max_gpu_power_notify,
};

#ifdef CONFIG_DEBUG_FS
static int show_edp_max(void *data, u64 *val)
{
	struct edp_attrs *attr = data;
	*val = *attr->var;
	return 0;
}
static int store_edp_max(void *data, u64 val)
{
	struct edp_attrs *attr = data;
	struct gpu_edp *ctx = attr->ctx;

	*attr->var = val;

	edp_update_cap(ctx, ctx->temperature_now,
		       ctx->imax, ctx->pmax);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(edp_max_fops, show_edp_max, store_edp_max, "%llu\n");

static int tegra_edp_debugfs_init(struct device *dev, struct gpu_edp *ctx)
{
	struct dentry *edp_dir;
	struct edp_attrs *attr;
	struct edp_attrs attrs[] = {
		{ ctx, &ctx->imax, "imax" },
		{ ctx, &ctx->pmax, "pmax" },
		{ NULL}
	};

	edp_dir = debugfs_create_dir("gpu_edp", NULL);
	if (IS_ERR_OR_NULL(edp_dir))
		return -ENOMEM;

	ctx->debugfs_attrs = devm_kzalloc(dev, sizeof(attrs), GFP_KERNEL);
	if (!ctx->debugfs_attrs)
		return -ENOMEM;

	memcpy(ctx->debugfs_attrs, attrs, sizeof(attrs));

	for (attr = ctx->debugfs_attrs; attr->ctx; attr++)
		debugfs_create_file(attr->name, S_IRUGO | S_IWUSR,
				    edp_dir, attr, &edp_max_fops);

	debugfs_create_u32("temperature", S_IRUGO,
			   edp_dir, &ctx->temperature_now);

	return 0;
}
#else
static int tegra_edp_debugfs_init(void)
{ return 0; }
#endif /* CONFIG_DEBUG_FS */

static int tegra_gpu_edp_parse_dt(struct platform_device *pdev,
				  struct gpu_edp_platform_data *pdata)
{

	struct device_node *np = pdev->dev.of_node;

	if (WARN(of_property_read_u32(np, "nvidia,freq_step",
				      &pdata->freq_step),
		 "missing required parameter: nvidia,freq_step\n"))
		return -ENODATA;

	if (WARN(of_property_read_u32(np, "nvidia,edp_limit",
				      &pdata->imax),
		 "missing required parameter: nvidia,edp_limit\n"))
		return -ENODATA;

	pdata->name = "gpu_edp";

	return 0;
}

static int tegra_gpu_edp_probe(struct platform_device *pdev)
{
	struct clk *gpu_clk;
	struct fv_relation *fv = NULL;
	struct tegra_ppm_params *params;
	unsigned iddq_ma;
	struct gpu_edp_platform_data pdata;
	struct gpu_edp *ctx;
	int ret;
	long maxf;

	if (WARN(!pdev || !pdev->dev.of_node, "DT node missing!\n"))
		return -EINVAL;

	ret = tegra_gpu_edp_parse_dt(pdev, &pdata);
	if (ret) {
		dev_err(&pdev->dev, "failed to parse device-tree\n");
		return ret;
	}

	params = of_read_tegra_ppm_params(pdev->dev.of_node);
	if (IS_ERR_OR_NULL(params)) {
		dev_err(&pdev->dev, "failed to initialise PPM params\n");
		return PTR_ERR(params);
	}

	ctx = devm_kzalloc(&pdev->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto free_params;
	}

	/*
	 * FIXME: Ideally we should look up the clock via device-tree,
	 * but currently this clock is only register as a devclk and so
	 * using device-tree is not possible.
	 */
	gpu_clk = clk_get_sys("gbus", "gbus");
	if (IS_ERR(gpu_clk)) {
		ret = -EPROBE_DEFER;
		goto free_params;
	}

	maxf = clk_round_rate(gpu_clk, ULONG_MAX);
	if (maxf <= 0) {
		dev_err(&pdev->dev, "unable to get max GPU freq\n");
		ret = -EPROBE_DEFER;
		goto free_params;
	}

	fv = fv_relation_create(gpu_clk, pdata.freq_step, 150, maxf, 0,
				tegra_gpu_edp_predict_millivolts);
	if (IS_ERR_OR_NULL(fv)) {
		dev_err(&pdev->dev, "failed to create freq/volt table\n");
		ret = PTR_ERR(fv);
		goto free_params;
	}

	iddq_ma = tegra_fuse_get_gpu_iddq();
	if (iddq_ma < 0) {
		dev_err(&pdev->dev, "unable to get GPU IDDQ: %d\n", iddq_ma);
		ret = iddq_ma;
		goto destroy_fv;
	}

	dev_dbg(&pdev->dev, "%s IDDQ value %d\n", pdata.name, iddq_ma);

	ctx->cap_clk = devm_clk_get(&pdev->dev, "gpu-edp-cap");
	if (IS_ERR(ctx->cap_clk)) {
		dev_err(&pdev->dev, "unable to find 'gpu-edp-cap' clock\n");
		ret = PTR_ERR(ctx->cap_clk);
		goto destroy_fv;
	}

	ctx->ppm = tegra_ppm_create(pdata.name, fv, params, iddq_ma, NULL);
	if (IS_ERR_OR_NULL(ctx->ppm)) {
		dev_err(&pdev->dev, "failed to create power model\n");
		ret = PTR_ERR(ctx->ppm);
		goto destroy_fv;
	}

	ctx->temperature_now = -273; /* HACK */
	ctx->imax = pdata.imax;
	ctx->pmax = PM_QOS_GPU_POWER_MAX_DEFAULT_VALUE;

	mutex_init(&ctx->edp_lock);

	s_gpu = ctx;
	ret = pm_qos_add_notifier(PM_QOS_MAX_GPU_POWER, &max_gpu_pwr_notifier);
	if (ret) {
		dev_err(&pdev->dev, "failed to add PM QOS notifier\n");
		goto destroy_ppm;
	}

	ctx->cdev = thermal_of_cooling_device_register(pdev->dev.of_node,
						       pdata.name, ctx,
						       &edp_cooling_ops);
	if (IS_ERR_OR_NULL(ctx->cdev)) {
		dev_err(&pdev->dev, "failed to register cooling device\n");
		ret = PTR_ERR(ctx->cdev);
		goto remove_pm_qos;
	}

	ret = tegra_edp_debugfs_init(&pdev->dev, ctx);
	if (ret) {
		dev_err(&pdev->dev, "failed to initialise debugfs\n");
		goto remove_cdev;
	}

	return 0;

remove_cdev:
	thermal_cooling_device_unregister(ctx->cdev);
remove_pm_qos:
	pm_qos_remove_notifier(PM_QOS_MAX_GPU_POWER, &max_gpu_pwr_notifier);
destroy_ppm:
	tegra_ppm_destroy(ctx->ppm, NULL, NULL);
destroy_fv:
	fv_relation_destroy(fv);
free_params:
	kfree(params);

	return ret;
}

static struct of_device_id tegra_gpu_edp_of_match[] = {
	{ .compatible = "nvidia,tegra124-gpu-edp-capping" },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_gpu_edp_of_match);

static struct platform_driver tegra_gpu_edp_driver = {
	.driver = {
		.name = "tegra_gpu_edp",
		.owner = THIS_MODULE,
		.of_match_table = tegra_gpu_edp_of_match,
	},
	.probe = tegra_gpu_edp_probe,
};

module_platform_driver(tegra_gpu_edp_driver);

MODULE_AUTHOR("NVIDIA Corp.");
MODULE_DESCRIPTION("Tegra GPU EDP management");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:tegra-gpu-edp");
