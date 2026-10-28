// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Spreadtrum Communications Inc.
 *
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>

#define SPRD_RTC_SEC_CNT_VALUE		0x0
#define SPRD_RTC_MIN_CNT_VALUE		0x4
#define SPRD_RTC_HOUR_CNT_VALUE		0x8
#define SPRD_RTC_DAY_CNT_VALUE		0xc
#define SPRD_RTC_SEC_CNT_UPD		0x10
#define SPRD_RTC_MIN_CNT_UPD		0x14
#define SPRD_RTC_HOUR_CNT_UPD		0x18
#define SPRD_RTC_DAY_CNT_UPD		0x1c
#define SPRD_RTC_SEC_ALM_UPD		0x20
#define SPRD_RTC_MIN_ALM_UPD		0x24
#define SPRD_RTC_HOUR_ALM_UPD		0x28
#define SPRD_RTC_DAY_ALM_UPD		0x2c
#define SPRD_RTC_INT_EN			0x30
#define SPRD_RTC_INT_RAW_STS		0x34
#define SPRD_RTC_INT_CLR		0x38
#define SPRD_RTC_INT_MASK_STS		0x3C
#define SPRD_RTC_SEC_ALM_VALUE		0x40
#define SPRD_RTC_MIN_ALM_VALUE		0x44
#define SPRD_RTC_HOUR_ALM_VALUE		0x48
#define SPRD_RTC_DAY_ALM_VALUE		0x4c
#define SPRD_RTC_SPG_VALUE		0x50
#define SPRD_RTC_SPG_UPD		0x54
#define SPRD_RTC_PWR_CTRL		0x58
#define SPRD_RTC_PWR_STS		0x5c
#define SPRD_RTC_SEC_AUXALM_UPD		0x60
#define SPRD_RTC_MIN_AUXALM_UPD		0x64
#define SPRD_RTC_HOUR_AUXALM_UPD	0x68
#define SPRD_RTC_DAY_AUXALM_UPD		0x6c

/* BIT & MASK definition for SPRD_RTC_INT_* registers */
#define SPRD_RTC_SEC_EN			BIT(0)
#define SPRD_RTC_MIN_EN			BIT(1)
#define SPRD_RTC_HOUR_EN		BIT(2)
#define SPRD_RTC_DAY_EN			BIT(3)
#define SPRD_RTC_ALARM_EN		BIT(4)
#define SPRD_RTC_HRS_FORMAT_EN		BIT(5)
#define SPRD_RTC_AUXALM_EN		BIT(6)
#define SPRD_RTC_SPG_UPD_EN		BIT(7)
#define SPRD_RTC_SEC_UPD_EN		BIT(8)
#define SPRD_RTC_MIN_UPD_EN		BIT(9)
#define SPRD_RTC_HOUR_UPD_EN		BIT(10)
#define SPRD_RTC_DAY_UPD_EN		BIT(11)
#define SPRD_RTC_ALMSEC_UPD_EN		BIT(12)
#define SPRD_RTC_ALMMIN_UPD_EN		BIT(13)
#define SPRD_RTC_ALMHOUR_UPD_EN		BIT(14)
#define SPRD_RTC_ALMDAY_UPD_EN		BIT(15)
#define SPRD_RTC_INT_MASK		GENMASK(15, 0)

#define SPRD_RTC_TIME_INT_MASK				\
	(SPRD_RTC_SEC_UPD_EN | SPRD_RTC_MIN_UPD_EN |	\
	 SPRD_RTC_HOUR_UPD_EN | SPRD_RTC_DAY_UPD_EN)

#define SPRD_RTC_ALMTIME_INT_MASK				\
	(SPRD_RTC_ALMSEC_UPD_EN | SPRD_RTC_ALMMIN_UPD_EN |	\
	 SPRD_RTC_ALMHOUR_UPD_EN | SPRD_RTC_ALMDAY_UPD_EN)

#define SPRD_RTC_ALM_INT_MASK			\
	(SPRD_RTC_SEC_EN | SPRD_RTC_MIN_EN |	\
	 SPRD_RTC_HOUR_EN | SPRD_RTC_DAY_EN |	\
	 SPRD_RTC_ALARM_EN | SPRD_RTC_AUXALM_EN)

