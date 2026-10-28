// SPDX-License-Identifier: GPL-2.0
/* Copyright 2019 Collabora ltd. */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/thermal.h>

#include "panfrost_device.h"
#include "panfrost_devfreq.h"

static void panfrost_devfreq_update_utilization(struct panfrost_devfreq *pfdevfreq)
{
	ktime_t now, last;

	now = ktime_get();
	last = pfdevfreq->time_last_update;

	if (pfdevfreq->busy_count > 0)
		pfdevfreq->busy_time += ktime_sub(now, last);
	else
		pfdevfreq->idle_time += ktime_sub(now, last);

	pfdevfreq->time_last_update = now;
}

static int panfrost_devfreq_target(struct device *dev, unsigned long *freq,
				   u32 flags)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	int err;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	err = dev_pm_opp_set_rate(dev, *freq);
	if (!err)
		pfdev->pfdevfreq.current_frequency = *freq;

	return err;
}

static void panfrost_devfreq_reset(struct panfrost_devfreq *pfdevfreq)
{
	pfdevfreq->busy_time = 0;
	pfdevfreq->idle_time = 0;
	pfdevfreq->time_last_update = ktime_get();
}

static int panfrost_devfreq_get_dev_status(struct device *dev,
					   struct devfreq_dev_status *status)
{
	struct panfrost_device *pfdev = dev_get_drvdata(dev);
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;
	unsigned long irqflags;

	status->current_frequency = clk_get_rate(pfdev->clock);

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);

	panfrost_devfreq_update_utilization(pfdevfreq);

	status->total_time = ktime_to_ns(ktime_add(pfdevfreq->busy_time,
						   pfdevfreq->idle_time));

	status->busy_time = ktime_to_ns(pfdevfreq->busy_time);

	panfrost_devfreq_reset(pfdevfreq);

	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);

	dev_dbg(pfdev->dev, "busy %lu total %lu %lu %% freq %lu MHz\n",
		status->busy_time, status->total_time,
		status->busy_time / (status->total_time / 100),
		status->current_frequency / 1000 / 1000);

	return 0;
}

static struct devfreq_dev_profile panfrost_devfreq_profile = {
	.timer = DEVFREQ_TIMER_DELAYED,
	.polling_ms = 50, /* ~3 frames */
	.target = panfrost_devfreq_target,
	.get_dev_status = panfrost_devfreq_get_dev_status,
};

static int panfrost_read_speedbin(struct device *dev)
{
	u32 val;
	int ret;

	ret = nvmem_cell_read_variable_le_u32(dev, "speed-bin", &val);
	if (ret) {
		/*
		 * -ENOENT means that this platform doesn't support speedbins
		 * as it didn't declare any speed-bin nvmem: in this case, we
		 * keep going without it; any other error means that we are
		 * supposed to read the bin value, but we failed doing so.
		 */
		if (ret != -ENOENT && ret != -EOPNOTSUPP) {
			DRM_DEV_ERROR(dev, "Cannot read speed-bin (%d).", ret);
			return ret;
		}

		return 0;
	}
	DRM_DEV_DEBUG(dev, "Using speed-bin = 0x%x\n", val);

	return devm_pm_opp_set_supported_hw(dev, &val, 1);
}

/*
 * Unisoc "sprd,mali-power-model" (the vendor kbase cooling device), driven by
 * the same DT coefficients and the same arithmetic as the vendor
 * drivers/thermal/sprd_gpu_device.c, plugged into the devfreq cooling
 * framework as the real-power callback.
 */
struct sprd_scale {
	s32 a, b, c, d;
};

struct sprd_power_model {
	bool valid;
	s32 leak_core_base;
	struct sprd_scale temp_scale;
	struct sprd_scale voltage_scale;
	s32 dyn_per_ghz, dyn_freq, dyn_vbase;
};

static struct sprd_power_model sprd_pm;

static int sprd_power_model_init(struct device *dev)
{
	struct device_node *np = of_parse_phandle(dev->of_node, "sprd,power-model", 0);
	int ret;
	u32 dyn[3];

	if (!np)
		return -ENODEV;
	ret = of_property_read_u32(np, "sprd,core-base", &sprd_pm.leak_core_base);
	if (!ret)
		ret = of_property_read_u32_array(np, "sprd,temp-scale",
						 (u32 *)&sprd_pm.temp_scale, 4);
	if (!ret)
		ret = of_property_read_u32_array(np, "sprd,voltage-scale",
						 (u32 *)&sprd_pm.voltage_scale, 4);
	if (!ret)
		ret = of_property_read_u32_array(np, "sprd,dynamic-core", dyn, 3);
	of_node_put(np);
	if (ret)
		return ret;
	sprd_pm.dyn_per_ghz = dyn[0];
	sprd_pm.dyn_freq = dyn[1];
	sprd_pm.dyn_vbase = dyn[2];
	sprd_pm.valid = true;
	return 0;
}

