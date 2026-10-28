// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum CPU cooling devices ("sprd,cluster-cooling"), port of the vendor
 * 4.14 sprd_cpu_device.c + sprd_cpu_cooling.c to Android GKI 6.12.
 *
 * What the vendor code did, and what is kept here:
 *  - one cooling device per CPU cluster, created from the DT nodes under
 *    /cooling-devices (numbered through the "cooling-device" aliases);
 *    cooling state N = the Nth highest frequency of the cluster's cpufreq
 *    table (state 0 = unrestricted)                                    [kept]
 *  - a leakage/dynamic power model (DT coefficients) behind the
 *    get_requested_power/state2power/power2state ops for the IPA governor
 *                                                  [kept, 6.12 op signatures]
 *  - frequency clipping: vendor hooked a cpufreq policy notifier, 6.12 uses a
 *    FREQ_QOS_MAX request on the cluster's policy                    [changed]
 *  - sprd_cpufreq_update_opp(): the vendor power_allocator called it on every
 *    governor pass to switch the temperature dependent OPP table; here a
 *    delayed work does it                                              [kept]
 *  - CPU hotplug / core isolation (pm_qos HOTPLUG class, core_ctl idle
 *    injection): NOT ported. The pm_qos classes are gone and sprd_core_ctl in
 *    this port only keeps a mask, it does not isolate CPUs.
 *
 * GKI has no power_allocator governor (CONFIG_THERMAL_GOV_POWER_ALLOCATOR is
 * off), so in practice the zones use step_wise and only the frequency clipping
 * is exercised; the power ops are there for a power_allocator build.
 */
#define pr_fmt(fmt) "sprd_cpu_cooling: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm_opp.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/topology.h>
#include <linux/workqueue.h>

/* exported by sprd_hw_cpufreq (temperature dependent OPP table switch) */
extern unsigned int sprd_cpufreq_update_opp(int cpu, int temp_now);

#define MAX_CLUSTERS	4
#define MAX_SENSORS	8

static char *zone = "soc-thmzone";
module_param(zone, charp, 0444);
MODULE_PARM_DESC(zone, "thermal zone that drives the cooling devices (temperature source)");

static bool opp_update = true;
module_param(opp_update, bool, 0644);
MODULE_PARM_DESC(opp_update, "call sprd_cpufreq_update_opp() with the zone temperature");

static unsigned int opp_update_ms = 1000;
module_param(opp_update_ms, uint, 0644);
MODULE_PARM_DESC(opp_update_ms, "period of the temperature dependent OPP update");

/* Tscale = aT^3 + bT^2 + cT + d ; Vscale = aV^3 + bV^2 + cV + d */
struct scale_coeff {
	int a, b, c, d;
};

/* Pdyn = dynperghz * freq * (V/Vbase)^2 */
struct dyn_coeff {
	int dynperghz;
	int freq;
	int voltage_base;
};

struct power_model {
	int leak_core_base;
	int leak_cluster_base;
	struct scale_coeff core_temp, core_volt, cluster_temp, cluster_volt;
	struct dyn_coeff core, cluster;
	u32 hotplug_period;
	u32 min_cpufreq;
	u32 min_cpunum;
	u32 resistance_ja;
};

struct power_table {
	unsigned int frequency;	/* kHz */
	u32 power;		/* mW */
};

struct sprd_cpu_cdev {
	struct list_head node;
	int cluster_id;
	struct device_node *np;
	cpumask_t cpus;
	/* every CPU may have its own cpufreq policy: one MAX request per policy */
	struct cpufreq_policy **pol;
	struct freq_qos_request *req;
	unsigned int npol;
	struct cpufreq_policy *policy;	/* policy of the first CPU (frequency table) */
	struct thermal_cooling_device *cdev;

	struct power_model pm;

	struct mutex lock;
	unsigned int *freq_table;	/* kHz, descending */
	unsigned int max_level;		/* index of the last valid entry */
	unsigned int curr_max_freq;	/* highest frequency the OPP table allows */
	unsigned long state;
	unsigned int clipped_freq;

