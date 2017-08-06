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
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <soc/tegra/sysedp.h>
#define CREATE_TRACE_POINTS
#include <trace/events/sysedp.h>

#include "sysedp_internal.h"

#define DEFAULT_AVAIL_BUDGET	100000	/* in mW */

/* Locked when updating limit or avail_budget */
DEFINE_MUTEX(sysedp_lock);
LIST_HEAD(registered_consumers);
static struct sysedp_platform_data *pdata;
unsigned int avail_budget = DEFAULT_AVAIL_BUDGET;
int margin;
int min_budget;

void sysedp_set_avail_budget(unsigned int power)
{
	mutex_lock(&sysedp_lock);
	if ((avail_budget != power) && (sysedp_dynamic_cap_ready())) {
		trace_sysedp_set_avail_budget(avail_budget, power);
		avail_budget = power;
		_sysedp_refresh();
	}
	mutex_unlock(&sysedp_lock);
}

void _sysedp_refresh(void)
{
	struct sysedp_consumer *p;
	int limit; /* Amount of power available when OC=1*/
	int oc_relax; /* Additional power available when OC=0 */
	int pmax_sum = 0; /* sum of peak values (OC=1) */
	int pthres_sum = 0; /* sum of peak values (OC=0) */

	list_for_each_entry(p, &registered_consumers, link) {
		pmax_sum += _cur_oclevel(p);
		pthres_sum += _cur_level(p);
	}

	limit = (int)avail_budget - pmax_sum - margin;
	limit = limit >= min_budget ? limit : min_budget;
	oc_relax = pmax_sum - pthres_sum;
	oc_relax = oc_relax >= 0 ? oc_relax : 0;

	sysedp_set_dynamic_cap((unsigned int)limit, (unsigned int)oc_relax);
}

static struct sysedp_consumer *_sysedp_get_consumer(const char *name)
{
	struct sysedp_consumer *p;

	list_for_each_entry(p, &registered_consumers, link)
		if (!strncmp(p->name, name, SYSEDP_NAME_LEN))
			return p;

	return NULL;
}

int sysedp_register_consumer(struct sysedp_consumer *consumer)
{
	int r;

	if (!consumer)
		return -EINVAL;

	mutex_lock(&sysedp_lock);
	if (_sysedp_get_consumer(consumer->name)) {
		r = -EEXIST;
		goto err;
	}

	r = sysedp_consumer_add_kobject(consumer);
	if (r)
		goto err;

	list_add_tail(&consumer->link, &registered_consumers);
	_sysedp_refresh();
	mutex_unlock(&sysedp_lock);
	return 0;

err:
	mutex_unlock(&sysedp_lock);
	return r;
}
EXPORT_SYMBOL(sysedp_register_consumer);

void sysedp_unregister_consumer(struct sysedp_consumer *consumer)
{
	if (!consumer)
		return;

	mutex_lock(&sysedp_lock);
	list_del(&consumer->link);
	_sysedp_refresh();
	mutex_unlock(&sysedp_lock);
	sysedp_consumer_remove_kobject(consumer);
}
EXPORT_SYMBOL(sysedp_unregister_consumer);

struct sysedp_consumer *sysedp_unregister_consumer_by_name(char *name)
{
	struct sysedp_consumer *consumer;

	if (!name)
		return NULL;

	mutex_lock(&sysedp_lock);
	consumer = _sysedp_get_consumer(name);
	if (!consumer) {
		mutex_unlock(&sysedp_lock);
		return NULL;
	}

	list_del(&consumer->link);
	_sysedp_refresh();
	mutex_unlock(&sysedp_lock);
	sysedp_consumer_remove_kobject(consumer);
	return consumer;
}
EXPORT_SYMBOL(sysedp_unregister_consumer_by_name);

void sysedp_free_consumer(struct sysedp_consumer *consumer)
{
	if (consumer) {
		sysedp_unregister_consumer(consumer);
		kfree(consumer);
	}
}
EXPORT_SYMBOL(sysedp_free_consumer);

static struct sysedp_consumer_data *sysedp_find_consumer_data(
						struct device_node *dn)
{
	unsigned int i;

	if (!pdata || !pdata->consumer_data)
		return NULL;