/* Pdyn(mW) = dynperghz * freq_mhz * (V / Vbase)^2 / 10000 */
static u64 sprd_dyn_power(unsigned long freq_hz, unsigned long mv)
{
	u64 power = (u64)sprd_pm.dyn_per_ghz * (freq_hz / 1000000) * mv * mv;

	do_div(power, sprd_pm.dyn_vbase * sprd_pm.dyn_vbase);
	do_div(power, 10000);
	return power;
}

static s64 sprd_scale3(const struct sprd_scale *c, s64 x)
{
	return c->a * x * x * x + c->b * x * x + c->c * x + c->d;
}

/* static power (mW) from the die temperature and the OPP voltage (mV) */
static u64 sprd_static_power(unsigned long mv)
{
	struct thermal_zone_device *tz = thermal_zone_get_zone_by_name("gpu-thmzone");
	int temp_mc = 55000;	/* vendor fallback when the sensor cannot be read */
	s64 t_scale, v_scale, p;

	if (!IS_ERR(tz) && thermal_zone_get_temp(tz, &temp_mc))
		temp_mc = 55000;

	t_scale = sprd_scale3(&sprd_pm.temp_scale, temp_mc / 1000);
	t_scale = div_s64(t_scale, 10000);
	v_scale = sprd_pm.voltage_scale.a * ((s64)mv * mv * mv / 1000) +
		  sprd_pm.voltage_scale.b * (s64)mv * mv +
		  sprd_pm.voltage_scale.c * (s64)mv * 1000 +
		  sprd_pm.voltage_scale.d * 1000 * 1000;
	v_scale = div_s64(v_scale, 100000);

	p = div_s64((s64)sprd_pm.leak_core_base * t_scale * v_scale, 100000000);
	return p > 0 ? p : 0;
}

static int sprd_get_real_power(struct devfreq *df, u32 *power,
			       unsigned long freq, unsigned long voltage_mv)
{
	struct devfreq_dev_status *st = &df->last_status;
	u64 dyn = sprd_dyn_power(freq, voltage_mv);

	/* dynamic power scales with the GPU utilisation, leakage does not */
	if (st->total_time)
		dyn = div64_u64(dyn * st->busy_time, st->total_time);
	*power = sprd_static_power(voltage_mv) + dyn;
	return 0;
}

static struct devfreq_cooling_power sprd_power_ops = {
	.get_real_power = sprd_get_real_power,
};

/* OPPs above what the clock can run (silicon variant limit) are unusable */
static void panfrost_devfreq_trim_opps(struct device *dev, struct clk *clk)
{
	unsigned long max = clk_round_rate(clk, ULONG_MAX), f = 0;
	struct dev_pm_opp *opp;

	if (!max)
		return;
	while (!IS_ERR(opp = dev_pm_opp_find_freq_ceil(dev, &f))) {
		dev_pm_opp_put(opp);
		if (f > max) {
			dev_info(dev, "OPP %lu Hz is above the clock limit, disabled\n", f);
			dev_pm_opp_disable(dev, f);
		}
		f++;
	}
}