	struct power_table *dyn_power_table;
	struct power_table *l2_power_table;
	unsigned int dyn_entries;

	u32 last_load;
	u64 *time_in_idle;
	u64 *time_in_idle_ts;

	struct delayed_work opp_work;
};

static LIST_HEAD(cdev_list);
static DEFINE_MUTEX(cdev_list_lock);
static struct delayed_work probe_work;
static int probe_tries;

/*
 * On 6.12 topology_physical_package_id() is the NUMA/package id (0 for all CPUs
 * of this SoC); the cpu-map clusters show up as topology_cluster_id().
 */
static int cpu_cluster(int cpu)
{
	int id = topology_cluster_id(cpu);

	return id >= 0 ? id : topology_physical_package_id(cpu);
}

/* ---------------------------------------------------------------- power */

static u64 dyn_power(const struct dyn_coeff *c, unsigned int freq_mhz,
		     unsigned int volt_mv)
{
	u64 power;

	if (!c->voltage_base)
		return 0;
	power = (u64)c->dynperghz * freq_mhz * volt_mv * volt_mv;
	do_div(power, (u32)c->voltage_base * c->voltage_base);
	do_div(power, 10000);
	return power;
}

/* returns Tscale * 1000 (temperature in degrees C); clamped at 0 */
static u64 temp_scale(const struct scale_coeff *c, long t)
{
	s64 v = (s64)c->a * t * t * t + (s64)c->b * t * t + (s64)c->c * t + c->d;

	if (v < 0)
		return 0;
	return div_u64(v, 10000);
}

/* returns Vscale * 1000 (voltage in uV); clamped at 0 */
static u64 volt_scale(const struct scale_coeff *c, unsigned long u_volt)
{
	long mv = u_volt / 1000;
	s64 cubic = (s64)c->a * ((mv * mv * mv) / 1000000);
	s64 square = (s64)c->b * ((mv * mv) / 1000);
	s64 common = (s64)c->c * mv;
	s64 data = (s64)c->d * 1000;
	s64 v = cubic + square + common + data;

	if (v < 0)
		return 0;
	return div_u64(v, 100);
}

/* leakage of the online cores (+ cache) at u_volt/temperature; vendor math */
static u32 static_power(struct sprd_cpu_cdev *c, unsigned int nr_cpus,
			unsigned long u_volt, int temp_mc)
{
	u64 core_t = temp_scale(&c->pm.core_temp, temp_mc / 1000);
	u64 core_v = volt_scale(&c->pm.core_volt, u_volt);
	u64 tmp;
	u32 power;

	if (core_t * core_v > 1000000) {
		tmp = div_u64(core_t * core_v, 1000000);
		power = div_u64((u64)nr_cpus * c->pm.leak_core_base * tmp, 100);
	} else {
		tmp = div_u64(core_t * core_v, 100000);
		power = div_u64((u64)nr_cpus * c->pm.leak_core_base * tmp, 1000);
	}

	if (nr_cpus) {
		u64 cl_t = temp_scale(&c->pm.cluster_temp, temp_mc / 1000);
		u64 cl_v = volt_scale(&c->pm.cluster_volt, u_volt);

		if (cl_t * cl_v > 1000000) {
			tmp = div_u64(cl_t * cl_v, 1000000);
			power += div_u64((u64)c->pm.leak_cluster_base * tmp, 100);
		} else {
			tmp = div_u64(cl_t * cl_v, 100000);
			power += div_u64((u64)c->pm.leak_cluster_base * tmp, 1000);
		}
	}
	return power;
}

static int zone_temp(int *temp)
{
	struct thermal_zone_device *tz = thermal_zone_get_zone_by_name(zone);

	if (IS_ERR(tz))
		return PTR_ERR(tz);
	return thermal_zone_get_temp(tz, temp);
}

