// SPDX-License-Identifier: GPL-2.0
/*
 * Unisoc UMS512 (sharkl5pro) Mali-G52 power domain and DVFS clock.
 *
 * The vendor Mali kbase platform code powers the GPU with a fixed sequence
 * that does not fit the generic panfrost driver: clear the GPU top
 * force-shutdown bit, enable the PLL gates and GPU gates, then program the
 * hardware DVFS gear maps, the DVFS index and the QoS registers. This genpd
 * provider performs exactly that sequence so that panfrost only needs
 * `power-domains`.
 *
 * The GPU core frequency is not a clk-framework rate on this SoC: it is a
 * DVFS *index* written to a register, and the hardware switches clock source,
 * divider and voltage by itself. The same node therefore provides a clock
 * ("gpu core") whose set_rate() maps a rate from the vendor `sprd,dvfs-lists`
 * to that index, which lets panfrost devfreq and the thermal framework drive
 * the GPU exactly like the vendor devfreq code did.
 *
 * Every register, mask, clock, the DVFS list and the default index are read
 * from the GPU node referenced by the "sprd,gpu" phandle (the vendor DT
 * properties); nothing about the hardware is hard-coded here. The silicon
 * variant (efuse "auto/efuse": T618/T610/T700) and bin select the vendor's
 * gear maps and frequency limit; when the bootloader does not report it the
 * driver stays on the gears common to every variant (see below).
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define HWFEATURE_STR_SIZE_LIMIT	64
void sprd_kproperty_get(const char *key, char *value, const char *default_value);

/* indexes into the GPU node "clocks" list: entry 0 is this node's own clock */
#define GPU_CLK_OFFSET		1
#define GPU_CLK_I		(0 + GPU_CLK_OFFSET)	/* AON gpu-eb gate */
#define GPU_CLK_CORE_EB		(1 + GPU_CLK_OFFSET)
#define GPU_CLK_MEM_EB		(2 + GPU_CLK_OFFSET)
#define GPU_CLK_SYS_EB		(3 + GPU_CLK_OFFSET)
#define GPU_CLK_FIRST_SRC	(4 + GPU_CLK_OFFSET)	/* PLL gates follow */

#define DVFS_LIST_ENTRY		4	/* kHz, uV, clk index, div */
#define MAX_OPPS		16

/*
 * Gears that exist on every variant. When the variant is unknown the vendor
 * code (which then sees "-1") would still use all gears with the default gear
 * maps; the 768/850 MHz gears need a variant specific voltage map, so they are
 * held back until the variant is known (module parameter or /hwfeature).
 */
#define SAFE_MAX_INDEX		2

static char *efuse_type;
module_param(efuse_type, charp, 0444);
MODULE_PARM_DESC(efuse_type,
		 "silicon variant (T618, T610, T700); default: /hwfeature auto/efuse");

struct sprd_reg {
	struct regmap *map;
	u32 off;
	u32 mask;
};

struct sprd_gpu_pd {
	struct generic_pm_domain genpd;
	struct device *dev;

	struct sprd_reg top_force;
	struct sprd_reg qos_sel;
	struct sprd_reg qos;
	struct sprd_reg dvfs_index;
	struct sprd_reg freq_upd;
	struct sprd_reg core_map[8];
	bool have_core_map[8];

	struct clk **clks;	/* in power-on order */
	int nclks;

	u32 dvfs_default;
	u32 qos_val;
	u32 gpu_bin;
	char variant[HWFEATURE_STR_SIZE_LIMIT];

	/* DVFS list from the vendor DT, frequencies in kHz */
	u32 freq_khz[MAX_OPPS];
	u32 volt_uv[MAX_OPPS];
	int nfreq;
	int max_index;

	struct clk_hw hw;
	spinlock_t lock;	/* cur_index, powered and the index register */
	bool powered;
	int cur_index;
};

