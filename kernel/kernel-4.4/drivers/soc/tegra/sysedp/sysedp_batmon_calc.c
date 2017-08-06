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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/suspend.h>
#include <linux/debugfs.h>
#include <linux/of.h>
#include <soc/tegra/sysedp.h>
#include "sysedp_internal.h"

#define UPDATE_INTERVAL	60000 /* in ms */

static struct sysedp_batmon_calc_platform_data *pdata;
static struct delayed_work work;
static struct power_supply *psy;

int (*get_ocv)(unsigned int capacity);

static struct kobject batmon_kobj;

/*
 * ratio between user-space ESR setting and look-up-table based ESR value
 * 100 means to multiply by 1.0.  Expected values are between 0-100%
 */
static int user_esr_ratio = 100;

static int psy_get_property(enum power_supply_property psp, int *val)
{
	union power_supply_propval pv;

	if (power_supply_get_property(psy, psp, &pv))
		return -EFAULT;
	if (val)
		*val = pv.intval;
	return 0;
}

/* read battery's open-circuit-voltage in uV */
static int psy_ocv_from_chip(unsigned int capacity)
{
	int val;

	if (psy_get_property(POWER_SUPPLY_PROP_VOLTAGE_OCV, &val))
		return pdata->vsys_min;
	return val;
}

/* read battery's capacity in percents */
static int psy_capacity(void)
{
	int val;

	if (psy_get_property(POWER_SUPPLY_PROP_CAPACITY, &val))
		return 0;
	return val;
}

/* read battery's temperature in tenths of degree Celsius */
static int psy_temp(void)
{
	int val;

	if (psy_get_property(POWER_SUPPLY_PROP_TEMP, &val))
		return 250;
	return val;
}

/* read battery's max current in uA */
static int psy_imax(void)
{
	int val;

	if (psy_get_property(POWER_SUPPLY_PROP_CURRENT_MAX, &val))
		return 0;
	return val;
}

/* Given two points (x1, y1) and (x2, y2), find the y coord of x */
static int interpolate(int x, int x1, int y1, int x2, int y2)
{
	if (x1 == x2)
		return y1;
	return (y2 * (x - x1) - y1 * (x - x2)) / (x2 - x1);
}

/* bi-linearly interpolate from table */
static int bilinear_interpolate(int *array, int *xaxis, int *yaxis,
				int x_size, int y_size, int x, int y)
{
	s64 r;
	int d;
	int yi1, yi2;
	int xi1, xi2;
	int q11, q12, q21, q22;
	int x1, x2, y1, y2;

	if (x_size <= 0 || y_size <= 0)
		return 0;

	if (x_size == 1 && y_size == 1)
		return array[0];

	/*
	 * Find x1 and x2 that satisfy x1 >= x >= x2
	 * Normally, the x is within xaxis, if the x out range,
	 * it will select boundary values
	 */
	for (xi2 = 1; xi2 < x_size - 1; xi2++)
		if (x > xaxis[xi2])
			break;
	xi1 = xi2 - 1;
	xi2 = x_size > 1 ? xi2 : 0;
	x1 = xaxis[xi1];
	x2 = xaxis[xi2];

	for (yi2 = 1; yi2 < y_size - 1; yi2++)
		if (y > yaxis[yi2])
			break;
	yi1 = yi2 - 1;
	yi2 = y_size > 1 ? yi2 : 0;
	y1 = yaxis[yi1];
	y2 = yaxis[yi2];

	if (x_size == 1)
		return interpolate(y, y1, array[yi1], y2, array[yi2]);
	if (y_size == 1)
		return interpolate(x, x1, array[xi1], x2, array[xi2]);

	q11 = array[xi1 + yi1 * x_size];
	q12 = array[xi1 + yi2 * x_size];
	q21 = array[xi2 + yi1 * x_size];
	q22 = array[xi2 + yi2 * x_size];

	r = (s64)q11 * (x2 - x) * (y2 - y);
	r += (s64)q21 * (x - x1) * (y2 - y);
	r += (s64)q12 * (x2 - x) * (y - y1);
	r += (s64)q22 * (x - x1) * (y - y1);
	d = ((x2-x1)*(y2-y1));
	r = d ? div64_s64(r, d) : 0;

	return r;
}