static int opp_voltage(struct sprd_cpu_cdev *c, unsigned long freq_khz,
		       unsigned long *uv)
{
	struct device *dev = get_cpu_device(cpumask_first(&c->cpus));
	unsigned long hz = freq_khz * 1000;
	struct dev_pm_opp *opp;

	if (!dev)
		return -ENODEV;
	opp = dev_pm_opp_find_freq_exact(dev, hz, true);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	*uv = dev_pm_opp_get_voltage(opp);
	dev_pm_opp_put(opp);
	return *uv ? 0 : -EINVAL;
}

static int get_static(struct sprd_cpu_cdev *c, unsigned long freq_khz,
		      unsigned int nr_cpus, u32 *power)
{
	unsigned long uv;
	int temp, ret;

	if (!nr_cpus) {
		*power = 0;
		return 0;
	}
	ret = opp_voltage(c, freq_khz, &uv);
	if (ret)
		return ret;
	ret = zone_temp(&temp);
	if (ret)
		return ret;
	*power = static_power(c, nr_cpus, uv, temp);
	return 0;
}

static u32 table_power(const struct power_table *pt, unsigned int n, u32 freq)
{
	unsigned int i;

	for (i = 1; i < n; i++)
		if (freq < pt[i].frequency)
			break;
	return pt[i - 1].power;
}

static u32 table_freq(const struct power_table *pt, unsigned int n, u32 power)
{
	unsigned int i;

	for (i = 1; i < n; i++)
		if (power < pt[i].power)
			break;
	return pt[i - 1].frequency;
}

static int build_power_tables(struct sprd_cpu_cdev *c)
{
	struct device *dev = get_cpu_device(cpumask_first(&c->cpus));
	struct power_table *pt, *l2;
	struct dev_pm_opp *opp;
	unsigned long freq = 0;
	int n, i = 0;

	if (!dev)
		return -ENODEV;
	n = dev_pm_opp_get_opp_count(dev);
	if (n <= 0)
		return n ? n : -EINVAL;

	pt = kcalloc(n, sizeof(*pt), GFP_KERNEL);
	l2 = kcalloc(n, sizeof(*l2), GFP_KERNEL);
	if (!pt || !l2) {
		kfree(pt);
		kfree(l2);
		return -ENOMEM;
	}

	while (i < n && !IS_ERR(opp = dev_pm_opp_find_freq_ceil(dev, &freq))) {
		unsigned int mhz = freq / 1000000;
		unsigned int mv = dev_pm_opp_get_voltage(opp) / 1000;

		dev_pm_opp_put(opp);
		/* frequencies above the temperature limited maximum are skipped */
		if (freq / 1000 <= c->curr_max_freq) {
			pt[i].frequency = freq / 1000;
			pt[i].power = dyn_power(&c->pm.core, mhz, mv);
			l2[i].frequency = freq / 1000;
			l2[i].power = dyn_power(&c->pm.cluster, mhz, mv);
			i++;
		}
		freq++;
	}
	if (!i) {
		kfree(pt);
		kfree(l2);
		return -EINVAL;
	}

	kfree(c->dyn_power_table);
	kfree(c->l2_power_table);
	c->dyn_power_table = pt;
	c->l2_power_table = l2;
	c->dyn_entries = i;
	return 0;
}

/* ------------------------------------------------------------ freq table */

static unsigned int find_next_max(struct cpufreq_frequency_table *table,
				  unsigned int prev_max)
{
	struct cpufreq_frequency_table *pos;
	unsigned int max = 0;

	cpufreq_for_each_valid_entry(pos, table)
		if (pos->frequency > max && pos->frequency < prev_max)
			max = pos->frequency;
	return max;
}