#define to_pd(g) container_of(g, struct sprd_gpu_pd, genpd)
#define hw_to_pd(h) container_of(h, struct sprd_gpu_pd, hw)

static int sprd_get_reg(struct device_node *np, const char *name,
			struct sprd_reg *r)
{
	struct of_phandle_args args;
	int idx = of_property_match_string(np, "syscon-names", name);
	int ret;

	if (idx < 0)
		return idx;
	ret = of_parse_phandle_with_fixed_args(np, "syscons", 2, idx, &args);
	if (ret)
		return ret;
	r->map = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(r->map))
		return PTR_ERR(r->map);
	r->off = args.args[0];
	r->mask = args.args[1];
	return 0;
}

static void sprd_reg_set(const struct sprd_reg *r)
{
	regmap_update_bits(r->map, r->off, r->mask, r->mask);
}

static void sprd_reg_clear(const struct sprd_reg *r)
{
	regmap_update_bits(r->map, r->off, r->mask, 0);
}

/* gear maps, as the vendor platform code does per silicon variant and bin */
static void sprd_gpu_pd_gear_maps(struct sprd_gpu_pd *pd)
{
	const char *v = pd->variant;
	int i;

	if (!strcmp(v, "T618")) {
		if (pd->gpu_bin == 1) {
			/* BIN1 FF: 512M at 0.7V */
			sprd_reg_clear(&pd->core_map[1]);
		} else if (pd->gpu_bin == 2) {
			/* BIN2 TT: 512M 0.7V, 768M/850M 0.85V */
			sprd_reg_clear(&pd->core_map[1]);
			for (i = 4; i <= 7; i++)
				sprd_reg_set(&pd->core_map[i]);
		}
	} else if (!strcmp(v, "T610")) {
		/* 512M at 0.7V */
		sprd_reg_clear(&pd->core_map[1]);
	} else if (!strcmp(v, "T700")) {
		/* 850M at 0.85V */
		for (i = 4; i <= 7; i++)
			sprd_reg_set(&pd->core_map[i]);
	}
}

/* PMU_APB PWR_STATUS3_DBG (0x10c), PD_GPU_TOP = bits 15:8 (vendor ums512.dtsi) */
static void sprd_gpu_pd_log_state(struct sprd_gpu_pd *pd, const char *what)
{
	unsigned int v;

	if (!regmap_read(pd->top_force.map, 0x10c, &v))
		dev_info(pd->dev, "%s: PD_GPU_TOP state 0x%x (status3 0x%08x)\n",
			 what, (v >> 8) & 0xff, v);
}

/*
 * PD_GPU_TOP states seen on hardware (T610): 0x0 powered, 0x7 shut down,
 * 0xd in between while shutting down. Returns the last state read.
 */
#define GPU_TOP_ON	0x0
#define GPU_TOP_OFF	0x7

static int sprd_gpu_top_state(struct sprd_gpu_pd *pd)
{
	unsigned int v;

	if (regmap_read(pd->top_force.map, 0x10c, &v))
		return -1;
	return (v >> 8) & 0xff;
}

/* wait (<= timeout_us) until PD_GPU_TOP is in one of the two stable states */
static int sprd_gpu_top_wait(struct sprd_gpu_pd *pd, int want, int any_stable,
			     unsigned int timeout_us, unsigned int *took_us)
{
	unsigned int t = 0;
	int st;

	for (;;) {
		st = sprd_gpu_top_state(pd);
		if (st == want || (any_stable && (st == GPU_TOP_ON || st == GPU_TOP_OFF)))
			break;
		if (t >= timeout_us)
			break;
		udelay(50);
		t += 50;
	}
	*took_us = t;
	return st;
}