/* second/minute/hour/day values mask definition */
#define SPRD_RTC_SEC_MASK		GENMASK(5, 0)
#define SPRD_RTC_MIN_MASK		GENMASK(5, 0)
#define SPRD_RTC_HOUR_MASK		GENMASK(4, 0)
#define SPRD_RTC_DAY_MASK		GENMASK(15, 0)

/* alarm lock definition for SPRD_RTC_SPG_UPD register */
#define SPRD_RTC_ALMLOCK_MASK		GENMASK(7, 0)
#define SPRD_RTC_ALM_UNLOCK		0xa5
#define SPRD_RTC_ALM_LOCK		(~SPRD_RTC_ALM_UNLOCK &	\
					 SPRD_RTC_ALMLOCK_MASK)

/* SPG values definition for SPRD_RTC_SPG_UPD register */
#define SPRD_RTC_POWEROFF_ALM_FLAG	BIT(8)

/* power control/status definition */
#define SPRD_RTC_POWER_RESET_VALUE	0x96
#define SPRD_RTC_POWER_STS_CLEAR	GENMASK(7, 0)
#define SPRD_RTC_POWER_STS_SHIFT	8
#define SPRD_RTC_POWER_STS_VALID	\
	(~SPRD_RTC_POWER_RESET_VALUE << SPRD_RTC_POWER_STS_SHIFT)

/*
 * Timeout of synchronizing time and alarm registers (us). Upstream uses a
 * fixed 200 ms; on the ADT1061 these waits time out, so they are tunable at
 * runtime (/sys/module/sprd_sc27xx_rtc/parameters) to find out whether the
 * update-done flags are late or never arrive.
 */
static unsigned int poll_timeout_us = 200000;
module_param(poll_timeout_us, uint, 0644);
MODULE_PARM_DESC(poll_timeout_us, "wait for RTC update-done flags (us)");

static unsigned int poll_delay_us = 20000;
module_param(poll_delay_us, uint, 0644);
MODULE_PARM_DESC(poll_delay_us, "poll interval for RTC update-done flags (us)");

/* experiment: also enable the *_UPD_EN bits in SPRD_RTC_INT_EN before waiting */
static bool upd_int_en;
module_param(upd_int_en, bool, 0644);
MODULE_PARM_DESC(upd_int_en, "enable update-done bits in INT_EN before waiting");

#define SPRD_RTC_POLL_TIMEOUT		poll_timeout_us
#define SPRD_RTC_POLL_DELAY_US		poll_delay_us

/*
 * SC2730: the RTC block only answers once its module and clock enable bits are
 * set in the PMIC global block. The stock U-Boot does this (function at
 * 0x2ddd0 of u-boot-sign.bin: MODULE_EN0 |= 2, CLK_EN0 |= 2, followed by writes
 * to the RTC registers at 0x228..0x238); with another bootloader flow the block
 * stays dead: every register reads 0 and updates never complete.
 */
struct sprd_rtc_variant {
	u32 module_en;
	u32 clk_en;
	u32 en_bit;
};

static const struct sprd_rtc_variant sc2730_variant = {
	.module_en = 0x1808,
	.clk_en = 0x1810,
	.en_bit = BIT(1),
};

struct sprd_rtc {
	struct rtc_device	*rtc;
	struct regmap		*regmap;
	struct device		*dev;
	u32			base;
	int			irq;
	bool			valid;
	struct dentry		*dbg;
};

static void sprd_rtc_dump(struct sprd_rtc *rtc, const char *why)
{
	static const u32 regs[] = {
		SPRD_RTC_SEC_CNT_VALUE, SPRD_RTC_MIN_CNT_VALUE,
		SPRD_RTC_HOUR_CNT_VALUE, SPRD_RTC_DAY_CNT_VALUE,
		SPRD_RTC_SEC_CNT_UPD, SPRD_RTC_MIN_CNT_UPD,
		SPRD_RTC_HOUR_CNT_UPD, SPRD_RTC_DAY_CNT_UPD,
		SPRD_RTC_INT_EN, SPRD_RTC_INT_RAW_STS, SPRD_RTC_INT_MASK_STS,
		SPRD_RTC_SEC_ALM_VALUE, SPRD_RTC_MIN_ALM_VALUE,
		SPRD_RTC_HOUR_ALM_VALUE, SPRD_RTC_DAY_ALM_VALUE,
		SPRD_RTC_SPG_VALUE, SPRD_RTC_SPG_UPD,
		SPRD_RTC_PWR_CTRL, SPRD_RTC_PWR_STS,
	};
	char line[160];
	int i, n = 0;
	u32 val;

	dev_info(rtc->dev, "register dump (%s), base 0x%x\n", why, rtc->base);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		int ret = regmap_read(rtc->regmap, rtc->base + regs[i], &val);

		n += scnprintf(line + n, sizeof(line) - n, "[%02x]=%s%x ", regs[i],
			       ret ? "ERR" : "", ret ? ret : val);
		if (n > 110 || i == ARRAY_SIZE(regs) - 1) {
			dev_info(rtc->dev, "%s\n", line);
			n = 0;
		}
	}
}