	for (i = 0; i < pdata->consumer_data_size; i++)
		if (pdata->consumer_data[i].dn == dn)
			return &pdata->consumer_data[i];

	pr_err("%s: Failed to find consumer data for %s\n", __func__, dn->name);

	return NULL;
}

struct sysedp_consumer *sysedp_create_consumer(struct device_node *dn,
					       const char *consumername)
{
	struct sysedp_consumer *consumer;
	struct sysedp_consumer_data *match;

	match = sysedp_find_consumer_data(dn);
	if (!match)
		return NULL;

	consumer = kzalloc(sizeof(*consumer), GFP_KERNEL);
	if (!consumer)
		return NULL;

	strlcpy(consumer->name, consumername, SYSEDP_NAME_LEN);
	consumer->states = match->states;
	consumer->num_states = match->num_states;

	if (sysedp_register_consumer(consumer)) {
		pr_err("%s: Failed to register consumer for %s\n", __func__,
		       dn->name);
		kfree(consumer);
		return NULL;
	}

	return consumer;
}
EXPORT_SYMBOL(sysedp_create_consumer);

static void _sysedp_set_state(struct sysedp_consumer *consumer,
			      unsigned int new_state)
{
	new_state = clamp_t(unsigned int, new_state, 0,
			    consumer->num_states - 1);
	if (consumer->state != new_state) {
		trace_sysedp_change_state(consumer->name, consumer->state,
					  new_state);
		consumer->state = new_state;
		_sysedp_refresh();
	}
}

void sysedp_set_state(struct sysedp_consumer *consumer, unsigned int new_state)
{
	if (!consumer)
		return;

	mutex_lock(&sysedp_lock);
	_sysedp_set_state(consumer, new_state);
	mutex_unlock(&sysedp_lock);
}
EXPORT_SYMBOL(sysedp_set_state);

void sysedp_set_state_by_name(const char *name, unsigned int new_state)
{
	struct sysedp_consumer *consumer = NULL;

	if (!name)
		return;

	mutex_lock(&sysedp_lock);
	consumer = _sysedp_get_consumer(name);
	if (consumer)
		_sysedp_set_state(consumer, new_state);
	mutex_unlock(&sysedp_lock);
}
EXPORT_SYMBOL(sysedp_set_state_by_name);

unsigned int sysedp_get_state(struct sysedp_consumer *consumer)
{
	unsigned int state;

	if (!consumer)
		return 0;

	mutex_lock(&sysedp_lock);
	state = consumer->state;
	mutex_unlock(&sysedp_lock);

	return state;
}
EXPORT_SYMBOL(sysedp_get_state);