static int sprd_gpu_pd_power_on(struct generic_pm_domain *genpd)
{
	struct sprd_gpu_pd *pd = to_pd(genpd);
	unsigned long flags;
	unsigned int v, took;
	int i, ret, st;

	/* clear the GPU top force-shutdown bit */
	/* a previous shutdown may still be in flight (state 0xd): let it finish */
	st = sprd_gpu_top_wait(pd, GPU_TOP_OFF, 1, 20000, &took);
	if (st != GPU_TOP_OFF && st != GPU_TOP_ON)
		dev_warn(pd->dev, "power-on: PD_GPU_TOP still 0x%x after %u us\n", st, took);
	sprd_gpu_pd_log_state(pd, "power-on: before release");
	sprd_reg_clear(&pd->top_force);
	/*
	 * The vendor waits 300 us blindly, but the first gate access before the
	 * domain is up takes a bus abort (UART-paced debug prints used to hide
	 * this). Wait for the domain to report powered (0x0).
	 */
	st = sprd_gpu_top_wait(pd, GPU_TOP_ON, 0, 30000, &took);
	if (st != GPU_TOP_ON) {
		dev_err(pd->dev, "power-on: PD_GPU_TOP 0x%x after %u us, not powering clocks\n",
			st, took);
		sprd_reg_set(&pd->top_force);
		return -ETIMEDOUT;
	}
	dev_dbg(pd->dev, "power-on: PD_GPU_TOP up after %u us\n", took);
	udelay(100);
	if (!regmap_read(pd->top_force.map, pd->top_force.off, &v))
		dev_dbg(pd->dev, "top force-shutdown reg 0x%x = 0x%x (mask 0x%x)\n",
			pd->top_force.off, v, pd->top_force.mask);

	for (i = 0; i < pd->nclks; i++) {
		dev_dbg(pd->dev, "power-on: enabling clock %d/%d %s\n", i + 1,
			pd->nclks, __clk_get_name(pd->clks[i]));
		ret = clk_prepare_enable(pd->clks[i]);
		udelay(100);
		if (ret) {
			dev_err(pd->dev, "clock %d enable failed: %d\n", i, ret);
			while (--i >= 0)
				clk_disable_unprepare(pd->clks[i]);
			regmap_update_bits(pd->top_force.map, pd->top_force.off,
					   pd->top_force.mask, pd->top_force.mask);
			return ret;
		}
	}
	udelay(400);

	sprd_gpu_pd_gear_maps(pd);

	/* hardware DVFS: latch the maps and select the current index */
	regmap_update_bits(pd->freq_upd.map, pd->freq_upd.off,
			   pd->freq_upd.mask, 1);
	spin_lock_irqsave(&pd->lock, flags);
	regmap_update_bits(pd->dvfs_index.map, pd->dvfs_index.off,
			   pd->dvfs_index.mask, pd->cur_index);
	pd->powered = true;
	spin_unlock_irqrestore(&pd->lock, flags);

	/* QoS */
	regmap_update_bits(pd->qos_sel.map, pd->qos_sel.off, pd->qos_sel.mask,
			   pd->qos_sel.mask);
	regmap_update_bits(pd->qos.map, pd->qos.off, pd->qos.mask, pd->qos_val);

	udelay(200);
	return 0;
}

static int sprd_gpu_pd_power_off(struct generic_pm_domain *genpd)
{
	struct sprd_gpu_pd *pd = to_pd(genpd);
	unsigned long flags;
	int i;

	spin_lock_irqsave(&pd->lock, flags);
	pd->powered = false;
	spin_unlock_irqrestore(&pd->lock, flags);

	/* gates first (reverse of enable), then the force-shutdown bit */
	for (i = pd->nclks - 1; i >= 0; i--)
		clk_disable_unprepare(pd->clks[i]);
	sprd_reg_set(&pd->top_force);
	usleep_range(2000, 2500);
	sprd_gpu_pd_log_state(pd, "power-off: after force shutdown");
	return 0;
}

/* ---- the "gpu core" clock: rate <-> DVFS index ------------------------- */