/*
 * Estimate the battery Open-Circuit-Voltage (Vocv in uV) from the OCV
 * lookup table. This table is relating State-of-Charge (in %) to OCV.
 */
static int psy_ocv_from_lut(unsigned int capacity)
{
	struct sysedp_batmon_ocv_lut *p = NULL;
	struct sysedp_batmon_ocv_lut *q;
	int i;

	for (i = 0; i < pdata->ocv_lut_size; i++) {
		p = pdata->ocv_lut + i;
		if (p->capacity <= capacity)
			break;
	}

	if (WARN_ON(!p))
		return 0;

	if (p == pdata->ocv_lut)
		return p->ocv;

	q = p - 1;

	return interpolate(capacity, p->capacity, p->ocv, q->capacity,
			   q->ocv);
}

/*
 * Estimate Battery Impedance (Rbat in uOhm) from the ESR lookup table.
 * This table is relating State-of-Charge (in %) and Temperature
 * (in decicelsius) to Battery Impedance.
 * Resr = Rbat + Rconst
 */
static int lookup_esr(int capacity, int temp)
{
	struct sysedp_batmon_rbat_lut *lut = &pdata->rbat_lut;
	int ret = pdata->r_const;

	ret += bilinear_interpolate(lut->data, lut->temp_axis,
				    lut->capacity_axis, lut->temp_size,
				    lut->capacity_size, temp, capacity);
	return ret;
}

/*
 * Calculate Resr (in uOhm) by State-of-Charge (in %) and Temperature
 * (in decicelsius).
 * There is a ratio between user-space ESR setting and look-up-table
 * based ESR value, so calc_esr = ratio * lookup_esr.
 */
static int calc_esr(int capacity, int temp)
{
	int esr;

	esr = lookup_esr(capacity, temp);
	esr = esr * user_esr_ratio / 100;
	return esr;
}

/*
 * Calculate maximum allowed current (Imax in mA) limited by equivalent
 * series resistance (Resr in uOhm)
 */
static s64 calc_ibat_esr(s64 ocv, s64 esr)
{
	if (ocv <= pdata->vsys_min)
		return 0;
	else if (esr <= 0)
		return 0;
	else
		return div64_s64(1000 * (ocv - pdata->vsys_min), esr);
}

/* Calculate Ibat (in mA) for a given temperature (*in decicelsius). */
static int calc_ibat(int temp)
{
	struct sysedp_batmon_ibat_lut *p;
	struct sysedp_batmon_ibat_lut *q;
	int ibat, i = 0;

	p = pdata->ibat_lut;

	while ((i < (pdata->ibat_lut_size - 1)) &&
		p->ibat && p->temp > temp) {
		i++;
		p++;
	}

	if (p == pdata->ibat_lut)
		return p->ibat;

	q = p - 1;
	ibat = interpolate(temp, p->temp, p->ibat, q->temp, q->ibat);

	return ibat > 0 ? ibat : 0;
}

/*
 * Calculate Pmax (in mW) by Vocv (in uV), Ibat (in mA) and Resr (in uOhm).
 */
static s64 calc_pbat(s64 ocv, s64 ibat, s64 esr)
{
	s64 vsys;

	/* Vsys(uV) = Vocv(uV) - Ibat(mA) / 1000 * Resr(uOhm) */
	vsys = ocv - div64_s64(ibat * esr, 1000);
	/* Pmax(mW) = Vsys(uV) / 1000000 * Ibat(mA) */
	return div64_s64(vsys * ibat, 1000000);
}

static unsigned int calc_avail_budget(void)
{
	s64 ibat_max, pbat;

	if (pdata->imax_mode) {
		ibat_max = div64_s64(psy_imax(), 1000);
		if (!ibat_max)
			pr_err("ibat_max is zero\n");

		/* Pbat(mW) = Imax(mA) * Vsys_min(uV) / 1000000 */
		pbat = div64_s64(ibat_max * pdata->vsys_min, 1000000);
	} else {
		int esr, capacity, temp;
		s64 ocv;
		s64 ibat_esr;
		s64 ibat;

		capacity = psy_capacity();
		temp = psy_temp();
		ocv = get_ocv(capacity);
		esr = calc_esr(capacity, temp);

		ibat_esr = calc_ibat_esr(ocv, esr);
		ibat = calc_ibat(temp);
		ibat_max = min(ibat_esr, ibat);

		pbat = calc_pbat(ocv, ibat_max, esr);

		pr_debug("capacity : %u\n", capacity);
		pr_debug("ocv      : %lld\n", ocv);
		pr_debug("esr      : %d\n", esr);
		pr_debug("ibat_esr : %lld\n", ibat_esr);
		pr_debug("ibat     : %lld\n", ibat);
	}
	pr_debug("ibat_max : %lld\n", ibat_max);
	pr_debug("pbat     : %lld\n", pbat);

	return pbat;
}