/* rebuild the descending frequency list limited to curr_max_freq */
static int build_freq_table(struct sprd_cpu_cdev *c)
{
	struct cpufreq_frequency_table *table = c->policy->freq_table;
	struct cpufreq_frequency_table *pos;
	unsigned int n = 0, i, freq = UINT_MAX;
	unsigned int *ft;

	if (!table)
		return -ENODEV;
	cpufreq_for_each_valid_entry(pos, table)
		n++;
	if (!n)
		return -EINVAL;

	ft = kcalloc(n, sizeof(*ft), GFP_KERNEL);
	if (!ft)
		return -ENOMEM;

	for (i = 0, n = 0; ; i++) {
		freq = find_next_max(table, freq);
		if (!freq)
			break;
		if (freq > c->curr_max_freq)
			continue;
		ft[n++] = freq;
	}
	if (!n) {
		kfree(ft);
		return -EINVAL;
	}

	kfree(c->freq_table);
	c->freq_table = ft;
	c->max_level = n - 1;
	return 0;
}

/* ----------------------------------------------------- cooling device ops */

static int cdev_get_max_state(struct thermal_cooling_device *cdev,
			      unsigned long *state)
{
	struct sprd_cpu_cdev *c = cdev->devdata;

	mutex_lock(&c->lock);
	*state = c->max_level;
	mutex_unlock(&c->lock);
	return 0;
}

static int cdev_get_cur_state(struct thermal_cooling_device *cdev,
			      unsigned long *state)
{
	struct sprd_cpu_cdev *c = cdev->devdata;

	mutex_lock(&c->lock);
	*state = c->state;
	mutex_unlock(&c->lock);
	return 0;
}

static int __set_state(struct sprd_cpu_cdev *c, unsigned long state)
{
	unsigned int clip, i;
	int ret;

	if (state > c->max_level)
		return -EINVAL;
	clip = c->freq_table[state];
	if (c->state == state && c->clipped_freq == clip)
		return 0;

	for (i = 0; i < c->npol; i++) {
		ret = freq_qos_update_request(&c->req[i], clip);
		if (ret < 0)
			return ret;
	}
	c->state = state;
	c->clipped_freq = clip;
	pr_debug("cluster%d: max freq %u kHz (state %lu)\n", c->cluster_id, clip, state);
	return 0;
}

static int cdev_set_cur_state(struct thermal_cooling_device *cdev,
			      unsigned long state)
{
	struct sprd_cpu_cdev *c = cdev->devdata;
	int ret;

	mutex_lock(&c->lock);
	ret = __set_state(c, state);
	mutex_unlock(&c->lock);
	return ret;
}

static u32 get_load(struct sprd_cpu_cdev *c, int cpu, int idx)
{
	u64 now, now_idle, dt, di;
	u32 load;

	now_idle = get_cpu_idle_time(cpu, &now, 0);
	di = now_idle - c->time_in_idle[idx];
	dt = now - c->time_in_idle_ts[idx];
	load = dt <= di ? 0 : div64_u64(100 * (dt - di), dt);
	c->time_in_idle[idx] = now_idle;
	c->time_in_idle_ts[idx] = now;
	return load;
}

static int cdev_get_requested_power(struct thermal_cooling_device *cdev, u32 *power)
{
	struct sprd_cpu_cdev *c = cdev->devdata;
	unsigned int cpu = cpumask_first(&c->cpus), i = 0, online = 0;
	u32 total_load = 0, stat, dyn;
	unsigned long freq;
	int ret;

	mutex_lock(&c->lock);
	if (cpu >= nr_cpu_ids) {
		*power = 0;
		ret = 0;
		goto out;
	}
	freq = cpufreq_quick_get(cpu);
	freq = freq ?: c->freq_table[0];

	for_each_cpu(cpu, &c->cpus) {
		u32 load = 0;

		if (cpu_online(cpu)) {
			load = get_load(c, cpu, i);
			online++;
		}
		total_load += load;
		i++;
	}
	total_load = total_load ?: 100;
	c->last_load = total_load;

	dyn = table_power(c->dyn_power_table, c->dyn_entries, freq) * total_load / 100;
	dyn += table_power(c->l2_power_table, c->dyn_entries, freq) * total_load / 100;

	ret = get_static(c, freq, online, &stat);
	if (!ret)
		*power = stat + dyn;
out:
	mutex_unlock(&c->lock);
	return ret;
}