static int sprd_rtc_wait_flags(struct sprd_rtc *rtc, u32 mask, const char *what)
{
	ktime_t t0 = ktime_get();
	u32 val = 0;
	int ret;

	if (upd_int_en)
		regmap_update_bits(rtc->regmap, rtc->base + SPRD_RTC_INT_EN,
				   SPRD_RTC_INT_MASK & ~SPRD_RTC_ALM_INT_MASK,
				   mask);

	ret = regmap_read_poll_timeout(rtc->regmap,
				       rtc->base + SPRD_RTC_INT_RAW_STS, val,
				       ((val & mask) == mask),
				       SPRD_RTC_POLL_DELAY_US,
				       SPRD_RTC_POLL_TIMEOUT);
	if (ret) {
		dev_err(rtc->dev, "%s: wait for 0x%x timed out after %lld ms, raw status 0x%x\n",
			what, mask, ktime_to_ms(ktime_sub(ktime_get(), t0)), val);
		sprd_rtc_dump(rtc, what);
	} else {
		dev_dbg(rtc->dev, "%s: flags 0x%x set after %lld us\n", what,
			mask, ktime_to_us(ktime_sub(ktime_get(), t0)));
	}
	return ret;
}

/*
 * The Spreadtrum RTC controller has 3 groups registers, including time, normal
 * alarm and auxiliary alarm. The time group registers are used to set RTC time,
 * the normal alarm registers are used to set normal alarm, and the auxiliary
 * alarm registers are used to set auxiliary alarm. Both alarm event and
 * auxiliary alarm event can wake up system from deep sleep, but only alarm
 * event can power up system from power down status.
 */
enum sprd_rtc_reg_types {
	SPRD_RTC_TIME,
	SPRD_RTC_ALARM,
	SPRD_RTC_AUX_ALARM,
};

static int sprd_rtc_clear_alarm_ints(struct sprd_rtc *rtc)
{
	return regmap_write(rtc->regmap, rtc->base + SPRD_RTC_INT_CLR,
			    SPRD_RTC_ALM_INT_MASK);
}

static int sprd_rtc_lock_alarm(struct sprd_rtc *rtc, bool lock)
{
	int ret;
	u32 val;

	ret = regmap_read(rtc->regmap, rtc->base + SPRD_RTC_SPG_VALUE, &val);
	if (ret)
		return ret;

	val &= ~SPRD_RTC_ALMLOCK_MASK;
	if (lock)
		val |= SPRD_RTC_ALM_LOCK;
	else
		val |= SPRD_RTC_ALM_UNLOCK | SPRD_RTC_POWEROFF_ALM_FLAG;

	ret = regmap_write(rtc->regmap, rtc->base + SPRD_RTC_SPG_UPD, val);
	if (ret)
		return ret;

	/* wait until the SPG value is updated successfully */
	ret = sprd_rtc_wait_flags(rtc, SPRD_RTC_SPG_UPD_EN, "SPG update");
	if (ret)
		return ret;

	return regmap_write(rtc->regmap, rtc->base + SPRD_RTC_INT_CLR,
			    SPRD_RTC_SPG_UPD_EN);
}