void batmon_update_budget(void)
{
	unsigned int budget;

	if (!pdata)
		return;

	budget = calc_avail_budget();
	sysedp_set_avail_budget(budget);
}

static void batmon_update(struct work_struct *work)
{
	unsigned int update_interval;

	batmon_update_budget();

	update_interval = pdata->update_interval ?: UPDATE_INTERVAL;

	schedule_delayed_work(to_delayed_work(work),
			      msecs_to_jiffies(update_interval));
}

static void batmon_shutdown(struct platform_device *pdev)
{
	cancel_delayed_work_sync(&work);
}

static int batmon_suspend(struct platform_device *pdev, pm_message_t state)
{
	batmon_shutdown(pdev);
	return 0;
}

static int batmon_resume(struct platform_device *pdev)
{
	schedule_delayed_work(&work, 0);
	return 0;
}

static int init_ocv_reader(void)
{
	if (pdata->ocv_lut)
		get_ocv = psy_ocv_from_lut;
	else if (!psy_get_property(POWER_SUPPLY_PROP_VOLTAGE_OCV, NULL))
		get_ocv = psy_ocv_from_chip;
	else
		return -ENODEV;

	return 0;
}

#ifdef CONFIG_DEBUG_FS

static int rbat_show(struct seq_file *file, void *data)
{
	int t, c, i = 0;
	struct sysedp_batmon_rbat_lut *lut = &pdata->rbat_lut;

	seq_printf(file, " %8s", "capacity");
	for (t = 0; t < lut->temp_size; t++)
		seq_printf(file, "%8d", lut->temp_axis[t]);
	seq_puts(file, "\n");

	for (c = 0; c < lut->capacity_size; c++) {
		seq_printf(file, "%8d%%", lut->capacity_axis[c]);
		for (t = 0; t < lut->temp_size; t++)
			seq_printf(file, "%8d", lut->data[i++]);
		seq_puts(file, "\n");
	}
	return 0;
}

static int rbat_open(struct inode *inode, struct file *file)
{
	return single_open(file, rbat_show, NULL);
}

