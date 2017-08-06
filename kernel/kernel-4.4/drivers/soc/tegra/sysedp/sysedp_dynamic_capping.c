/*
 * Copyright (c) 2013-2016, NVIDIA CORPORATION.  All rights reserved.
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
#include <linux/cpu.h>
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_data/tegra_edp.h>
#include <linux/pm_qos.h>
#include <linux/workqueue.h>
#include <soc/tegra/tegra-edp.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <trace/events/sysedp.h>
#include <dt-bindings/soc/tegra-sysedp.h>

#include "sysedp_internal.h"

/* Threshold of gpu load, the rang is 0 ~ 1000 */
static unsigned int gpu_high_threshold = 500;
/* Delay (in ms) to update capping if gpu is not busy */
static unsigned int gpu_window = 80;
/* GPU busy if load was above threshold for at least gpu_high_count */
static unsigned int gpu_high_count = 2;
/*
 * Choose CPU priority until how GPU frequency gets "near"
 * the CPU-priority-limited-GPU-fmax, this parameter is used
 * to facilitate tuning of "near", the rang is 0 ~ 100.
 */
static unsigned int priority_bias = 75;
/* History of gpu high load */
static unsigned int gpu_high_hist;
/* Indicate gpu busy or not */
static bool gpu_busy;
/* Frequency of GPU in kHz */
static unsigned int fgpu;

/* available power budget (in mW) */
static unsigned int avail_power;
/* available additional power budget (in mW) */
static unsigned int avail_oc_relax;
/* CPU balanced power (in mW) */
static unsigned int cpu_power_balance;
/* defines which calculation to use when determinging corecap entry */
static unsigned int cap_method;
/* force in gpu priority */
static unsigned int force_gpu_pri;

/* current core capping */
static struct tegra_sysedp_corecap *cur_corecap;
static struct clk *emc_cap_clk;

/* Qos to notify cpu power */
static struct pm_qos_request cpupwr_qos;
/* Qos to notify gpu power */
static struct pm_qos_request gpupwr_qos;

/* core capping which are forced to */
static struct tegra_sysedp_devcap forced_caps;
/* core capping which need to update to */
static struct tegra_sysedp_devcap core_policy;
/* core capping which are currently used */
static struct tegra_sysedp_devcap cur_caps;

/* Locked when update capability of cpu/gpu/emc clocks */
static DEFINE_MUTEX(core_lock);

static struct delayed_work capping_work;
static struct tegra_sysedp_platform_data *capping_data;
static int init_done;

static void pr_caps(struct tegra_sysedp_devcap *old,
		    struct tegra_sysedp_devcap *new)
{
	if (!IS_ENABLED(CONFIG_DEBUG_KERNEL))
		return;

	if ((new->cpu_power == old->cpu_power) &&
	    (new->gpu_cap == old->gpu_cap) &&
	    (new->emcfreq == old->emcfreq))
		return;

	pr_debug("sysedp: gpupri %d, core %5u mW, cpu %5u mW, gpu %u mW, emc %u kHz\n",
		 gpu_busy, cur_corecap->power,
		 new->cpu_power, new->gpu_cap,
		 new->emcfreq);
}

static void apply_caps(struct tegra_sysedp_devcap *devcap)
{
	struct tegra_sysedp_devcap new;
	int r;
	bool do_trace = false;

	core_policy.cpu_power = devcap->cpu_power + cpu_power_balance;
	core_policy.gpu_cap = devcap->gpu_cap;
	core_policy.emcfreq = devcap->emcfreq;

	new.cpu_power = forced_caps.cpu_power ?: core_policy.cpu_power;
	new.gpu_cap = forced_caps.gpu_cap ?: core_policy.gpu_cap;
	new.emcfreq = forced_caps.emcfreq ?: core_policy.emcfreq;

	if (new.cpu_power != cur_caps.cpu_power) {
		pm_qos_update_request(&cpupwr_qos, new.cpu_power);
		do_trace = true;
	}

	if (new.emcfreq != cur_caps.emcfreq) {
		r = clk_set_rate(emc_cap_clk, new.emcfreq * 1000);
		WARN_ON(r);
		do_trace = true;
	}

	if (new.gpu_cap != cur_caps.gpu_cap) {
		pm_qos_update_request(&gpupwr_qos, new.gpu_cap);
		do_trace = true;
	}

	if (do_trace)
		trace_sysedp_dynamic_capping(new.cpu_power, new.gpu_cap,
					new.emcfreq, gpu_busy);
	pr_caps(&cur_caps, &new);
	cur_caps = new;
}