static int sprd_rtc_get_secs(struct sprd_rtc *rtc, enum sprd_rtc_reg_types type,
			     time64_t *secs)
{
	u32 sec_reg, min_reg, hour_reg, day_reg;
	u32 val, sec, min, hour, day;
	int ret;

	switch (type) {
	case SPRD_RTC_TIME:
		sec_reg = SPRD_RTC_SEC_CNT_VALUE;
		min_reg = SPRD_RTC_MIN_CNT_VALUE;
		hour_reg = SPRD_RTC_HOUR_CNT_VALUE;
		day_reg = SPRD_RTC_DAY_CNT_VALUE;
		break;
	case SPRD_RTC_ALARM:
		sec_reg = SPRD_RTC_SEC_ALM_VALUE;
		min_reg = SPRD_RTC_MIN_ALM_VALUE;
		hour_reg = SPRD_RTC_HOUR_ALM_VALUE;
		day_reg = SPRD_RTC_DAY_ALM_VALUE;
		break;
	case SPRD_RTC_AUX_ALARM:
		sec_reg = SPRD_RTC_SEC_AUXALM_UPD;
		min_reg = SPRD_RTC_MIN_AUXALM_UPD;
		hour_reg = SPRD_RTC_HOUR_AUXALM_UPD;
		day_reg = SPRD_RTC_DAY_AUXALM_UPD;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_read(rtc->regmap, rtc->base + sec_reg, &val);
	if (ret)
		return ret;

	sec = val & SPRD_RTC_SEC_MASK;

	ret = regmap_read(rtc->regmap, rtc->base + min_reg, &val);
	if (ret)
		return ret;

	min = val & SPRD_RTC_MIN_MASK;

	ret = regmap_read(rtc->regmap, rtc->base + hour_reg, &val);
	if (ret)
		return ret;

	hour = val & SPRD_RTC_HOUR_MASK;

	ret = regmap_read(rtc->regmap, rtc->base + day_reg, &val);
	if (ret)
		return ret;

	day = val & SPRD_RTC_DAY_MASK;
	*secs = (((time64_t)(day * 24) + hour) * 60 + min) * 60 + sec;
	return 0;
}

static int sprd_rtc_set_secs(struct sprd_rtc *rtc, enum sprd_rtc_reg_types type,
			     time64_t secs)
{
	u32 sec_reg, min_reg, hour_reg, day_reg, sts_mask;
	u32 sec, min, hour, day;
	int ret, rem;

	/* convert seconds to RTC time format */
	day = div_s64_rem(secs, 86400, &rem);
	hour = rem / 3600;
	rem -= hour * 3600;
	min = rem / 60;
	sec = rem - min * 60;

	switch (type) {
	case SPRD_RTC_TIME:
		sec_reg = SPRD_RTC_SEC_CNT_UPD;
		min_reg = SPRD_RTC_MIN_CNT_UPD;
		hour_reg = SPRD_RTC_HOUR_CNT_UPD;
		day_reg = SPRD_RTC_DAY_CNT_UPD;
		sts_mask = SPRD_RTC_TIME_INT_MASK;
		break;
	case SPRD_RTC_ALARM:
		sec_reg = SPRD_RTC_SEC_ALM_UPD;
		min_reg = SPRD_RTC_MIN_ALM_UPD;
		hour_reg = SPRD_RTC_HOUR_ALM_UPD;
		day_reg = SPRD_RTC_DAY_ALM_UPD;
		sts_mask = SPRD_RTC_ALMTIME_INT_MASK;
		break;
	case SPRD_RTC_AUX_ALARM:
		sec_reg = SPRD_RTC_SEC_AUXALM_UPD;
		min_reg = SPRD_RTC_MIN_AUXALM_UPD;
		hour_reg = SPRD_RTC_HOUR_AUXALM_UPD;
		day_reg = SPRD_RTC_DAY_AUXALM_UPD;
		sts_mask = 0;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_write(rtc->regmap, rtc->base + sec_reg, sec);
	if (ret)
		return ret;

	ret = regmap_write(rtc->regmap, rtc->base + min_reg, min);
	if (ret)
		return ret;

	ret = regmap_write(rtc->regmap, rtc->base + hour_reg, hour);
	if (ret)
		return ret;

	ret = regmap_write(rtc->regmap, rtc->base + day_reg, day);
	if (ret)
		return ret;

	if (type == SPRD_RTC_AUX_ALARM)
		return 0;

	/*
	 * Since the time and normal alarm registers are put in always-power-on
	 * region supplied by VDDRTC, then these registers changing time will
	 * be very long, about 125ms. Thus here we should wait until all
	 * values are updated successfully.
	 */
	ret = sprd_rtc_wait_flags(rtc, sts_mask,
				  type == SPRD_RTC_TIME ? "set time" : "set alarm");
	if (ret < 0)
		return ret;

	return regmap_write(rtc->regmap, rtc->base + SPRD_RTC_INT_CLR,
			    sts_mask);
}

static int sprd_rtc_set_aux_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct sprd_rtc *rtc = dev_get_drvdata(dev);
	time64_t secs = rtc_tm_to_time64(&alrm->time);
	int ret;

	/* clear the auxiliary alarm interrupt status */
	ret = regmap_write(rtc->regmap, rtc->base + SPRD_RTC_INT_CLR,
			   SPRD_RTC_AUXALM_EN);
	if (ret)
		return ret;

	ret = sprd_rtc_set_secs(rtc, SPRD_RTC_AUX_ALARM, secs);
	if (ret)
		return ret;

	if (alrm->enabled) {
		ret = regmap_update_bits(rtc->regmap,
					 rtc->base + SPRD_RTC_INT_EN,
					 SPRD_RTC_AUXALM_EN,
					 SPRD_RTC_AUXALM_EN);
	} else {
		ret = regmap_update_bits(rtc->regmap,
					 rtc->base + SPRD_RTC_INT_EN,
					 SPRD_RTC_AUXALM_EN, 0);
	}

	return ret;
}

static int sprd_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct sprd_rtc *rtc = dev_get_drvdata(dev);
	time64_t secs;
	int ret;

	if (!rtc->valid) {
		dev_warn(dev, "RTC values are invalid\n");
		return -EINVAL;
	}

	ret = sprd_rtc_get_secs(rtc, SPRD_RTC_TIME, &secs);
	if (ret)
		return ret;

	rtc_time64_to_tm(secs, tm);
	return 0;
}

