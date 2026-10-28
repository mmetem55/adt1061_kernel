// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 Spreadtrum Communications Inc.
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "timer-of.h"

#define TIMER_NAME		"sprd_timer"

#define TIMER_LOAD_LO		0x0
#define TIMER_LOAD_HI		0x4
#define TIMER_VALUE_LO		0x8
#define TIMER_VALUE_HI		0xc

#define TIMER_CTL		0x10
#define TIMER_CTL_PERIOD_MODE	BIT(0)
#define TIMER_CTL_ENABLE	BIT(1)
#define TIMER_CTL_64BIT_WIDTH	BIT(16)

#define TIMER_INT		0x14
#define TIMER_INT_EN		BIT(0)
#define TIMER_INT_RAW_STS	BIT(1)
#define TIMER_INT_MASK_STS	BIT(2)
#define TIMER_INT_CLR		BIT(3)

#define TIMER_VALUE_SHDW_LO	0x18
#define TIMER_VALUE_SHDW_HI	0x1c

#define TIMER_VALUE_LO_MASK	GENMASK(31, 0)

static void sprd_timer_enable(void __iomem *base, u32 flag)
{
	u32 val = readl_relaxed(base + TIMER_CTL);

	val |= TIMER_CTL_ENABLE;
	if (flag & TIMER_CTL_64BIT_WIDTH)
		val |= TIMER_CTL_64BIT_WIDTH;
	else
		val &= ~TIMER_CTL_64BIT_WIDTH;

	if (flag & TIMER_CTL_PERIOD_MODE)
		val |= TIMER_CTL_PERIOD_MODE;
	else
		val &= ~TIMER_CTL_PERIOD_MODE;

	writel_relaxed(val, base + TIMER_CTL);
}

static void sprd_timer_disable(void __iomem *base)
{
	u32 val = readl_relaxed(base + TIMER_CTL);

	val &= ~TIMER_CTL_ENABLE;
	writel_relaxed(val, base + TIMER_CTL);
}

static void sprd_timer_update_counter(void __iomem *base, unsigned long cycles)
{
	writel_relaxed(cycles & TIMER_VALUE_LO_MASK, base + TIMER_LOAD_LO);
	writel_relaxed(0, base + TIMER_LOAD_HI);
}

static void sprd_timer_enable_interrupt(void __iomem *base)
{
	writel_relaxed(TIMER_INT_EN, base + TIMER_INT);
}

static void sprd_timer_clear_interrupt(void __iomem *base)
{
	u32 val = readl_relaxed(base + TIMER_INT);

	val |= TIMER_INT_CLR;
	writel_relaxed(val, base + TIMER_INT);
}

static int sprd_timer_set_next_event(unsigned long cycles,
									 struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_disable(timer_of_base(to));
	sprd_timer_update_counter(timer_of_base(to), cycles);
	sprd_timer_enable(timer_of_base(to), 0);

	return 0;
}

static int sprd_timer_set_periodic(struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_disable(timer_of_base(to));
	sprd_timer_update_counter(timer_of_base(to), timer_of_period(to));
	sprd_timer_enable(timer_of_base(to), TIMER_CTL_PERIOD_MODE);

	return 0;
}

static int sprd_timer_shutdown(struct clock_event_device *ce)
{
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_disable(timer_of_base(to));
	return 0;
}

static irqreturn_t sprd_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = (struct clock_event_device *)dev_id;
	struct timer_of *to = to_timer_of(ce);

	sprd_timer_clear_interrupt(timer_of_base(to));

	if (clockevent_state_oneshot(ce))
		sprd_timer_disable(timer_of_base(to));

	ce->event_handler(ce);
	return IRQ_HANDLED;
}

static struct timer_of to = {
	.flags = TIMER_OF_IRQ | TIMER_OF_BASE | TIMER_OF_CLOCK,

	.clkevt = {
		.name = TIMER_NAME,
		.rating = 300,
		.features = CLOCK_EVT_FEAT_DYNIRQ | CLOCK_EVT_FEAT_PERIODIC |
		CLOCK_EVT_FEAT_ONESHOT,
		.set_state_shutdown = sprd_timer_shutdown,
		.set_state_periodic = sprd_timer_set_periodic,
		.set_next_event = sprd_timer_set_next_event,
		.cpumask = cpu_possible_mask,
	},