/* lowest gear whose frequency satisfies the request, capped at max_index */
static int sprd_gpu_rate_to_index(struct sprd_gpu_pd *pd, unsigned long rate)
{
	int i;

	for (i = 0; i <= pd->max_index; i++)
		if ((unsigned long)pd->freq_khz[i] * 1000 >= rate)
			return i;
	return pd->max_index;
}

static unsigned long sprd_gpu_clk_recalc_rate(struct clk_hw *hw,
					      unsigned long parent_rate)
{
	struct sprd_gpu_pd *pd = hw_to_pd(hw);

	return (unsigned long)pd->freq_khz[pd->cur_index] * 1000;
}

static int sprd_gpu_clk_determine_rate(struct clk_hw *hw,
				       struct clk_rate_request *req)
{
	struct sprd_gpu_pd *pd = hw_to_pd(hw);
	int i = sprd_gpu_rate_to_index(pd, req->rate);

	req->rate = (unsigned long)pd->freq_khz[i] * 1000;
	return 0;
}

static int sprd_gpu_clk_set_rate(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct sprd_gpu_pd *pd = hw_to_pd(hw);
	unsigned long flags;
	int idx = sprd_gpu_rate_to_index(pd, rate);

	spin_lock_irqsave(&pd->lock, flags);
	pd->cur_index = idx;
	/* like the vendor code: write the index only while the GPU is up */
	if (pd->powered)
		regmap_update_bits(pd->dvfs_index.map, pd->dvfs_index.off,
				   pd->dvfs_index.mask, idx);
	spin_unlock_irqrestore(&pd->lock, flags);
	return 0;
}

static const struct clk_ops sprd_gpu_clk_ops = {
	.recalc_rate	= sprd_gpu_clk_recalc_rate,
	.determine_rate	= sprd_gpu_clk_determine_rate,
	.set_rate	= sprd_gpu_clk_set_rate,
};

/*
 * Silicon variant, derived exactly like the vendor U-Boot does
 * (arch/arm/cpu/armv8/sharkl5pro/misc.c get_chip_bonding() + the hwfeature
 * rule "auto/efuse=auto.efuse,1:T610,:T618"): AON_APB BOND_OPT0 bits
 * 7/16/18/20/21 select T618 (0), T610 (1) or T700 (2); only code 1 becomes the
 * string "T610", every other code (including T700) is reported as "T618".
 */
static int sprd_gpu_pd_bond_variant(struct device *dev, char *out, size_t len)
{
	struct of_phandle_args args;
	struct regmap *map;
	unsigned int v;
	int code;

	if (of_parse_phandle_with_fixed_args(dev->of_node, "sprd,bond-option", 1, 0, &args))
		return -ENOENT;
	map = syscon_node_to_regmap(args.np);
	of_node_put(args.np);
	if (IS_ERR(map) || regmap_read(map, args.args[0], &v))
		return -EIO;

	if (!(v & BIT(7)) && (v & BIT(16)) && (v & BIT(18)) && (v & BIT(20)) && !(v & BIT(21)))
		code = 1;	/* T610 */
	else if (!(v & BIT(16)) && !(v & BIT(18)) && !(v & BIT(20)) && !(v & BIT(21)))
		code = 0;	/* T618 */
	else if ((v & BIT(16)) && !(v & BIT(18)) && !(v & BIT(20)) && !(v & BIT(21)))
		code = 2;	/* T700 */
	else
		code = -1;	/* BIT_DEFAULT */
	strscpy(out, code == 1 ? "T610" : "T618", len);
	dev_info(dev, "BOND_OPT0 = 0x%08x -> code %d -> \"%s\"\n", v, code, out);
	return 0;
}

static int sprd_gpu_pd_read_dvfs(struct device *dev, struct device_node *gpu,
				 struct sprd_gpu_pd *pd)
{
	int n = of_property_count_elems_of_size(gpu, "sprd,dvfs-lists",
						DVFS_LIST_ENTRY * sizeof(u32));
	int i;