static int sprd_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct sprd_rtc *rtc = dev_get_drvdata(dev);
	time64_t secs = rtc_tm_to_time64(tm);
	int ret;

	ret = sprd_rtc_set_secs(rtc, SPRD_RTC_TIME, secs);
	if (ret)
		return ret;

	if (!rtc->valid) {
		/* Clear RTC power status firstly */
		ret = regmap_write(rtc->regmap, rtc->base + SPRD_RTC_PWR_CTRL,
				   SPRD_RTC_POWER_STS_CLEAR);
		if (ret)
			return ret;

		/*
		 * Set RTC power status to indicate now RTC has valid time
		 * values.
		 */
		ret = regmap_write(rtc->regmap, rtc->base + SPRD_RTC_PWR_CTRL,
				   SPRD_RTC_POWER_STS_VALID);
		if (ret)
			return ret;

		rtc->valid = true;
	}

	return 0;
}

static int sprd_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct sprd_rtc *rtc = dev_get_drvdata(dev);
	time64_t secs;
	int ret;
	u32 val;

	/*
	 * The RTC core checks to see if there is an alarm already set in RTC
	 * hardware, and we always read the normal alarm at this time.
	 */
	ret = sprd_rtc_get_secs(rtc, SPRD_RTC_ALARM, &secs);
	if (ret)
		return ret;

	rtc_time64_to_tm(secs, &alrm->time);

	ret = regmap_read(rtc->regmap, rtc->base + SPRD_RTC_INT_EN, &val);
	if (ret)
		return ret;

	alrm->enabled = !!(val & SPRD_RTC_ALARM_EN);

	ret = regmap_read(rtc->regmap, rtc->base + SPRD_RTC_INT_RAW_STS, &val);
	if (ret)
		return ret;

	alrm->pending = !!(val & SPRD_RTC_ALARM_EN);
	return 0;
}