	.of_irq = {
		.handler = sprd_timer_interrupt,
		.flags = IRQF_TIMER | IRQF_IRQPOLL,
	},
};

static int sprd_timer_probe(struct platform_device *pdev)
{
	int ret;

	ret = timer_of_init(pdev->dev.of_node, &to);
	if (ret)
		return ret;

	sprd_timer_enable_interrupt(timer_of_base(&to));
	clockevents_config_and_register(&to.clkevt, timer_of_rate(&to),
									1, UINT_MAX);

	return 0;
}

static struct timer_of suspend_to = {
	.flags = TIMER_OF_BASE | TIMER_OF_CLOCK,
};

static u64 sprd_suspend_timer_read(struct clocksource *cs)
{
	return ~(u64)readl_relaxed(timer_of_base(&suspend_to) +
	TIMER_VALUE_SHDW_LO) & cs->mask;
}

static int sprd_suspend_timer_enable(struct clocksource *cs)
{
	sprd_timer_update_counter(timer_of_base(&suspend_to),
							  TIMER_VALUE_LO_MASK);
	sprd_timer_enable(timer_of_base(&suspend_to), TIMER_CTL_PERIOD_MODE);

	return 0;
}

static void sprd_suspend_timer_disable(struct clocksource *cs)
{
	sprd_timer_disable(timer_of_base(&suspend_to));
}

static struct clocksource suspend_clocksource = {
	.name	= "sprd_suspend_timer",
	.rating	= 200,
	.read	= sprd_suspend_timer_read,
	.enable = sprd_suspend_timer_enable,
	.disable = sprd_suspend_timer_disable,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS | CLOCK_SOURCE_SUSPEND_NONSTOP,
};

static int sprd_suspend_timer_probe(struct platform_device *pdev)
{
	int ret;

	ret = timer_of_init(pdev->dev.of_node, &suspend_to);
	if (ret)
		return ret;

	/*
	 * The ARM generic counter stops in suspend on this SoC
	 * ("arm,no-tick-in-suspend"), so this always-on counter is what the
	 * timekeeping core uses to account the time spent suspended.
	 */
	return clocksource_register_hz(&suspend_clocksource,
				       timer_of_rate(&suspend_to));
}

/*
 * TIMER_OF_DECLARE() is only processed for built-in drivers, so the
 * timers are bound through ordinary platform drivers when the module is
 * loaded. They cannot be unbound or unloaded once registered.
 */
static const struct of_device_id sprd_timer_of_match[] = {
	{ .compatible = "sprd,sc9860-timer" },
	{ .compatible = "sprd,sharkl5-timer" },
	{ .compatible = "sprd,roc1-timer" },
	{ .compatible = "sprd,sharkl3-timer" },
	{ .compatible = "sprd,orca-timer" },
	{ .compatible = "sprd,sharkle-timer" },
	{ .compatible = "sprd,pike2-timer" },
	{ .compatible = "sprd,sharkl5Pro-timer" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_timer_of_match);

static const struct of_device_id sprd_suspend_timer_of_match[] = {
	{ .compatible = "sprd,sc9860-suspend-timer" },
	{ .compatible = "sprd,sharkl5-suspend-timer" },
	{ .compatible = "sprd,roc1-suspend-timer" },
	{ .compatible = "sprd,sharkl3-suspend-timer" },
	{ .compatible = "sprd,orca-suspend-timer" },
	{ .compatible = "sprd,sharkle-suspend-timer" },
	{ .compatible = "sprd,pike2-suspend-timer" },
	{ .compatible = "sprd,sharkl5Pro-suspend-timer" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_suspend_timer_of_match);

static struct platform_driver sprd_timer_driver = {
	.probe = sprd_timer_probe,
	.driver = {
		.name = "sprd-timer",
		.of_match_table = sprd_timer_of_match,
		.suppress_bind_attrs = true,
	},
};

static struct platform_driver sprd_suspend_timer_driver = {
	.probe = sprd_suspend_timer_probe,
	.driver = {
		.name = "sprd-suspend-timer",
		.of_match_table = sprd_suspend_timer_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init sprd_timer_module_init(void)
{
	int ret;

	ret = platform_driver_register(&sprd_suspend_timer_driver);
	if (ret)
		return ret;

	return platform_driver_register(&sprd_timer_driver);
}
module_init(sprd_timer_module_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Unisoc");
MODULE_DESCRIPTION("Spreadtrum Timer Driver");