static int cdev_state2power(struct thermal_cooling_device *cdev,
			    unsigned long state, u32 *power)
{
	struct sprd_cpu_cdev *c = cdev->devdata;
	unsigned int n = cpumask_weight(&c->cpus);
	u32 stat, dyn;
	int ret = 0;

	mutex_lock(&c->lock);
	if (state > c->max_level || !c->freq_table[state]) {
		ret = -EINVAL;
		goto out;
	}
	dyn = table_power(c->dyn_power_table, c->dyn_entries, c->freq_table[state]) * n;
	dyn += table_power(c->l2_power_table, c->dyn_entries, c->freq_table[state]) * n;
	ret = get_static(c, c->freq_table[state], n, &stat);
	if (!ret)
		*power = stat + dyn;
out:
	mutex_unlock(&c->lock);
	return ret;
}

static int level_for_freq(struct sprd_cpu_cdev *c, unsigned int freq)
{
	unsigned int l;

	for (l = 0; l <= c->max_level; l++) {
		if (freq == c->freq_table[l])
			return l;
		if (freq > c->freq_table[l])
			break;
	}
	return -EINVAL;
}

static int cdev_power2state(struct thermal_cooling_device *cdev, u32 power,
			    unsigned long *state)
{
	struct sprd_cpu_cdev *c = cdev->devdata;
	unsigned int cpu = cpumask_first(&c->cpus), cur, target;
	u32 last_load, norm, stat = 0, l2, dyn;
	int ret = 0, lvl;

	if (cpu >= nr_cpu_ids)
		return -ENODEV;

	mutex_lock(&c->lock);
	last_load = c->last_load ?: 100;
	cur = cpufreq_quick_get(cpu) ?: c->freq_table[0];

	ret = get_static(c, cur, cpumask_weight(&c->cpus), &stat);
	if (ret)
		goto out;
	l2 = table_power(c->l2_power_table, c->dyn_entries, cur) * last_load / 100;

	dyn = power > stat + l2 ? power - stat - l2 : 0;
	norm = dyn * 100 / last_load;
	target = table_freq(c->dyn_power_table, c->dyn_entries, norm);
	if (c->pm.min_cpufreq && target < c->pm.min_cpufreq)
		target = c->pm.min_cpufreq;

	lvl = level_for_freq(c, target);
	if (lvl < 0) {
		dev_warn_ratelimited(&cdev->device,
				     "cannot convert %u kHz into a cooling state\n", target);
		ret = -EINVAL;
		goto out;
	}
	*state = lvl;
out:
	mutex_unlock(&c->lock);
	return ret;
}

static const struct thermal_cooling_device_ops cdev_ops = {
	.get_max_state = cdev_get_max_state,
	.get_cur_state = cdev_get_cur_state,
	.set_cur_state = cdev_set_cur_state,
	.get_requested_power = cdev_get_requested_power,
	.state2power = cdev_state2power,
	.power2state = cdev_power2state,
};

/* sysfs: the vendor exposed min_freq / min_core_num on the cooling device */
static ssize_t min_freq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_cooling_device *cdev = container_of(dev, struct thermal_cooling_device, device);
	struct sprd_cpu_cdev *c = cdev->devdata;

	return sysfs_emit(buf, "%u\n", c->pm.min_cpufreq);
}

static ssize_t min_freq_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct thermal_cooling_device *cdev = container_of(dev, struct thermal_cooling_device, device);
	struct sprd_cpu_cdev *c = cdev->devdata;
	unsigned long val;
	unsigned int l;
	int ret = -EINVAL;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;
	mutex_lock(&c->lock);
	for (l = 0; l <= c->max_level; l++)
		if (val == c->freq_table[l]) {
			c->pm.min_cpufreq = val;
			ret = count;
			break;
		}
	mutex_unlock(&c->lock);
	return ret;
}
static DEVICE_ATTR_RW(min_freq);

