/*
 * 2026 GKI 6.12 port: compatibility shim for syscon_regmap_lookup_by_name()
 * and syscon_get_args_by_name().
 *
 * These two helpers come from Spreadtrum's downstream patch to
 * drivers/mfd/syscon.c and are not part of GKI. The downstream binding is:
 *
 *   syscons      = <&regs_a arg0 arg1>, <&regs_b arg0 arg1>;
 *   syscon-names = "enable", "power";
 *
 * where each entry is a phandle plus <reg mask>, and the position of the name in "syscon-names" selects the entry in "syscons".
 * (An earlier version of this shim assumed a per-name property, which does
 * not exist in the DT, so every lookup failed with "failed to map ... glb
 * reg".)
 */
#ifndef _SPRD_SYSCON_COMPAT_H_
#define _SPRD_SYSCON_COMPAT_H_

#include <linux/err.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>

static inline int sprd_syscon_parse_by_name(struct device_node *np,
					     const char *name,
					     struct of_phandle_args *args)
{
	int index = of_property_match_string(np, "syscon-names", name);

	if (index < 0)
		return -EINVAL;

	/* every entry is <phandle reg mask>, independent of #syscon-cells */
	return of_parse_phandle_with_fixed_args(np, "syscons", 2, index, args);
}

static inline struct regmap *syscon_regmap_lookup_by_name(struct device_node *np,
							    const char *name)
{
	struct of_phandle_args args;
	struct regmap *regmap;
	int ret;

	ret = sprd_syscon_parse_by_name(np, name, &args);
	if (ret)
		return ERR_PTR(ret);

	regmap = syscon_node_to_regmap(args.np);
	of_node_put(args.np);

	return regmap;
}

static inline int syscon_get_args_by_name(struct device_node *np, const char *name,
					   int arg_count, unsigned int *out_args)
{
	struct of_phandle_args args;
	int ret, i;

	ret = sprd_syscon_parse_by_name(np, name, &args);
	if (ret)
		return ret;

	if (arg_count > args.args_count)
		arg_count = args.args_count;
	for (i = 0; i < arg_count; i++)
		out_args[i] = args.args[i];

	of_node_put(args.np);

	return arg_count;
}

#endif /* _SPRD_SYSCON_COMPAT_H_ */