static int sprd_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	struct sprd_rtc *rtc = dev_get_drvdata(dev);
	time64_t secs = rtc_tm_to_time64(&alrm->time);
	struct rtc_time aie_time =
		rtc_ktime_to_tm(rtc->rtc->aie_timer.node.expires);
	int ret;

	/*
	 * We have 2 groups alarms: normal alarm and auxiliary alarm. Since
	 * both normal alarm event and auxiliary alarm event can wake up system
	 * from deep sleep, but only alarm event can power up system from power
	 * down status. Moreover we do not need to poll about 125ms when
	 * updating auxiliary alarm registers. Thus we usually set auxiliary
	 * alarm when wake up system from deep sleep, and for other scenarios,
	 * we should set normal alarm with polling status.
	 *
	 * So here we check if the alarm time is set by aie_timer, if yes, we
	 * should set normal alarm, if not, we should set auxiliary alarm which
	 * means it is just a wake event.
	 */
	if (!rtc->rtc->aie_timer.enabled || rtc_tm_sub(&aie_time, &alrm->time))
		return sprd_rtc_set_aux_alarm(dev, alrm);

	/* clear the alarm interrupt status firstly */
	ret = regmap_write(rtc->regmap, rtc->base + SPRD_RTC_INT_CLR,
			   SPRD_RTC_ALARM_EN);
	if (ret)
		return ret;

	ret = sprd_rtc_set_secs(rtc, SPRD_RTC_ALARM, secs);
	if (ret)
		return ret;

	if (alrm->enabled) {
		ret = regmap_update_bits(rtc->regmap,
					 rtc->base + SPRD_RTC_INT_EN,
					 SPRD_RTC_ALARM_EN,
					 SPRD_RTC_ALARM_EN);
		if (ret)
			return ret;

		/* unlock the alarm to enable the alarm function. */
		ret = sprd_rtc_lock_alarm(rtc, false);
	} else {
		regmap_update_bits(rtc->regmap,
				   rtc->base + SPRD_RTC_INT_EN,
				   SPRD_RTC_ALARM_EN, 0);

		/*
		 * Lock the alarm function in case fake alarm event will power
		 * up systems.
		 */
		ret = sprd_rtc_lock_alarm(rtc, true);
	}

	return ret;
}

static int sprd_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	struct sprd_rtc *rtc = dev_get_drvdata(dev);
	int ret;

	if (enabled) {
		ret = regmap_update_bits(rtc->regmap,
					 rtc->base + SPRD_RTC_INT_EN,
					 SPRD_RTC_ALARM_EN | SPRD_RTC_AUXALM_EN,
					 SPRD_RTC_ALARM_EN | SPRD_RTC_AUXALM_EN);
		if (ret)
			return ret;

		ret = sprd_rtc_lock_alarm(rtc, false);
	} else {
		regmap_update_bits(rtc->regmap, rtc->base + SPRD_RTC_INT_EN,
				   SPRD_RTC_ALARM_EN | SPRD_RTC_AUXALM_EN, 0);

		ret = sprd_rtc_lock_alarm(rtc, true);
	}

	return ret;
}

static const struct rtc_class_ops sprd_rtc_ops = {
	.read_time = sprd_rtc_read_time,
	.set_time = sprd_rtc_set_time,
	.read_alarm = sprd_rtc_read_alarm,
	.set_alarm = sprd_rtc_set_alarm,
	.alarm_irq_enable = sprd_rtc_alarm_irq_enable,
};

static int sprd_rtc_dbg_show(struct seq_file *m, void *unused)
{
	struct sprd_rtc *rtc = m->private;
	u32 v[4], w[4], val, i;
	static const struct { const char *n; u32 reg; } regs[] = {
		{ "INT_EN", SPRD_RTC_INT_EN }, { "INT_RAW_STS", SPRD_RTC_INT_RAW_STS },
		{ "INT_MASK_STS", SPRD_RTC_INT_MASK_STS },
		{ "SPG_VALUE", SPRD_RTC_SPG_VALUE }, { "PWR_CTRL", SPRD_RTC_PWR_CTRL },
		{ "PWR_STS", SPRD_RTC_PWR_STS },
		{ "SEC_ALM", SPRD_RTC_SEC_ALM_VALUE }, { "MIN_ALM", SPRD_RTC_MIN_ALM_VALUE },
		{ "HOUR_ALM", SPRD_RTC_HOUR_ALM_VALUE }, { "DAY_ALM", SPRD_RTC_DAY_ALM_VALUE },
	};
	static const u32 cnt[4] = { SPRD_RTC_SEC_CNT_VALUE, SPRD_RTC_MIN_CNT_VALUE,
				    SPRD_RTC_HOUR_CNT_VALUE, SPRD_RTC_DAY_CNT_VALUE };

	seq_printf(m, "valid=%d irq=%d base=0x%x poll_timeout_us=%u poll_delay_us=%u upd_int_en=%d\n",
		   rtc->valid, rtc->irq, rtc->base, poll_timeout_us, poll_delay_us, upd_int_en);
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		int ret = regmap_read(rtc->regmap, rtc->base + regs[i].reg, &val);

		seq_printf(m, "%-13s [0x%02x] = ", regs[i].n, regs[i].reg);
		if (ret)
			seq_printf(m, "read error %d\n", ret);
		else
			seq_printf(m, "0x%08x\n", val);
	}
	for (i = 0; i < 4; i++)
		regmap_read(rtc->regmap, rtc->base + cnt[i], &v[i]);
	/* does the counter tick? */
	msleep(2100);
	for (i = 0; i < 4; i++)
		regmap_read(rtc->regmap, rtc->base + cnt[i], &w[i]);
	seq_printf(m, "counter now  sec=%u min=%u hour=%u day=%u\n", w[0], w[1], w[2], w[3]);
	seq_printf(m, "counter 2.1s before sec=%u min=%u hour=%u day=%u -> %s\n", v[0], v[1], v[2], v[3],
		   (v[0] != w[0] || v[1] != w[1]) ? "TICKING" : "STUCK");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sprd_rtc_dbg);