static const struct file_operations rbat_fops = {
	.open = rbat_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ibat_show(struct seq_file *file, void *data)
{
	struct sysedp_batmon_ibat_lut *lut = pdata->ibat_lut;
	int i;

	if (lut) {
		for (i = 0; i < pdata->ibat_lut_size; i++)
			seq_printf(file, "%7d %7dmA\n",
				lut[i].temp, lut[i].ibat);
	}
	return 0;
}

static int ibat_open(struct inode *inode, struct file *file)
{
	return single_open(file, ibat_show, NULL);
}

static const struct file_operations ibat_fops = {
	.open = ibat_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int ocv_show(struct seq_file *file, void *data)
{
	struct sysedp_batmon_ocv_lut *lut = pdata->ocv_lut;
	int i;

	if (lut) {
		for (i = 0; i < pdata->ocv_lut_size; i++) {
			seq_printf(file, "%7d%% %7duV\n", lut[i].capacity,
				   lut[i].ocv);
		}
	}
	return 0;
}

static int ocv_open(struct inode *inode, struct file *file)
{
	return single_open(file, ocv_show, NULL);
}

static const struct file_operations ocv_fops = {
	.open = ocv_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int imax_show(struct seq_file *file, void *data)
{
	int imax = psy_imax();

	seq_printf(file, "%d\n", imax);
	return 0;
}

static int imax_open(struct inode *inode, struct file *file)
{
	return single_open(file, imax_show, NULL);
}

static const struct file_operations imax_fops = {
	.open = imax_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void init_debugfs(void)
{
	struct dentry *dd, *df;

	if (!sysedp_debugfs_dir)
		return;

	dd = debugfs_create_dir("batmon", sysedp_debugfs_dir);
	WARN_ON(IS_ERR_OR_NULL(dd));

	df = debugfs_create_u32("vsys_min", S_IRUGO, dd, &pdata->vsys_min);
	WARN_ON(IS_ERR_OR_NULL(df));

	if (pdata->imax_mode) {
		df = debugfs_create_file("imax", S_IRUGO, dd, NULL,
					 &imax_fops);
	} else {
		df = debugfs_create_file("rbat", S_IRUGO, dd, NULL,
					 &rbat_fops);
		WARN_ON(IS_ERR_OR_NULL(df));

		df = debugfs_create_file("ibat", S_IRUGO, dd, NULL,
					 &ibat_fops);
		WARN_ON(IS_ERR_OR_NULL(df));

		df = debugfs_create_file("ocv", S_IRUGO, dd, NULL,
					 &ocv_fops);
		WARN_ON(IS_ERR_OR_NULL(df));

		df = debugfs_create_u32("r_const", S_IRUGO, dd,
					&pdata->r_const);
		WARN_ON(IS_ERR_OR_NULL(df));
	}

}
#else
static inline void init_debugfs(void) {}
#endif

static int of_batmon_cal_get_array(struct platform_device *pdev,
				char *array_name,
				bool pair,
				int *array_length,
				u32 **u32_ptr)
{
	struct device_node *np = pdev->dev.of_node;
	const void *ptr;
	u32 *_u32_ptr;
	u32 lenp;
	int n;

	if (*u32_ptr)
		*u32_ptr = NULL;

	ptr = of_get_property(np, array_name, &lenp);
	if (!ptr) {
		dev_err(&pdev->dev, "Fail to get %s\n", array_name);
		return -EINVAL;
	}

	n = lenp / sizeof(u32);
	if (!n || (pair && ((n % 2) != 0)))
		goto err;

	_u32_ptr = kcalloc(n, sizeof(u32), GFP_KERNEL);
	if (!_u32_ptr)
		return -ENOMEM;
	if (of_property_read_u32_array(np, array_name, _u32_ptr, n)) {
		dev_err(&pdev->dev, "Fail to read %s\n", array_name);
		kfree(_u32_ptr);
		return -EINVAL;
	}

	*u32_ptr = _u32_ptr;
	*array_length = n;

	return 0;
err:
	dev_err(&pdev->dev, "The length of %s array is incorrect\n",
		array_name);
	return -EINVAL;
}

static int of_batmon_calc_get_pdata(struct platform_device *pdev,
	struct sysedp_batmon_calc_platform_data **pdata)
{
	struct device_node *np = pdev->dev.of_node;
	struct sysedp_batmon_calc_platform_data *obj_ptr;
	u32 *u32_ptr;
	u32 val;
	int n;
	int ret;
	int i;

	obj_ptr = devm_kzalloc(&pdev->dev,
		sizeof(struct sysedp_batmon_calc_platform_data), GFP_KERNEL);
	if (!obj_ptr)
		return -ENOMEM;

	ret = of_property_read_u32(np, "vsys-min", &val);
	if (ret)
		dev_info(&pdev->dev, "Fail to read vsys-min\n");
	else
		obj_ptr->vsys_min = val;

	ret = of_property_read_u32(np, "update-interval", &val);
	if (!ret)
		obj_ptr->update_interval = val;

	if (of_find_property(np, "imax-mode", NULL)) {
		obj_ptr->imax_mode = true;
		*pdata = obj_ptr;
		return 0;
	}

	obj_ptr->imax_mode = false;

	ret = of_property_read_u32(np, "r-const", &val);
	if (ret)
		dev_info(&pdev->dev, "Fail to read r-const\n");
	else
		obj_ptr->r_const = val;

	/* read ocv-curve */
	ret = of_batmon_cal_get_array(pdev, "ocv-curve", true, &n, &u32_ptr);
	if (ret)
		return ret;
	obj_ptr->ocv_lut = devm_kzalloc(&pdev->dev,
		sizeof(struct sysedp_batmon_ocv_lut) * n / 2, GFP_KERNEL);
	if (!obj_ptr->ocv_lut) {
		kfree(u32_ptr);
		return -ENOMEM;
	}
	for (i = 0; i < n / 2; ++i) {
		obj_ptr->ocv_lut[i].capacity = u32_ptr[2 * i];
		obj_ptr->ocv_lut[i].ocv = u32_ptr[2 * i + 1];
	}
	obj_ptr->ocv_lut_size = n / 2;
	kfree(u32_ptr);

	/* read maxcurrent-curve */
	ret = of_batmon_cal_get_array(pdev, "maxcurrent-curve",
				      true, &n, &u32_ptr);
	if (ret)
		return ret;
	obj_ptr->ibat_lut = devm_kzalloc(&pdev->dev,
		sizeof(struct sysedp_batmon_ibat_lut) * n / 2, GFP_KERNEL);
	if (!obj_ptr->ibat_lut) {
		kfree(u32_ptr);
		return -ENOMEM;
	}
	for (i = 0; i < n / 2; ++i) {
		obj_ptr->ibat_lut[i].temp = (s32)u32_ptr[2 * i];
		obj_ptr->ibat_lut[i].ibat = u32_ptr[2 * i + 1];
	}
	obj_ptr->ibat_lut_size = n / 2;
	kfree(u32_ptr);

	/* fill the rbat look up table */
	/* read impedance-curve */
	ret = of_batmon_cal_get_array(pdev, "impedance-curve",
				      false, &n, &u32_ptr);
	if (ret)
		return ret;
	obj_ptr->rbat_lut.data = devm_kzalloc(&pdev->dev,
		sizeof(int) * n, GFP_KERNEL);
	if (!obj_ptr->rbat_lut.data) {
		kfree(u32_ptr);
		return -ENOMEM;
	}
	for (i = 0; i < n; ++i)
		obj_ptr->rbat_lut.data[i] = u32_ptr[i];
	obj_ptr->rbat_lut.data_size = n;
	kfree(u32_ptr);

	/* read temp-axis */
	ret = of_batmon_cal_get_array(pdev, "temp-axis", false, &n, &u32_ptr);
	if (ret)
		return ret;
	obj_ptr->rbat_lut.temp_axis = devm_kzalloc(&pdev->dev,
		sizeof(int) * n, GFP_KERNEL);
	if (!obj_ptr->rbat_lut.temp_axis) {
		kfree(u32_ptr);
		return -ENOMEM;
	}
	for (i = 0; i < n; ++i)
		obj_ptr->rbat_lut.temp_axis[i] = (s32)u32_ptr[i];
	obj_ptr->rbat_lut.temp_size = n;
	kfree(u32_ptr);

	/* read capacity-axis */
	ret = of_batmon_cal_get_array(pdev, "capacity-axis",
				      false, &n, &u32_ptr);
	if (ret)
		return ret;
	obj_ptr->rbat_lut.capacity_axis = devm_kzalloc(&pdev->dev,
		sizeof(int) * n, GFP_KERNEL);
	if (!obj_ptr->rbat_lut.capacity_axis) {
		kfree(u32_ptr);
		return -ENOMEM;
	}
	for (i = 0; i < n; ++i)
		obj_ptr->rbat_lut.capacity_axis[i] = u32_ptr[i];
	obj_ptr->rbat_lut.capacity_size = n;
	kfree(u32_ptr);

	*pdata = obj_ptr;
	return 0;
}

struct batmon_attribute {
	struct attribute attr;
	ssize_t (*show)(char *buf);
	ssize_t (*store)(const char *buf, size_t count);
};

/* Show current Resr value in mOhm */
static ssize_t esr_show(char *s)
{
	int capacity, temp, esr;

	capacity = psy_capacity();
	temp = psy_temp();
	esr = calc_esr(capacity, temp);
	esr /= 1000; /* to mOhm */

	return sprintf(s, "%d\n", esr);
}

/*
 * Allow an user space process to update current Resr by
 * impedance(in mOhm)-capacity(in %)-temperature(in decicelsius)
 * Compute the ratio between internal table based Resr estimate
 * and value set from user space. This ratio will be then
 * used to scale all future table based ESR estimations
 */
static ssize_t esr_store(const char *s, size_t count)
{
	int mohm, capacity, temp;
	int lut_esr;
	int n;

	n = sscanf(s, "%d %d %d", &mohm, &capacity, &temp);
	if (n != 1 && n != 3)
		return -EINVAL;
	if (mohm <= 0)
		return -EINVAL;

	if (n != 3) {
		capacity = psy_capacity();
		temp = psy_temp();
	} else {
		if (capacity < 0 || capacity > 100)
			return -EINVAL;
	}

	lut_esr = lookup_esr(capacity, temp);
	if (!lut_esr)
		return -EINVAL;

	/*
	 * Calculate ratio(in %) by user_esr (in mOhm) that is set from
	 * user space and lut_esr which is estimate from internal table.
	 *
	 * ratio (in %) = user_esr (in mOhm) * 1000 / lut_esr (in uOhm) * 100
	 */
	user_esr_ratio = DIV_ROUND_CLOSEST(100 * 1000 * mohm, lut_esr);

	cancel_delayed_work_sync(&work);
	schedule_delayed_work(&work, 0);

	return count;
}

static struct batmon_attribute attr_esr =
	__ATTR(esr, 0660, esr_show, esr_store);

static struct attribute *batmon_attrs[] = {
	&attr_esr.attr,
	NULL
};

static ssize_t batmon_attr_show(struct kobject *kobj,
				struct attribute *_attr, char *buf)
{
	ssize_t r = -EINVAL;
	struct batmon_attribute *attr;

	attr = container_of(_attr, struct batmon_attribute, attr);
	if (attr && attr->show)
		r = attr->show(buf);
	return r;
}

static ssize_t batmon_attr_store(struct kobject *kobj, struct attribute *_attr,
				 const char *buf, size_t count)
{
	ssize_t r = -EINVAL;
	struct batmon_attribute *attr;

	attr = container_of(_attr, struct batmon_attribute, attr);
	if (attr && attr->store)
		r = attr->store(buf, count);
	return r;
}

static const struct sysfs_ops batmon_sysfs_ops = {
	.show = batmon_attr_show,
	.store = batmon_attr_store,
};

static struct kobj_type ktype_batmon = {
	.sysfs_ops = &batmon_sysfs_ops,
	.default_attrs = batmon_attrs,
};

static int init_sysfs(void)
{
	if (!sysedp_kobj.ktype)
		return -EINVAL;
	return kobject_init_and_add(&batmon_kobj, &ktype_batmon,
				    &sysedp_kobj, "batmon");
}

static int batmon_probe(struct platform_device *pdev)
{
	struct sysedp_batmon_rbat_lut *rbat;
	int ret, i;

	if (pdev->dev.of_node) {
		ret = of_batmon_calc_get_pdata(pdev, &pdata);
		if (ret)
			return ret;
	} else {
		pdata = pdev->dev.platform_data;
	}

	if (!pdata)
		return -EINVAL;

	psy = power_supply_get_by_phandle(pdev->dev.of_node, "power-supply");
	if (!psy)
		return -EPROBE_DEFER;
	else if (IS_ERR(psy))
		return PTR_ERR(psy);

	/* validate member of pdata->rbat_lut table */
	if (!pdata->imax_mode) {
		rbat = &pdata->rbat_lut;
		for (i = 1; i < rbat->temp_size; i++)
			if (rbat->temp_axis[i] >= rbat->temp_axis[i-1])
				return -EINVAL;
		for (i = 1; i < rbat->capacity_size; i++)
			if (rbat->capacity_axis[i] >= rbat->capacity_axis[i-1])
				return -EINVAL;
		if (rbat->capacity_size * rbat->temp_size != rbat->data_size)
			return -EINVAL;

		if (init_ocv_reader())
			return -EFAULT;

		init_sysfs();
	}


	INIT_DEFERRABLE_WORK(&work, batmon_update);
	schedule_delayed_work(&work, 0);

	init_debugfs();

	return 0;
}

static const struct of_device_id batmon_calc_of_match[] = {
	{ .compatible = "nvidia,tegra124-sysedp-batmon-calc", },
	{ },
};
MODULE_DEVICE_TABLE(of, batmon_calc_of_match);

static struct platform_driver batmon_driver = {
	.probe = batmon_probe,
	.shutdown = batmon_shutdown,
	.suspend = batmon_suspend,
	.resume = batmon_resume,
	.driver = {
		.name = "sysedp_batmon_calc",
		.owner = THIS_MODULE,
		.of_match_table = batmon_calc_of_match,
	}
};

static __init int batmon_init(void)
{
	return platform_driver_register(&batmon_driver);
}
late_initcall(batmon_init);