static ssize_t min_core_num_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct thermal_cooling_device *cdev = container_of(dev, struct thermal_cooling_device, device);
	struct sprd_cpu_cdev *c = cdev->devdata;

	return sysfs_emit(buf, "%u\n", c->pm.min_cpunum);
}

static ssize_t min_core_num_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct thermal_cooling_device *cdev = container_of(dev, struct thermal_cooling_device, device);
	struct sprd_cpu_cdev *c = cdev->devdata;
	unsigned long val;

	if (kstrtoul(buf, 10, &val))
		return -EINVAL;
	c->pm.min_cpunum = val;
	return count;
}
static DEVICE_ATTR_RW(min_core_num);

/* ------------------------------------------------- temperature dependent OPP */

static void opp_update_fn(struct work_struct *work)
{
	struct sprd_cpu_cdev *c = container_of(to_delayed_work(work),
					       struct sprd_cpu_cdev, opp_work);
	unsigned int allow;
	int temp;

	if (opp_update && !zone_temp(&temp)) {
		allow = sprd_cpufreq_update_opp(cpumask_first(&c->cpus), temp);
		/* returns the table maximum, or 0 when nothing was switched */
		if (allow && (int)allow > 0 && allow != c->curr_max_freq) {
			mutex_lock(&c->lock);
			c->curr_max_freq = allow;
			if (!build_freq_table(c) && !build_power_tables(c)) {
				c->state = 0;
				c->clipped_freq = 0;
				__set_state(c, 0);
			}
			mutex_unlock(&c->lock);
			pr_info("cluster%d: allowed max frequency now %u kHz\n",
				c->cluster_id, allow);
		}
	}
	schedule_delayed_work(&c->opp_work, msecs_to_jiffies(opp_update_ms));
}

/* ------------------------------------------------------------- DT parsing */

static int read_scale(struct device_node *np, const char *prop, struct scale_coeff *s)
{
	u32 v[4];
	int ret = of_property_read_u32_array(np, prop, v, 4);

	if (ret) {
		pr_err("%pOF: missing %s\n", np, prop);
		return ret;
	}
	s->a = v[0];
	s->b = v[1];
	s->c = v[2];
	s->d = v[3];
	return 0;
}

static int read_dyn(struct device_node *np, const char *prop, struct dyn_coeff *d)
{
	u32 v[3];
	int ret = of_property_read_u32_array(np, prop, v, 3);

	if (ret) {
		pr_err("%pOF: missing %s\n", np, prop);
		return ret;
	}
	d->dynperghz = v[0];
	d->freq = v[1];
	d->voltage_base = v[2];
	return 0;
}

/*
 * sprd,efuse-switch selects leakage from the AP efuse; the DT of this board has
 * it at 0 and the efuse read helper (sprd_ap_efuse_read) does not exist on 6.12,
 * so the fixed bases from the DT are used, as the vendor did with the switch off.
 */
static int parse_power_model(struct device_node *np, struct power_model *pm)
{
	u32 efuse_switch = 0;
	int ret;

	of_property_read_u32(np, "sprd,efuse-switch", &efuse_switch);
	if (efuse_switch)
		pr_warn("%pOF: sprd,efuse-switch is set but efuse leakage is not supported, using fixed bases\n", np);

	ret = of_property_read_u32(np, "sprd,core-base", &pm->leak_core_base);
	if (ret)
		return ret;
	ret = of_property_read_u32(np, "sprd,cluster-base", &pm->leak_cluster_base);
	if (ret)
		return ret;
	if (read_scale(np, "sprd,core-temp-scale", &pm->core_temp) ||
	    read_scale(np, "sprd,core-voltage-scale", &pm->core_volt) ||
	    read_scale(np, "sprd,cluster-temp-scale", &pm->cluster_temp) ||
	    read_scale(np, "sprd,cluster-voltage-scale", &pm->cluster_volt) ||
	    read_dyn(np, "sprd,dynamic-core", &pm->core) ||
	    read_dyn(np, "sprd,dynamic-cluster", &pm->cluster))
		return -EINVAL;

	of_property_read_u32(np, "sprd,hotplug-period", &pm->hotplug_period);
	of_property_read_u32(np, "sprd,min-cpufreq", &pm->min_cpufreq);
	of_property_read_u32(np, "sprd,min-cpunum", &pm->min_cpunum);
	of_property_read_u32(np, "sprd,resistance-ja", &pm->resistance_ja);
	return 0;
}