	if (n <= 0 || n > MAX_OPPS)
		return dev_err_probe(dev, -EINVAL, "bad sprd,dvfs-lists (%d)\n", n);
	for (i = 0; i < n; i++) {
		of_property_read_u32_index(gpu, "sprd,dvfs-lists",
					   DVFS_LIST_ENTRY * i, &pd->freq_khz[i]);
		of_property_read_u32_index(gpu, "sprd,dvfs-lists",
					   DVFS_LIST_ENTRY * i + 1, &pd->volt_uv[i]);
	}
	pd->nfreq = n;
	return 0;
}

static int sprd_gpu_pd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *gpu, *qos_np;
	struct sprd_gpu_pd *pd;
	struct clk_init_data init = { };
	struct nvmem_cell *cell;
	u8 arq = 0, awq = 0, arqt = 0, awqt = 0;
	char name[16];
	int ret, i, n, k;

	gpu = of_parse_phandle(dev->of_node, "sprd,gpu", 0);
	if (!gpu)
		return dev_err_probe(dev, -EINVAL, "missing sprd,gpu\n");

	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd) {
		ret = -ENOMEM;
		goto out_put;
	}
	pd->dev = dev;
	spin_lock_init(&pd->lock);

	ret = sprd_get_reg(gpu, "top_force_shutdown", &pd->top_force);
	if (!ret)
		ret = sprd_get_reg(gpu, "gpu_qos_sel", &pd->qos_sel);
	if (!ret)
		ret = sprd_get_reg(gpu, "gpu_qos", &pd->qos);
	if (!ret)
		ret = sprd_get_reg(gpu, "dvfs_index_cfg", &pd->dvfs_index);
	if (!ret)
		ret = sprd_get_reg(gpu, "freq_upd_cfg", &pd->freq_upd);
	for (i = 0; !ret && i < 8; i++) {
		snprintf(name, sizeof(name), "core_index%d_map", i);
		if (!sprd_get_reg(gpu, name, &pd->core_map[i]))
			pd->have_core_map[i] = true;
	}
	if (ret) {
		ret = dev_err_probe(dev, ret, "syscon lookup failed\n");
		goto out_put;
	}

	ret = sprd_gpu_pd_read_dvfs(dev, gpu, pd);
	if (ret)
		goto out_put;
	if (of_property_read_u32(gpu, "sprd,dvfs-default", &pd->dvfs_default) ||
	    pd->dvfs_default >= pd->nfreq)
		pd->dvfs_default = 0;
	pd->cur_index = pd->dvfs_default;

	/* silicon variant: module parameter, then /hwfeature, then BOND_OPT0 */
	if (efuse_type && *efuse_type)
		strscpy(pd->variant, efuse_type, sizeof(pd->variant));
	else
		sprd_kproperty_get("auto/efuse", pd->variant, "-1");
	if (!strcmp(pd->variant, "-1"))
		sprd_gpu_pd_bond_variant(dev, pd->variant, sizeof(pd->variant));

	cell = of_nvmem_cell_get(gpu, "gpu_bin");
	if (!IS_ERR(cell)) {
		size_t len;
		void *buf = nvmem_cell_read(cell, &len);

		if (!IS_ERR(buf)) {
			memcpy(&pd->gpu_bin, buf, min(len, sizeof(u32)));
			kfree(buf);
		}
		nvmem_cell_put(cell);
	} else if (PTR_ERR(cell) == -EPROBE_DEFER) {
		ret = -EPROBE_DEFER;
		goto out_put;
	}

	/* highest gear the variant may use */
	pd->max_index = pd->nfreq - 1;
	if (!strcmp(pd->variant, "T610"))
		pd->max_index = max(0, pd->nfreq - 3);	/* no 768M/850M */
	else if (strcmp(pd->variant, "T618") && strcmp(pd->variant, "T700"))
		pd->max_index = min(pd->max_index, SAFE_MAX_INDEX);
	pd->cur_index = min(pd->cur_index, pd->max_index);

	qos_np = of_parse_phandle(gpu, "sprd,qos", 0);
	if (qos_np) {
		of_property_read_u8(qos_np, "arqos", &arq);
		of_property_read_u8(qos_np, "awqos", &awq);
		of_property_read_u8(qos_np, "arqos-threshold", &arqt);
		of_property_read_u8(qos_np, "awqos-threshold", &awqt);
		of_node_put(qos_np);
	}
	pd->qos_val = (awqt << 12) | (arqt << 8) | (awq << 4) | arq;

	/*
	 * Vendor order: PLL gates, then gpu-eb, then the core/mem/sys gates.
	 * Entry 0 of the GPU node "clocks" is the clock provided here.
	 */
	n = of_count_phandle_with_args(gpu, "clocks", "#clock-cells");
	if (n < GPU_CLK_FIRST_SRC) {
		ret = dev_err_probe(dev, -EINVAL, "gpu node has %d clocks\n", n);
		goto out_put;
	}
	pd->clks = devm_kcalloc(dev, n, sizeof(*pd->clks), GFP_KERNEL);
	if (!pd->clks) {
		ret = -ENOMEM;
		goto out_put;
	}
	k = 0;
	for (i = GPU_CLK_FIRST_SRC; i < n; i++)
		pd->clks[k++] = of_clk_get(gpu, i);
	pd->clks[k++] = of_clk_get(gpu, GPU_CLK_I);
	pd->clks[k++] = of_clk_get(gpu, GPU_CLK_CORE_EB);
	pd->clks[k++] = of_clk_get(gpu, GPU_CLK_MEM_EB);
	pd->clks[k++] = of_clk_get(gpu, GPU_CLK_SYS_EB);
	pd->nclks = k;
	for (i = 0; i < k; i++) {
		if (IS_ERR(pd->clks[i])) {
			ret = dev_err_probe(dev, PTR_ERR(pd->clks[i]),
					    "clock %d\n", i);
			while (--i >= 0)
				clk_put(pd->clks[i]);
			goto out_put;
		}
	}

	init.name = "sprd-gpu-core";
	init.ops = &sprd_gpu_clk_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	pd->hw.init = &init;
	ret = devm_clk_hw_register(dev, &pd->hw);
	if (!ret)
		ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &pd->hw);
	if (ret) {
		dev_err_probe(dev, ret, "gpu core clock\n");
		goto out_put;
	}

	pd->genpd.name = "sprd-gpu";
	pd->genpd.power_on = sprd_gpu_pd_power_on;
	pd->genpd.power_off = sprd_gpu_pd_power_off;
	ret = pm_genpd_init(&pd->genpd, NULL, true);
	if (ret)
		goto out_put;
	ret = of_genpd_add_provider_simple(dev->of_node, &pd->genpd);
	if (ret) {
		pm_genpd_remove(&pd->genpd);
		goto out_put;
	}
	dev_info(dev, "GPU power domain ready: %d clocks, variant \"%s\" bin %u, %d gears (max index %d, default %u, %u..%u kHz), qos 0x%x\n",
		 pd->nclks, pd->variant, pd->gpu_bin, pd->nfreq, pd->max_index,
		 pd->dvfs_default, pd->freq_khz[0], pd->freq_khz[pd->max_index],
		 pd->qos_val);
out_put:
	of_node_put(gpu);
	return ret;
}

static const struct of_device_id sprd_gpu_pd_of_match[] = {
	{ .compatible = "adt1061,sprd-gpu-pd" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_gpu_pd_of_match);

static struct platform_driver sprd_gpu_pd_driver = {
	.probe = sprd_gpu_pd_probe,
	.driver = {
		.name = "sprd-gpu-pd",
		.of_match_table = sprd_gpu_pd_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(sprd_gpu_pd_driver);

MODULE_DESCRIPTION("Unisoc UMS512 Mali-G52 power domain and DVFS clock");
MODULE_LICENSE("GPL");
