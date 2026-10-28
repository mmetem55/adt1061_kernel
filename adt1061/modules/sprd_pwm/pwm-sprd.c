// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Spreadtrum Communications Inc.
 *
 * GKI 6.12 port of the 4.14 vendor driver. The in-tree pwm-sprd.c only binds
 * "sprd,ums512-pwm" with "pwmN"/"enableN" clocks; this board's DT keeps the
 * vendor "sprd,sharkl5-pwm" binding with "clk_parent"/"clk_pwmN"/"clk_pwmN_eb".
 */
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define NUM_PWM		4
#define PWM_REGS_SHIFT	5
#define PWM_MOD_MAX	GENMASK(9, 0)
#define PWM_REG_MSK	GENMASK(15, 0)

#define PWM_PRESCALE	0x0
#define PWM_MOD		0x4
#define PWM_DUTY	0x8
#define PWM_PAT_LOW	0x10
#define PWM_PAT_HIGH	0x14
#define PWM_ENABLE	0x18

#define BIT_ENABLE	BIT(0)
#define PWM_CLK_PARENT	"clk_parent"
#define PWM_CLK		"clk_pwm"

struct sprd_pwm_chip {
	void __iomem *mmio_base;
	struct clk *clk_pwm[NUM_PWM];
	struct clk *clk_eb[NUM_PWM];
	bool eb_enabled[NUM_PWM];
	bool pwm_enabled[NUM_PWM];
};

static inline struct sprd_pwm_chip *to_sprd_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static inline u32 sprd_pwm_readl(struct sprd_pwm_chip *spc, u32 num, u32 offset)
{
	return readl_relaxed(spc->mmio_base + (num << PWM_REGS_SHIFT) + offset);
}

static inline void sprd_pwm_writel(struct sprd_pwm_chip *spc, u32 num,
				   u32 offset, u32 val)
{
	writel_relaxed(val, spc->mmio_base + (num << PWM_REGS_SHIFT) + offset);
}

static int sprd_pwm_eb_get(struct pwm_chip *chip, unsigned int num)
{
	struct sprd_pwm_chip *spc = to_sprd_pwm(chip);
	int rc;

	if (spc->eb_enabled[num])
		return 0;

	rc = clk_prepare_enable(spc->clk_eb[num]);
	if (rc) {
		dev_err(pwmchip_parent(chip), "enable pwm%u failed\n", num);
		return rc;
	}
	spc->eb_enabled[num] = true;

	return 0;
}

static int sprd_pwm_config(struct pwm_chip *chip, unsigned int num,
			   u64 duty_ns, u64 period_ns)
{
	struct sprd_pwm_chip *spc = to_sprd_pwm(chip);
	u64 clk_rate, div, val;
	int rc, prescale, level;

	rc = sprd_pwm_eb_get(chip, num);
	if (rc)
		return rc;

	level = DIV_ROUND_CLOSEST_ULL(duty_ns * PWM_MOD_MAX, period_ns);

	clk_rate = clk_get_rate(spc->clk_pwm[num]);
	/*
	 * period_ns = 10^9 * (PRESCALE + 1) * PV / PWM_CLK_RATE
	 * duty_ns   = 10^9 * (PRESCALE + 1) * DC / PWM_CLK_RATE
	 */
	div = (u64)NSEC_PER_SEC * PWM_MOD_MAX;
	val = clk_rate * period_ns;
	prescale = div64_u64(val, div) - 1;
	if (prescale < 0)
		prescale = 0;

	sprd_pwm_writel(spc, num, PWM_MOD, PWM_MOD_MAX);
	sprd_pwm_writel(spc, num, PWM_DUTY, level);
	sprd_pwm_writel(spc, num, PWM_PAT_LOW, PWM_REG_MSK);
	sprd_pwm_writel(spc, num, PWM_PAT_HIGH, PWM_REG_MSK);
	sprd_pwm_writel(spc, num, PWM_PRESCALE, prescale);

	return 0;
}

static int sprd_pwm_enable(struct pwm_chip *chip, unsigned int num)
{
	struct sprd_pwm_chip *spc = to_sprd_pwm(chip);
	int rc;

	if (spc->pwm_enabled[num])
		return 0;

	rc = sprd_pwm_eb_get(chip, num);
	if (rc)
		return rc;

	rc = clk_prepare_enable(spc->clk_pwm[num]);
	if (rc) {
		dev_err(pwmchip_parent(chip), "enable pwm%u clock failed\n", num);
		clk_disable_unprepare(spc->clk_eb[num]);
		spc->eb_enabled[num] = false;
		return rc;
	}
	spc->pwm_enabled[num] = true;

	sprd_pwm_writel(spc, num, PWM_ENABLE, 1);

	return 0;
}

static void sprd_pwm_disable(struct pwm_chip *chip, unsigned int num)
{
	struct sprd_pwm_chip *spc = to_sprd_pwm(chip);

	sprd_pwm_writel(spc, num, PWM_ENABLE, 0);
	if (spc->pwm_enabled[num]) {
		clk_disable_unprepare(spc->clk_pwm[num]);
		spc->pwm_enabled[num] = false;
	}
	if (spc->eb_enabled[num]) {
		clk_disable_unprepare(spc->clk_eb[num]);
		spc->eb_enabled[num] = false;
	}
}