static inline bool gpu_priority(void)
{
	bool prefer_gpu = gpu_busy;
	unsigned int bias;

	/*
	 * NOTE: It is highly preferred to use supplemental
	 * GPU capping tables expressed in KHz.
	 */
	if (capping_data->gpu_supplement)
		bias = cur_corecap->cpupri.gpu_supp_freq;
	else
		bias = 0;

	if (bias) {
		bias = bias * priority_bias / 100;
		prefer_gpu = prefer_gpu && (fgpu > bias);
	}

	return force_gpu_pri || prefer_gpu;
}

static inline struct tegra_sysedp_devcap *get_devcap(void)
{
	return gpu_priority() ? &cur_corecap->gpupri : &cur_corecap->cpupri;
}

static void _do_cap_control(void)
{
	if (!cur_corecap)
		return;
	apply_caps(get_devcap());
}

static void do_cap_control(void)
{
	mutex_lock(&core_lock);
	_do_cap_control();
	mutex_unlock(&core_lock);
}

static void _update_cur_corecap(void)
{
	struct tegra_sysedp_corecap *cap;
	unsigned int power;
	unsigned int relaxed_power;
	int i;

	if (!capping_data)
		return;

	power = avail_power * capping_data->core_gain / 100;

	for (i = capping_data->corecap_size - 1; i >= 0; i--) {
		cap = &capping_data->corecap[i];
		switch (cap_method) {
		default:
			pr_warn("%s: Unknown cap_method, %x!  Assuming direct.\n",
					__func__, cap_method);
			cap_method = SYSEDP_CAP_METHOD_DIRECT;
			/* Intentional fall-through*/
		case SYSEDP_CAP_METHOD_DIRECT:
			relaxed_power = 0;
			break;

		case SYSEDP_CAP_METHOD_SIGNAL:
			relaxed_power = min(avail_oc_relax, cap->pthrot);
			break;

		case SYSEDP_CAP_METHOD_RELAX:
			relaxed_power = cap->pthrot;
			break;
		}

		/*
		 * cap->power is the power required by the AP+DRAM.
		 * power is the available power budget.
		 * relaxed_power is the available additional power budget.
		 * find the corecap if the required power is less than
		 * the available power.
		 */
		if (cap->power <= power + relaxed_power) {
			cur_corecap = cap;
			cpu_power_balance = power + relaxed_power
				- cap->power;
			return;
		}
	}

	cur_corecap = capping_data->corecap;
	cpu_power_balance = 0;
}

/* set the available power budget for cpu/gpu/emc (in mW) */
void sysedp_set_dynamic_cap(unsigned int power, unsigned int oc_relax)
{
	if (!init_done)
		return;

	mutex_lock(&core_lock);
	avail_power = power;
	avail_oc_relax = oc_relax;
	_update_cur_corecap();
	_do_cap_control();
	mutex_unlock(&core_lock);
}

int sysedp_dynamic_cap_ready(void)
{
	return init_done;
}

static void capping_worker(struct work_struct *work)
{
	if (!gpu_busy)
		do_cap_control();
}

/*
 * Return true if load was above threshold for at least
 * gpu_high_count number of notifications
 */
static bool calc_gpu_busy(unsigned int load)
{
	unsigned int mask;
	bool busy;

	mutex_lock(&core_lock);
	mask = (1 << gpu_high_count) - 1;

	gpu_high_hist <<= 1;
	if (load >= gpu_high_threshold)
		gpu_high_hist |= 1;

	busy = (gpu_high_hist & mask) == mask;
	mutex_unlock(&core_lock);

	return busy;
}

void tegra_edp_notify_gpu_load(unsigned int load, unsigned int freq_in_hz)
{
	bool old;

	old = gpu_busy;
	gpu_busy = calc_gpu_busy(load);
	fgpu = freq_in_hz / 1000;

	if (gpu_busy == old || force_gpu_pri || !capping_data)
		return;

	cancel_delayed_work(&capping_work);

	if (gpu_busy)
		do_cap_control();
	else
		schedule_delayed_work(&capping_work,
				msecs_to_jiffies(gpu_window));
}
EXPORT_SYMBOL(tegra_edp_notify_gpu_load);

#ifdef CONFIG_DEBUG_FS
static struct dentry *capping_debugfs_dir;

#define DEFINE_SDC_SIMPLE_ATTR(__name, __var)				     \
static int __name##_set(void *data, u64 val)				     \
{									     \
	if (val != __var) {						     \
		__var = val;						     \
		do_cap_control();					     \
	}								     \
									     \
	return 0;							     \
}									     \
									     \
static int __name##_get(void *data, u64 *val)				     \
{									     \
	*val = __var;							     \
	return 0;							     \
}									     \
									     \