int panfrost_devfreq_init(struct panfrost_device *pfdev)
{
	int ret;
	struct dev_pm_opp *opp;
	unsigned long cur_freq;
	struct device *dev = &pfdev->pdev->dev;
	struct devfreq *devfreq;
	struct thermal_cooling_device *cooling;
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;
	unsigned long freq = ULONG_MAX;

	if (pfdev->comp->num_supplies > 1) {
		/*
		 * GPUs with more than 1 supply require platform-specific handling:
		 * continue without devfreq
		 */
		DRM_DEV_INFO(dev, "More than 1 supply is not supported yet\n");
		return 0;
	}

	ret = panfrost_read_speedbin(dev);
	if (ret)
		return ret;

	ret = devm_pm_opp_set_regulators(dev, pfdev->comp->supply_names);
	if (ret) {
		/* Continue if the optional regulator is missing */
		if (ret != -ENODEV) {
			if (ret != -EPROBE_DEFER)
				DRM_DEV_ERROR(dev, "Couldn't set OPP regulators\n");
			return ret;
		}
	}

	ret = devm_pm_opp_of_add_table(dev);
	if (ret) {
		/* Optional, continue without devfreq */
		if (ret == -ENODEV)
			ret = 0;
		return ret;
	}
	pfdevfreq->opp_of_table_added = true;
	panfrost_devfreq_trim_opps(dev, pfdev->clock);

	spin_lock_init(&pfdevfreq->lock);

	panfrost_devfreq_reset(pfdevfreq);

	cur_freq = clk_get_rate(pfdev->clock);

	opp = devfreq_recommended_opp(dev, &cur_freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	panfrost_devfreq_profile.initial_freq = cur_freq;

	/*
	 * We could wait until panfrost_devfreq_target() to set this value, but
	 * since the simple_ondemand governor works asynchronously, there's a
	 * chance by the time someone opens the device's fdinfo file, current
	 * frequency hasn't been updated yet, so let's just do an early set.
	 */
	pfdevfreq->current_frequency = cur_freq;

	/*
	 * Set the recommend OPP this will enable and configure the regulator
	 * if any and will avoid a switch off by regulator_late_cleanup()
	 */
	ret = dev_pm_opp_set_opp(dev, opp);
	dev_pm_opp_put(opp);
	if (ret) {
		DRM_DEV_ERROR(dev, "Couldn't set recommended OPP\n");
		return ret;
	}

	/* Find the fastest defined rate  */
	opp = dev_pm_opp_find_freq_floor(dev, &freq);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	pfdevfreq->fast_rate = freq;

	dev_pm_opp_put(opp);

	/*
	 * Setup default thresholds for the simple_ondemand governor.
	 * The values are chosen based on experiments.
	 */
	pfdevfreq->gov_data.upthreshold = 45;
	pfdevfreq->gov_data.downdifferential = 5;

	devfreq = devm_devfreq_add_device(dev, &panfrost_devfreq_profile,
					  DEVFREQ_GOV_SIMPLE_ONDEMAND,
					  &pfdevfreq->gov_data);
	if (IS_ERR(devfreq)) {
		DRM_DEV_ERROR(dev, "Couldn't initialize GPU devfreq\n");
		return PTR_ERR(devfreq);
	}
	pfdevfreq->devfreq = devfreq;

	if (!sprd_power_model_init(dev))
		dev_info(dev, "using the vendor sprd,mali-power-model coefficients\n");
	cooling = devfreq_cooling_em_register(devfreq,
					      sprd_pm.valid ? &sprd_power_ops : NULL);
	if (IS_ERR(cooling))
		DRM_DEV_INFO(dev, "Failed to register cooling device\n");
	else
		pfdevfreq->cooling = cooling;

	return 0;
}

void panfrost_devfreq_fini(struct panfrost_device *pfdev)
{
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;

	if (pfdevfreq->cooling) {
		devfreq_cooling_unregister(pfdevfreq->cooling);
		pfdevfreq->cooling = NULL;
	}
}

void panfrost_devfreq_resume(struct panfrost_device *pfdev)
{
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;

	if (!pfdevfreq->devfreq)
		return;

	panfrost_devfreq_reset(pfdevfreq);

	devfreq_resume_device(pfdevfreq->devfreq);
}

void panfrost_devfreq_suspend(struct panfrost_device *pfdev)
{
	struct panfrost_devfreq *pfdevfreq = &pfdev->pfdevfreq;

	if (!pfdevfreq->devfreq)
		return;

	devfreq_suspend_device(pfdevfreq->devfreq);
}

void panfrost_devfreq_record_busy(struct panfrost_devfreq *pfdevfreq)
{
	unsigned long irqflags;

	if (!pfdevfreq->devfreq)
		return;

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);

	panfrost_devfreq_update_utilization(pfdevfreq);

	pfdevfreq->busy_count++;

	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);
}

void panfrost_devfreq_record_idle(struct panfrost_devfreq *pfdevfreq)
{
	unsigned long irqflags;

	if (!pfdevfreq->devfreq)
		return;

	spin_lock_irqsave(&pfdevfreq->lock, irqflags);

	panfrost_devfreq_update_utilization(pfdevfreq);

	WARN_ON(--pfdevfreq->busy_count < 0);

	spin_unlock_irqrestore(&pfdevfreq->lock, irqflags);
}