static int sprd_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			  const struct pwm_state *state)
{
	int rc;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		sprd_pwm_disable(chip, pwm->hwpwm);
		return 0;
	}

	/* The original driver left the registers untouched for duty == 0. */
	if (state->duty_cycle) {
		rc = sprd_pwm_config(chip, pwm->hwpwm, state->duty_cycle,
				     state->period);
		if (rc)
			return rc;
	}

	return sprd_pwm_enable(chip, pwm->hwpwm);
}

static int sprd_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			      struct pwm_state *state)
{
	struct sprd_pwm_chip *spc = to_sprd_pwm(chip);
	unsigned int num = pwm->hwpwm;
	u32 enabled, duty, prescale;
	u64 clk_rate, val;
	int rc;

	rc = sprd_pwm_eb_get(chip, num);
	if (rc)
		return rc;

	rc = clk_prepare_enable(spc->clk_pwm[num]);
	if (rc) {
		clk_disable_unprepare(spc->clk_eb[num]);
		spc->eb_enabled[num] = false;
		dev_err(pwmchip_parent(chip), "enable pwm%u clk failed\n", num);
		return rc;
	}

	duty = sprd_pwm_readl(spc, num, PWM_DUTY) & PWM_REG_MSK;
	prescale = sprd_pwm_readl(spc, num, PWM_PRESCALE) & PWM_REG_MSK;
	enabled = sprd_pwm_readl(spc, num, PWM_ENABLE) & BIT_ENABLE;

	clk_rate = clk_get_rate(spc->clk_pwm[num]);
	if (clk_rate) {
		val = ((u64)prescale + 1) * NSEC_PER_SEC * PWM_MOD_MAX;
		state->period = div64_u64(val, clk_rate);
		val = ((u64)prescale + 1) * NSEC_PER_SEC * duty;
		state->duty_cycle = div64_u64(val, clk_rate);
	} else {
		dev_err(pwmchip_parent(chip), "get pwm%u clk rate failed\n", num);
	}
	state->polarity = PWM_POLARITY_NORMAL;
	state->enabled = !!enabled;

	if (enabled) {
		/* keep the clocks the hardware is already running on */
		spc->pwm_enabled[num] = true;
	} else {
		clk_disable_unprepare(spc->clk_pwm[num]);
		clk_disable_unprepare(spc->clk_eb[num]);
		spc->eb_enabled[num] = false;
	}

	return 0;
}

static const struct pwm_ops sprd_pwm_ops = {
	.apply = sprd_pwm_apply,
	.get_state = sprd_pwm_get_state,
};

static int sprd_pwm_clk_init(struct platform_device *pdev,
			     struct sprd_pwm_chip *spc)
{
	struct clk *clk_parent;
	char clk_name[64];
	int i;

	clk_parent = devm_clk_get(&pdev->dev, PWM_CLK_PARENT);
	if (IS_ERR(clk_parent))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk_parent),
				     "get clk parent failed\n");

	for (i = 0; i < NUM_PWM; i++) {
		snprintf(clk_name, sizeof(clk_name), PWM_CLK "%d", i);
		spc->clk_pwm[i] = devm_clk_get(&pdev->dev, clk_name);
		if (IS_ERR(spc->clk_pwm[i])) {
			if (PTR_ERR(spc->clk_pwm[i]) == -ENOENT)
				break;

			return dev_err_probe(&pdev->dev, PTR_ERR(spc->clk_pwm[i]),
					     "get clk %d failed\n", i);
		}

		snprintf(clk_name, sizeof(clk_name), PWM_CLK "%d_eb", i);
		spc->clk_eb[i] = devm_clk_get(&pdev->dev, clk_name);
		if (IS_ERR(spc->clk_eb[i]))
			return dev_err_probe(&pdev->dev, PTR_ERR(spc->clk_eb[i]),
					     "get clk_eb %d failed\n", i);

		clk_set_parent(spc->clk_pwm[i], clk_parent);
	}

	return i;
}

static const struct of_device_id sprd_pwm_of_match[] = {
	{ .compatible = "sprd,sharkl5-pwm", },
	{ },
};
MODULE_DEVICE_TABLE(of, sprd_pwm_of_match);

static int sprd_pwm_probe(struct platform_device *pdev)
{
	struct sprd_pwm_chip *spc;
	struct sprd_pwm_chip tmp = { };
	struct pwm_chip *chip;
	int npwm;

	npwm = sprd_pwm_clk_init(pdev, &tmp);
	if (npwm < 0)
		return npwm;
	if (!npwm)
		return dev_err_probe(&pdev->dev, -ENODEV, "no PWM channels\n");

	chip = devm_pwmchip_alloc(&pdev->dev, npwm, sizeof(*spc));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	spc = to_sprd_pwm(chip);
	*spc = tmp;

	spc->mmio_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spc->mmio_base))
		return PTR_ERR(spc->mmio_base);

	chip->ops = &sprd_pwm_ops;

	return devm_pwmchip_add(&pdev->dev, chip);
}

static struct platform_driver sprd_pwm_driver = {
	.driver = {
		.name = "sprd-pwm",
		.of_match_table = sprd_pwm_of_match,
	},
	.probe = sprd_pwm_probe,
};
module_platform_driver(sprd_pwm_driver);

MODULE_DESCRIPTION("Spreadtrum PWM Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Neo Hou <neo.hou@unisoc.com>");