/*
 * Experiment hook: set module/clock enable bits in the PMIC global block.
 * Only MODULE_EN0 (0x1808) and CLK_EN0 (0x1810) and only the bits the stock
 * U-Boot itself sets at boot for other blocks (bit1/2/3) are accepted: this
 * only ever enables a clock/module, it never clears or reprograms anything.
 * usage: echo "0x1808 0x8" > /sys/kernel/debug/sprd-rtc/glb_set
 */
static ssize_t sprd_rtc_glb_set_write(struct file *file, const char __user *ubuf,
				      size_t len, loff_t *ppos)
{
	struct sprd_rtc *rtc = file->private_data;
	unsigned int reg, mask;
	char buf[40];
	int ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	if (sscanf(buf, "%i %i", &reg, &mask) != 2)
		return -EINVAL;
	if ((reg != 0x1808 && reg != 0x1810) || !mask || (mask & ~0xeU))
		return -EPERM;
	ret = regmap_update_bits(rtc->regmap, reg, mask, mask);
	if (ret)
		return ret;
	dev_info(rtc->dev, "glb_set: reg 0x%x |= 0x%x\n", reg, mask);
	return len;
}

static const struct file_operations sprd_rtc_glb_set_fops = {
	.open = simple_open,
	.write = sprd_rtc_glb_set_write,
	.llseek = noop_llseek,
};

