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

#ifndef _LINUX_SYSEDP_H
#define _LINUX_SYSEDP_H

#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/slab.h>

#define SYSEDP_NAME_LEN 32

/*
 * @name: name of consumer
 * @states: EDP state array holding the max peak power for each state.
 * @ocpeaks: array holding peak power values for each state when hardware
 *           OC signal is asserted for the consumer
 * @num_states: length of the above arrays
 * @state: current power state of sysedp consumer
 */
struct sysedp_consumer {
	char name[SYSEDP_NAME_LEN];
	unsigned int *states;
	unsigned int *ocpeaks;
	unsigned int num_states;

	unsigned int state;

	/* internal */
	struct list_head link;
	struct kobject kobj;

#ifdef CONFIG_DEBUG_FS
	/* public */
	struct dentry *dentry;
#endif
};

struct sysedp_consumer_data {
	struct device_node *dn;
	unsigned int *states;
	unsigned int num_states;
	bool always_on;
};

struct sysedp_platform_data {
	struct sysedp_consumer_data *consumer_data;
	unsigned int consumer_data_size;
	int margin;
	int min_budget;
	unsigned int avail_budget;
};

#define SYSEDP_CONSUMER_DATA(_name, _states)	  \
	{					  \
		.name = _name,			  \
		.states = _states,		  \
		.num_states = ARRAY_SIZE(_states) \
	}

/*
 * Temperature -> IBAT LUT
 * Should be descending wrt temp
 * { ..., .ibat = 0 } must be the last entry
 */
struct sysedp_batmon_ibat_lut {
	int temp;
	unsigned int ibat;
};

/*
 * Battery ESR look-up table (2 dimensional)
 * data -> battery impedance as function of capacity and temp (flat array)
 * temp_axis -> temperature indexes, must be in descending order
 * capacity_axis -> battery capacity indexes, must be in descending order
 * data_size -> size of data
 * temp_size -> size of temp_axis
 * capacity_size -> size of capacity_axis
 */
struct sysedp_batmon_rbat_lut {
	int *data;
	int *temp_axis;
	int *capacity_axis;
	int data_size;
	int temp_size;
	int capacity_size;
};

/*
 * Capacity -> OCV LUT
 * Should be descending wrt capacity
 * { .capacity = 0, ... } must be the last entry
 * @capacity: battery capacity in percents
 * @ocv: OCV in uV
 */
struct sysedp_batmon_ocv_lut {
	unsigned int capacity;
	unsigned int ocv;
};

/* Battery monitor data */
struct sysedp_batmon_calc_platform_data {
	bool imax_mode;
	unsigned int r_const;
	unsigned int vsys_min;
	struct sysedp_batmon_ibat_lut *ibat_lut;
	unsigned int ibat_lut_size;
	struct sysedp_batmon_ocv_lut *ocv_lut;
	struct sysedp_batmon_rbat_lut rbat_lut;
	unsigned int ocv_lut_size;
	unsigned int update_interval;
};

/* Sysedp reactive capping
 * @max_capping_mw: maximum capping in mW
 * @step_alarm_mw: amount of mW to cap on each interrupt
 * @step_relax_mw: amount ow mW to relax after relax_ms
 * @relax_ms: back-off period to relax capping (in ms)
 */
struct sysedp_reactive_capping_platform_data {
	int max_capping_mw;
	int step_alarm_mw;
	int step_relax_mw;
	int relax_ms;
	int irq;
	int irq_flags;
	struct sysedp_consumer sysedpc;

	/* internal */
	int cur_capping_mw;
	struct mutex mutex;
	struct delayed_work work;
};

#ifdef CONFIG_TEGRA_SYS_EDP
void sysedp_set_state(struct sysedp_consumer *, unsigned int);
void sysedp_set_state_by_name(const char *, unsigned int);
unsigned int sysedp_get_state(struct sysedp_consumer *);
struct sysedp_consumer *sysedp_create_consumer(struct device_node *,
						const char *);
int sysedp_register_consumer(struct sysedp_consumer *);
void sysedp_unregister_consumer(struct sysedp_consumer *);
struct sysedp_consumer *sysedp_unregister_consumer_by_name(char *);
void sysedp_free_consumer(struct sysedp_consumer *);
#else
static inline void
sysedp_set_state(struct sysedp_consumer *c, unsigned int i)
{ return; }
static inline void
sysedp_set_state_by_name(const char *c, unsigned int i)
{ return; }
static inline unsigned int
sysedp_get_state(struct sysedp_consumer *c)
{ return 0; }
static inline struct sysedp_consumer *
sysedp_create_consumer(struct device_node *dn, const char *s)
{ return NULL; }
static inline int
sysedp_register_consumer(struct sysedp_consumer *c)
{ return -ENODEV; }
static inline void
sysedp_unregister_consumer(struct sysedp_consumer *c)
{ return; }
static inline struct sysedp_consumer *
sysedp_unregister_consumer_by_name(char *n)
{ return NULL; }
static inline void
sysedp_free_consumer(struct sysedp_consumer *c)
{ return; }
static inline void
#endif

#endif