/* ------------------------------------------------------------ registration */

static void free_cdev(struct sprd_cpu_cdev *c)
{
	unsigned int i;

	if (c->cdev) {
		device_remove_file(&c->cdev->device, &dev_attr_min_freq);
		device_remove_file(&c->cdev->device, &dev_attr_min_core_num);
		thermal_cooling_device_unregister(c->cdev);
	}
	cancel_delayed_work_sync(&c->opp_work);
	for (i = 0; i < c->npol; i++) {
		freq_qos_remove_request(&c->req[i]);
		cpufreq_cpu_put(c->pol[i]);
	}
	kfree(c->req);
	kfree(c->pol);
	if (c->policy)
		cpufreq_cpu_put(c->policy);
	kfree(c->freq_table);
	kfree(c->dyn_power_table);
	kfree(c->l2_power_table);
	kfree(c->time_in_idle);
	kfree(c->time_in_idle_ts);
	of_node_put(c->np);
	kfree(c);
}

static int register_one(struct device_node *child)
{
	struct sprd_cpu_cdev *c;
	char name[THERMAL_NAME_LENGTH];
	int id, cpu, ret;

	id = of_alias_get_id(child, "cooling-device");
	if (id < 0 || id >= MAX_CLUSTERS) {
		pr_err("%pOF: no cooling-device alias\n", child);
		return -ENODEV;
	}

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->cluster_id = id;
	c->np = of_node_get(child);
	mutex_init(&c->lock);
	INIT_DELAYED_WORK(&c->opp_work, opp_update_fn);

	for_each_possible_cpu(cpu)
		if (cpu_cluster(cpu) == id)
			cpumask_set_cpu(cpu, &c->cpus);
	if (cpumask_empty(&c->cpus)) {
		pr_info("cluster%d has no cpus\n", id);
		ret = -ENODEV;
		goto err;
	}

	ret = parse_power_model(child, &c->pm);
	if (ret) {
		pr_err("%pOF: bad power model (%d)\n", child, ret);
		goto err;
	}

	c->policy = cpufreq_cpu_get(cpumask_first(&c->cpus));
	if (!c->policy || !c->policy->freq_table) {
		ret = -EPROBE_DEFER;
		goto err;
	}
	c->curr_max_freq = UINT_MAX;
	/* c->policy is only used for the frequency table; the QoS policies follow below */
	ret = build_freq_table(c);
	if (ret)
		goto err;
	/* the highest frequency the table currently offers is the unrestricted state */
	c->curr_max_freq = c->freq_table[0];
	ret = build_power_tables(c);
	if (ret)
		goto err;

	c->time_in_idle = kcalloc(cpumask_weight(&c->cpus), sizeof(u64), GFP_KERNEL);
	c->time_in_idle_ts = kcalloc(cpumask_weight(&c->cpus), sizeof(u64), GFP_KERNEL);
	if (!c->time_in_idle || !c->time_in_idle_ts) {
		ret = -ENOMEM;
		goto err;
	}

	/* one FREQ_QOS_MAX request on every distinct policy of the cluster */
	c->pol = kcalloc(cpumask_weight(&c->cpus), sizeof(*c->pol), GFP_KERNEL);
	c->req = kcalloc(cpumask_weight(&c->cpus), sizeof(*c->req), GFP_KERNEL);
	if (!c->pol || !c->req) {
		ret = -ENOMEM;
		goto err;
	}
	for_each_cpu(cpu, &c->cpus) {
		struct cpufreq_policy *pl = cpufreq_cpu_get(cpu);
		unsigned int k;
		bool dup = false;

		if (!pl) {
			ret = -EPROBE_DEFER;
			goto err;
		}
		for (k = 0; k < c->npol; k++)
			if (c->pol[k] == pl)
				dup = true;
		if (dup) {
			cpufreq_cpu_put(pl);
			continue;
		}
		ret = freq_qos_add_request(&pl->constraints, &c->req[c->npol],
					   FREQ_QOS_MAX, c->freq_table[0]);
		if (ret < 0) {
			cpufreq_cpu_put(pl);
			goto err;
		}
		c->pol[c->npol++] = pl;
	}
	c->clipped_freq = c->freq_table[0];

	snprintf(name, sizeof(name), "thermal-cpufreq-%d", id);
	c->cdev = thermal_of_cooling_device_register(child, name, c, &cdev_ops);
	if (IS_ERR(c->cdev)) {
		ret = PTR_ERR(c->cdev);
		c->cdev = NULL;
		goto err;
	}
	device_create_file(&c->cdev->device, &dev_attr_min_freq);
	device_create_file(&c->cdev->device, &dev_attr_min_core_num);

	mutex_lock(&cdev_list_lock);
	list_add_tail(&c->node, &cdev_list);
	mutex_unlock(&cdev_list_lock);

	schedule_delayed_work(&c->opp_work, msecs_to_jiffies(opp_update_ms));
	pr_info("cluster%d: cpus %*pbl, %u levels (%u..%u kHz), min_cpufreq %u kHz, zone %s\n",
		id, cpumask_pr_args(&c->cpus), c->max_level + 1,
		c->freq_table[c->max_level], c->freq_table[0],
		c->pm.min_cpufreq, zone);
	return 0;
err:
	free_cdev(c);
	return ret;
}