DEFINE_SIMPLE_ATTRIBUTE(__name##_fops, __name##_get, __name##_set, "%lld\n")

DEFINE_SDC_SIMPLE_ATTR(favor_gpu, force_gpu_pri);
DEFINE_SDC_SIMPLE_ATTR(gpu_threshold, gpu_high_threshold);
DEFINE_SDC_SIMPLE_ATTR(force_cpu_power, forced_caps.cpu_power);
DEFINE_SDC_SIMPLE_ATTR(force_gpu, forced_caps.gpu_cap);
DEFINE_SDC_SIMPLE_ATTR(force_emc, forced_caps.emcfreq);
DEFINE_SDC_SIMPLE_ATTR(gpu_window, gpu_window);
DEFINE_SDC_SIMPLE_ATTR(gpu_high_count, gpu_high_count);
DEFINE_SDC_SIMPLE_ATTR(priority_bias, priority_bias);

#define DEFINE_SDC_UPDATE_ATTR(__name, __var)				     \
static int __name##_set(void *data, u64 val)				     \
{									     \
	if (val != __var) {						     \
		__var = val;						     \
		mutex_lock(&core_lock);					     \
		_update_cur_corecap();					     \
		_do_cap_control();					     \
		mutex_unlock(&core_lock);				     \
	}								     \
									     \
	return 0;							     \
}									     \
									     \
static int __name##_get(void *data, u64 *val)				     \
{									     \
	*val = __var;							     \
	return 0;							     \
}									     \
									     \
DEFINE_SIMPLE_ATTRIBUTE(__name##_fops, __name##_get, __name##_set, "%lld\n")

DEFINE_SDC_UPDATE_ATTR(gain, capping_data->core_gain);
DEFINE_SDC_UPDATE_ATTR(cap_method, cap_method);

static int corecaps_show(struct seq_file *file, void *data)
{
	int i;
	struct tegra_sysedp_corecap *p;
	struct tegra_sysedp_devcap *c;
	struct tegra_sysedp_devcap *g;

	if (!capping_data || !capping_data->corecap)
		return -ENODEV;

	p = capping_data->corecap;

	seq_printf(file, "%s %s { %s %9s %9s } %s { %s %9s %9s } %7s\n",
		   "E-state",
		   "CPU-pri", "CPU-mW", "GPU-mW", "EMC-kHz",
		   "GPU-pri", "CPU-mW", "GPU-mW", "EMC-kHz",
		   "Pthrot");

	for (i = 0; i < capping_data->corecap_size; i++, p++) {
		c = &p->cpupri;
		g = &p->gpupri;
		seq_printf(file, "%7u %16u %9u %9u %18u %9u %9u %7u\n",
			   p->power,
			   c->cpu_power, c->gpu_cap, c->emcfreq,
			   g->cpu_power, g->gpu_cap, g->emcfreq,
			   p->pthrot);
	}

	return 0;
}

static int corecaps_open(struct inode *inode, struct file *file)
{
	return single_open(file, corecaps_show, inode->i_private);
}

static const struct file_operations corecaps_fops = {
	.open = corecaps_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int status_show(struct seq_file *file, void *data)
{
	mutex_lock(&core_lock);

	seq_printf(file, "gpu priority: %u\n", gpu_priority());
	seq_printf(file, "gain        : %u\n",
		   capping_data->core_gain);
	seq_printf(file, "core cap    : %u\n", cur_corecap->power);
	seq_printf(file, "max throttle: %u\n", cur_corecap->pthrot);
	seq_printf(file, "cpu balance : %u\n", cpu_power_balance);
	seq_printf(file, "cpu power   : %u\n", get_devcap()->cpu_power +
		   cpu_power_balance);
	seq_printf(file, "gpu cap     : %u mW\n", cur_caps.gpu_cap);
	seq_printf(file, "emc cap     : %u kHz\n", cur_caps.emcfreq);
	seq_printf(file, "cc method   : %u\n", cap_method);

	mutex_unlock(&core_lock);
	return 0;
}

static int status_open(struct inode *inode, struct file *file)
{
	return single_open(file, status_show, inode->i_private);
}

static const struct file_operations status_fops = {
	.open = status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

#define SDC_DEBUGFS_CREATE_FILE(__name)	\
	debugfs_create_file(#__name, S_IRUGO | S_IWUSR, d, NULL, &__name##_fops)

static void init_debugfs(void)
{
	struct dentry *d;
	struct dentry *df;

	if (!sysedp_debugfs_dir)
		return;

	d = debugfs_create_dir("capping", sysedp_debugfs_dir);
	if (WARN_ON(IS_ERR_OR_NULL(d)))
		return;

	capping_debugfs_dir = d;

	df = SDC_DEBUGFS_CREATE_FILE(favor_gpu);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gpu_threshold);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(force_cpu_power);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(force_gpu);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(force_emc);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gpu_window);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gpu_high_count);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(gain);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(cap_method);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(corecaps);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(priority_bias);
	WARN_ON(!df);
	df = SDC_DEBUGFS_CREATE_FILE(status);
	WARN_ON(!df);
}
#else
static inline void init_debugfs(void) {}
#endif

static int init_clks(struct platform_device *pdev)
{
	emc_cap_clk = devm_clk_get(&pdev->dev, "cap-battery-emc");
	if (IS_ERR(emc_cap_clk))
		return -ENODEV;

	return 0;
}

static int init_corecaps(struct platform_device *pdev,
			 struct tegra_sysedp_platform_data *obj_ptr)
{
	struct device_node *np = pdev->dev.of_node;
	u32 lenp;
	int ret, n, i, idx = 0;
	const void *ptr;
	u32 *u32_ptr;

	obj_ptr->gpu_supplement =
		!!of_find_property(np, "nvidia,gpu_supp_freq", NULL);

	ptr = of_get_property(np, "nvidia,corecap", &lenp);
	if (!ptr)
		return -EINVAL;

	if (obj_ptr->gpu_supplement) {
		if (lenp % (sizeof(u32)*10)) {
			dev_err(&pdev->dev, "sysedp_dynamic_capping: corecap needs to be a multiple of 10 u32s!\n");
			return -EINVAL;
		}
		n = lenp / (sizeof(u32)*10);
	} else {
		if (lenp % (sizeof(u32)*8)) {
			dev_err(&pdev->dev, "sysedp_dynamic_capping: corecap needs to be a multiple of 8 u32s!\n");
			return -EINVAL;
		}
		n = lenp / (sizeof(u32)*8);
	}

	if (!n)
		return -EINVAL;

	obj_ptr->corecap = devm_kzalloc(&pdev->dev,
				sizeof(struct tegra_sysedp_corecap) * n,
				GFP_KERNEL);
	if (!obj_ptr->corecap)
		return -ENOMEM;

	u32_ptr = kzalloc(lenp, GFP_KERNEL);
	if (!u32_ptr)
		return -ENOMEM;
	ret = of_property_read_u32_array(np, "nvidia,corecap",
					 u32_ptr, (lenp / sizeof(u32)));
	if (ret) {
		dev_err(&pdev->dev,
			"sysedp_dynamic_capping: Fail to read corecap\n");
		kfree(u32_ptr);
		return -EINVAL;
	}

	for (idx = 0, i = 0; idx < n; idx++) {
		obj_ptr->corecap[idx].power            = u32_ptr[i];
		obj_ptr->corecap[idx].cpupri.cpu_power = u32_ptr[i+1];
		obj_ptr->corecap[idx].cpupri.gpu_cap   = u32_ptr[i+2];
		obj_ptr->corecap[idx].cpupri.emcfreq   = u32_ptr[i+3];
		obj_ptr->corecap[idx].gpupri.cpu_power = u32_ptr[i+4];
		obj_ptr->corecap[idx].gpupri.gpu_cap   = u32_ptr[i+5];
		obj_ptr->corecap[idx].gpupri.emcfreq   = u32_ptr[i+6];
		obj_ptr->corecap[idx].pthrot           = u32_ptr[i+7];
		if (obj_ptr->gpu_supplement) {
			obj_ptr->corecap[idx].cpupri.gpu_supp_freq =
								u32_ptr[i+8];
			obj_ptr->corecap[idx].gpupri.gpu_supp_freq =
								u32_ptr[i+9];
			i += 10;
		} else
			i += 8;
	}
	obj_ptr->corecap_size = n;
	kfree(u32_ptr);

	return 0;
}

static int of_sysedp_dynamic_capping_get_pdata(struct platform_device *pdev,
		struct tegra_sysedp_platform_data **pdata)
{
	struct device_node *np = pdev->dev.of_node;
	struct tegra_sysedp_platform_data *obj_ptr;
	u32 val;
	int ret;

	*pdata = NULL;

	obj_ptr = devm_kzalloc(&pdev->dev,
			       sizeof(struct tegra_sysedp_platform_data),
			       GFP_KERNEL);
	if (!obj_ptr)
		return -ENOMEM;

	ret = init_corecaps(pdev, obj_ptr);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize corecaps\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "nvidia,core_gain", &val);
	if (!ret)
		obj_ptr->core_gain = (unsigned int)val;
	else {
		dev_err(&pdev->dev, "Fail to read core_gain\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "nvidia,init_req_watts", &val);
	if (!ret)
		obj_ptr->init_req_watts = (unsigned int)val;
	else {
		dev_err(&pdev->dev, "Fail to read init_req_watts\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "nvidia,throttle_depth", &val);
	if (!ret) {
		if (val > 100) {
			dev_err(&pdev->dev,
			    "sysedp_dynamic_capping: throttle_depth > 100\n");
			return -EINVAL;
		}
		obj_ptr->pthrot_ratio = (unsigned int)val;
	} else {
		dev_err(&pdev->dev, "Fail to read throttle_depth\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(np, "nvidia,cap_method", &val);
	if (!ret)
		obj_ptr->cap_method = (unsigned int)val;
	else {
		dev_err(&pdev->dev, "Fail to read cap_method\n");
		return -EINVAL;
	}

	if (!of_property_read_bool(np, "nvidia,gpu_cap_as_mw")) {
		dev_err(&pdev->dev, "GPU capping by frequency not supported\n");
		return -EINVAL;
	}

	*pdata = obj_ptr;

	return 0;
}


static int sysedp_dynamic_capping_probe(struct platform_device *pdev)
{
	struct tegra_sysedp_corecap *cap;
	int i, ret;

	if (!tegra_cpu_edp_ready())
		return -EPROBE_DEFER;

	if (init_clks(pdev))
		return -EPROBE_DEFER;

	/* only one instance is allowed */
	if (WARN_ON(init_done)) {
		ret = -EINVAL;
		goto err;
	}

	if (pdev->dev.of_node) {
		ret = of_sysedp_dynamic_capping_get_pdata(pdev,
						&capping_data);
		if (ret)
			goto err;
	} else
		capping_data = pdev->dev.platform_data;

	if (!capping_data) {
		ret = -EINVAL;
		goto err;
	}

	INIT_DELAYED_WORK(&capping_work, capping_worker);
	pm_qos_add_request(&cpupwr_qos, PM_QOS_MAX_CPU_POWER,
			   PM_QOS_CPU_POWER_MAX_DEFAULT_VALUE);

	pm_qos_add_request(&gpupwr_qos, PM_QOS_MAX_GPU_POWER,
			   PM_QOS_GPU_POWER_MAX_DEFAULT_VALUE);

	mutex_lock(&core_lock);
	avail_power = capping_data->init_req_watts;
	cap_method = capping_data->cap_method;
	switch (cap_method) {
	case SYSEDP_CAP_METHOD_DEFAULT:
		cap_method = SYSEDP_CAP_METHOD_DIRECT;
		break;
	case SYSEDP_CAP_METHOD_DIRECT:
	case SYSEDP_CAP_METHOD_SIGNAL:
	case SYSEDP_CAP_METHOD_RELAX:
		break;
	default:
		pr_warn("%s: Unknown cap_method, %x!  Assuming direct.\n",
				__func__, cap_method);
		cap_method = SYSEDP_CAP_METHOD_DIRECT;
		break;
	}

	/* scale pthrot value in capping table */
	for (i = capping_data->corecap_size - 1; i >= 0; i--) {
		cap = &capping_data->corecap[i];
		cap->pthrot *= capping_data->pthrot_ratio;
		cap->pthrot /= 100;
	}
	_update_cur_corecap();
	_do_cap_control();
	mutex_unlock(&core_lock);

	init_debugfs();

	init_done = 1;

	/* update battery power in time */
	batmon_update_budget();

	/*
	 * Refresh sysedp dynamic capping configuration in
	 * case any consumers have already been registered.
	 */
	_sysedp_refresh();

	return 0;

err:
	devm_clk_put(&pdev->dev, emc_cap_clk);
	return ret;
}

static const struct of_device_id sysedp_dynamic_capping_of_match[] = {
	{ .compatible = "nvidia,tegra124-sysedp-dynamic-capping", },
	{ },
};
MODULE_DEVICE_TABLE(of, sysedp_dynamic_capping_of_match);

static struct platform_driver sysedp_dynamic_capping_driver = {
	.probe = sysedp_dynamic_capping_probe,
	.driver = {
		.owner = THIS_MODULE,
		.name = "sysedp_dynamic_capping",
		.of_match_table = sysedp_dynamic_capping_of_match,
	}
};

static __init int sysedp_dynamic_capping_init(void)
{
	return platform_driver_register(&sysedp_dynamic_capping_driver);
}
late_initcall(sysedp_dynamic_capping_init);