/* read-only dump of the PMIC global block (module / clock enables live here) */
static int sprd_rtc_glb_show(struct seq_file *m, void *unused)
{
	struct sprd_rtc *rtc = m->private;
	u32 off, val;

	for (off = 0x1800; off <= 0x1860; off += 4) {
		if (regmap_read(rtc->regmap, off, &val))
			seq_printf(m, "[0x%04x] read error\n", off);
		else
			seq_printf(m, "[0x%04x] = 0x%08x\n", off, val);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sprd_rtc_glb);

static void sprd_rtc_dbg_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static irqreturn_t sprd_rtc_handler(int irq, void *dev_id)
{
	struct sprd_rtc *rtc = dev_id;
	int ret;

	ret = sprd_rtc_clear_alarm_ints(rtc);
	if (ret)
		return IRQ_RETVAL(ret);

	rtc_update_irq(rtc->rtc, 1, RTC_AF | RTC_IRQF);
	return IRQ_HANDLED;
}

static int sprd_rtc_check_power_down(struct sprd_rtc *rtc)
{
	u32 val;
	int ret;

	ret = regmap_read(rtc->regmap, rtc->base + SPRD_RTC_PWR_STS, &val);
	if (ret)
		return ret;

	/*
	 * If the RTC power status value is SPRD_RTC_POWER_RESET_VALUE, which
	 * means the RTC has been powered down, so the RTC time values are
	 * invalid.
	 */
	rtc->valid = val != SPRD_RTC_POWER_RESET_VALUE;
	return 0;
}

static int sprd_rtc_check_alarm_int(struct sprd_rtc *rtc)
{
	u32 val;
	int ret;

	ret = regmap_read(rtc->regmap, rtc->base + SPRD_RTC_SPG_VALUE, &val);
	if (ret)
		return ret;

	/*
	 * The SPRD_RTC_INT_EN register is not put in always-power-on region
	 * supplied by VDDRTC, so we should check if we need enable the alarm
	 * interrupt when system booting.
	 *
	 * If we have set SPRD_RTC_POWEROFF_ALM_FLAG which is saved in
	 * always-power-on region, that means we have set one alarm last time,
	 * so we should enable the alarm interrupt to help RTC core to see if
	 * there is an alarm already set in RTC hardware.
	 */
	if (!(val & SPRD_RTC_POWEROFF_ALM_FLAG))
		return 0;

	return regmap_update_bits(rtc->regmap, rtc->base + SPRD_RTC_INT_EN,
				  SPRD_RTC_ALARM_EN, SPRD_RTC_ALARM_EN);
}

static int sprd_rtc_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	const struct sprd_rtc_variant *var;
	struct sprd_rtc *rtc;
	int ret;

	rtc = devm_kzalloc(&pdev->dev, sizeof(*rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	rtc->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!rtc->regmap)
		return -ENODEV;

	ret = of_property_read_u32(node, "reg", &rtc->base);
	if (ret) {
		dev_err(&pdev->dev, "failed to get RTC base address\n");
		return ret;
	}

	rtc->irq = platform_get_irq(pdev, 0);
	if (rtc->irq < 0)
		return rtc->irq;

	rtc->rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc->rtc))
		return PTR_ERR(rtc->rtc);

	rtc->dev = &pdev->dev;
	platform_set_drvdata(pdev, rtc);

	var = of_device_get_match_data(&pdev->dev);
	if (var) {
		ret = regmap_update_bits(rtc->regmap, var->module_en, var->en_bit, var->en_bit);
		if (!ret)
			ret = regmap_update_bits(rtc->regmap, var->clk_en, var->en_bit, var->en_bit);
		if (ret) {
			dev_err(&pdev->dev, "failed to enable the RTC module/clock: %d\n", ret);
			return ret;
		}
		/* let the block come out of reset before the first access */
		usleep_range(1000, 2000);
	}

	/* check if we need set the alarm interrupt */
	ret = sprd_rtc_check_alarm_int(rtc);
	if (ret) {
		dev_err(&pdev->dev, "failed to check RTC alarm interrupt\n");
		return ret;
	}

	/* check if RTC time values are valid */
	ret = sprd_rtc_check_power_down(rtc);
	if (ret) {
		dev_err(&pdev->dev, "failed to check RTC time values\n");
		return ret;
	}

	ret = devm_request_threaded_irq(&pdev->dev, rtc->irq, NULL,
					sprd_rtc_handler,
					IRQF_ONESHOT | IRQF_EARLY_RESUME,
					pdev->name, rtc);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to request RTC irq\n");
		return ret;
	}

	device_init_wakeup(&pdev->dev, 1);

	rtc->dbg = debugfs_create_dir("sprd-rtc", NULL);
	debugfs_create_file("regs", 0444, rtc->dbg, rtc, &sprd_rtc_dbg_fops);
	debugfs_create_file("glb", 0444, rtc->dbg, rtc, &sprd_rtc_glb_fops);
	debugfs_create_file("glb_set", 0200, rtc->dbg, rtc, &sprd_rtc_glb_set_fops);
	devm_add_action_or_reset(&pdev->dev, sprd_rtc_dbg_remove, rtc->dbg);
	sprd_rtc_dump(rtc, "probe");

	rtc->rtc->ops = &sprd_rtc_ops;
	rtc->rtc->range_min = 0;
	rtc->rtc->range_max = 5662310399LL;
	ret = devm_rtc_register_device(rtc->rtc);
	if (ret) {
		device_init_wakeup(&pdev->dev, 0);
		return ret;
	}

	return 0;
}

static const struct of_device_id sprd_rtc_of_match[] = {
	{ .compatible = "sprd,sc2730-rtc", .data = &sc2730_variant },
	{ .compatible = "sprd,sc2721-rtc", },
	{ .compatible = "sprd,sc2731-rtc", },
	{ },
};
MODULE_DEVICE_TABLE(of, sprd_rtc_of_match);

static struct platform_driver sprd_rtc_driver = {
	.driver = {
		.name = "sprd-rtc",
		.of_match_table = sprd_rtc_of_match,
	},
	.probe	= sprd_rtc_probe,
};
module_platform_driver(sprd_rtc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Spreadtrum RTC Device Driver");
MODULE_AUTHOR("Baolin Wang <baolin.wang@spreadtrum.com>");