static void probe_fn(struct work_struct *work)
{
	struct device_node *np, *child;
	int pending = 0;

	np = of_find_node_by_name(NULL, "cooling-devices");
	if (!np) {
		pr_err("no /cooling-devices node\n");
		return;
	}
	for_each_available_child_of_node(np, child) {
		struct sprd_cpu_cdev *c;
		bool done = false;
		int ret;

		if (!of_device_is_compatible(child, "sprd,cluster-cooling"))
			continue;
		mutex_lock(&cdev_list_lock);
		list_for_each_entry(c, &cdev_list, node)
			if (c->np == child)
				done = true;
		mutex_unlock(&cdev_list_lock);
		if (done)
			continue;
		ret = register_one(child);
		if (ret == -EPROBE_DEFER)
			pending++;
		else if (ret)
			pr_err("%pOF: register failed (%d)\n", child, ret);
	}
	of_node_put(np);

	if (pending && ++probe_tries < 60)
		schedule_delayed_work(&probe_work, HZ);
	else if (pending)
		pr_err("gave up waiting for cpufreq policies\n");
}

static int __init sprd_cpu_cooling_init(void)
{
	INIT_DELAYED_WORK(&probe_work, probe_fn);
	schedule_delayed_work(&probe_work, 0);
	return 0;
}

static void __exit sprd_cpu_cooling_exit(void)
{
	struct sprd_cpu_cdev *c, *tmp;

	cancel_delayed_work_sync(&probe_work);
	mutex_lock(&cdev_list_lock);
	list_for_each_entry_safe(c, tmp, &cdev_list, node) {
		list_del(&c->node);
		free_cdev(c);
	}
	mutex_unlock(&cdev_list_lock);
}

module_init(sprd_cpu_cooling_init);
module_exit(sprd_cpu_cooling_exit);

MODULE_DESCRIPTION("Spreadtrum CPU cooling devices (port of the vendor sprd_cpu_cooling)");
MODULE_LICENSE("GPL");