static int of_sysedp_get_pdata(struct platform_device *pdev,
		      struct sysedp_platform_data **pdata)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *np_consumers, *child;
	struct sysedp_platform_data *obj_ptr;
	int i, n, ret, idx = 0;
	u32 val;

	obj_ptr = devm_kzalloc(&pdev->dev, sizeof(struct sysedp_platform_data),
		GFP_KERNEL);
	if (!obj_ptr)
		return -ENOMEM;

	np_consumers = of_get_child_by_name(np, "consumers");
	if (!np_consumers) {
		obj_ptr->consumer_data_size = 0;
		obj_ptr->consumer_data = NULL;
		dev_dbg(&pdev->dev, "Missing consumers group\n");
		goto no_consumer;
	}

	obj_ptr->consumer_data_size = of_get_child_count(np_consumers);
	if (!obj_ptr->consumer_data_size) {
		obj_ptr->consumer_data_size = 0;
		obj_ptr->consumer_data = NULL;
		dev_err(&pdev->dev, "Reading consumers group failed\n");
		goto no_consumer;
	}

	obj_ptr->consumer_data = devm_kzalloc(&pdev->dev,
		sizeof(struct sysedp_consumer_data) *
		obj_ptr->consumer_data_size, GFP_KERNEL);
	if (!obj_ptr->consumer_data)
		return -ENOMEM;

	for_each_child_of_node(np_consumers, child) {
		u32 lenp;
		u32 *u32_ptr;
		const void *ptr;
		const __be32 *prop;
		struct device_node *dn;
		struct device *dev = &pdev->dev;

		/* get consumer name */
		prop = of_get_property(child, "nvidia,consumer", NULL);
		if (!prop) {
			dev_warn(dev, "%s Missing consumer node, skip it\n",
				child->name);
			continue;
		}

		dn = of_find_node_by_phandle(be32_to_cpup(prop));
		if (!dn) {
			dev_warn(dev, "%s Can't parse consumer node, skip it\n",
				child->name);
			continue;
		}
		obj_ptr->consumer_data[idx].dn = dn;

		/* get consumer states */
		ptr = of_get_property(child, "nvidia,states", &lenp);
		if (!ptr) {
			dev_warn(dev, "%s Can't get consumer states, skip it\n",
				child->name);
			continue;
		}
		n = lenp / sizeof(u32);
		if (!n) {
			dev_warn(dev, "%s Can't parse consumer states, skip it\n",
				child->name);
			continue;
		}
		obj_ptr->consumer_data[idx].states = devm_kzalloc(&pdev->dev,
			sizeof(unsigned int) * n, GFP_KERNEL);
		if (!obj_ptr->consumer_data[idx].states)
			return -ENOMEM;
		u32_ptr = kcalloc(n, sizeof(u32), GFP_KERNEL);
		if (!u32_ptr)
			return -ENOMEM;
		ret = of_property_read_u32_array(child, "nvidia,states",
						 u32_ptr, n);
		if (ret) {
			dev_warn(dev, "%s Failed to read consumer states, skip it\n",
				child->name);
			kfree(u32_ptr);
			continue;
		}
		for (i = 0; i < n; ++i)
			obj_ptr->consumer_data[idx].states[i] = u32_ptr[i];
		obj_ptr->consumer_data[idx].num_states = n;
		kfree(u32_ptr);

		if (of_find_property(child, "nvidia,consumer-always-on", NULL))
			obj_ptr->consumer_data[idx].always_on = true;
		else
			obj_ptr->consumer_data[idx].always_on = false;

		++idx;
	}

no_consumer:

	ret = of_property_read_u32(np, "nvidia,margin", &val);
	if (!ret)
		obj_ptr->margin = (s32)val;

	ret = of_property_read_u32(np, "nvidia,min_budget", &val);
	if (!ret)
		obj_ptr->min_budget = (s32)val;

	ret = of_property_read_u32(np, "nvidia,initial_budget", &val);
	if (!ret)
		obj_ptr->avail_budget = val;
	else
		obj_ptr->avail_budget = DEFAULT_AVAIL_BUDGET;

	*pdata = obj_ptr;

	return 0;
}

static int sysedp_probe(struct platform_device *pdev)
{
	int ret, i;

	if (pdev->dev.of_node) {
		ret = of_sysedp_get_pdata(pdev, &pdata);
		if (ret)
			return ret;
	} else
		pdata = pdev->dev.platform_data;
	if (!pdata)
		return -EINVAL;

	margin = pdata->margin;
	min_budget = (pdata->min_budget >= 0) ? pdata->min_budget : 0;
	avail_budget = pdata->avail_budget;
	sysedp_init_sysfs();
	sysedp_init_debugfs();

	for (i = 0; i < pdata->consumer_data_size; i++) {
		if (pdata->consumer_data[i].always_on) {
			struct sysedp_consumer *sysedpc;
			struct device_node *dn = pdata->consumer_data[i].dn;

			sysedpc = sysedp_create_consumer(dn, dn->name);
			if (sysedpc)
				sysedpc->state =
					pdata->consumer_data[i].num_states - 1;
		}
	}

	return 0;
}

static const struct of_device_id sysedp_of_match[] = {
	{ .compatible = "nvidia,tegra124-sysedp", },
	{ },
};
MODULE_DEVICE_TABLE(of, sysedp_of_match);

static struct platform_driver sysedp_driver = {
	.probe = sysedp_probe,
	.driver = {
		.owner = THIS_MODULE,
		.name = "sysedp",
		.of_match_table = sysedp_of_match,
	}
};

static __init int sysedp_init(void)
{
	return platform_driver_register(&sysedp_driver);
}
pure_initcall(sysedp_init);
