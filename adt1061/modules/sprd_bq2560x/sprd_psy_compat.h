/* SPDX-License-Identifier: GPL-2.0 */
/*
 * GKI 6.12 port: pieces of the Unisoc 4.14 power_supply core extensions that
 * the SC27xx FGU / charger drivers rely on.
 *
 *  - struct sprd_battery_info: vendor layout of struct power_supply_battery_info
 *    (adds voltage/capacity/resistance-temp tables and per charger-type currents)
 *  - sprd_psy_get_battery_info(): the vendor DT parser ("monitored-battery")
 *  - charger-manager notifications: CONFIG_CHARGER_MANAGER is not part of GKI,
 *    the events are dropped
 */
#ifndef _SPRD_PSY_COMPAT_H_
#define _SPRD_PSY_COMPAT_H_

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/slab.h>

struct power_supply_vol_temp_table {
	int vol;	/* microVolts */
	int temp;	/* celsius */
};

struct power_supply_capacity_temp_table {
	int temp;	/* celsius */
	int cap;	/* capacity percentage */
};

struct power_supply_charge_current {
	int sdp_limit;
	int sdp_cur;
	int dcp_limit;
	int dcp_cur;
	int cdp_limit;
	int cdp_cur;
	int aca_limit;
	int aca_cur;
	int unknown_limit;
	int unknown_cur;
	int fchg_limit;
	int fchg_cur;
};

struct sprd_battery_info {
	int energy_full_design_uwh;
	int charge_full_design_uah;
	int voltage_min_design_uv;
	int precharge_current_ua;
	int charge_term_current_ua;
	int constant_charge_current_max_ua;
	int constant_charge_voltage_max_uv;
	int factory_internal_resistance_uohm;
	int ocv_temp[POWER_SUPPLY_OCV_TEMP_MAX];
	struct power_supply_battery_ocv_table *ocv_table[POWER_SUPPLY_OCV_TEMP_MAX];
	int ocv_table_size[POWER_SUPPLY_OCV_TEMP_MAX];
	struct power_supply_vol_temp_table *temp_table;
	struct power_supply_capacity_temp_table *cap_table;
	struct power_supply_resistance_temp_table *resistance_table;
	int temp_table_size;
	int cap_table_size;
	int resistance_table_size;
	struct power_supply_charge_current cur;
};

/* charger-manager (not in GKI): events are ignored */
enum cm_event_types {
	CM_EVENT_UNKNOWN = 0,
	CM_EVENT_BATT_FULL,
	CM_EVENT_BATT_IN,
	CM_EVENT_BATT_OUT,
	CM_EVENT_BATT_OVERHEAT,
	CM_EVENT_BATT_COLD,
	CM_EVENT_EXT_PWR_IN_OUT,
	CM_EVENT_CHG_START_STOP,
	CM_EVENT_WL_CHG_START_STOP,
	CM_EVENT_OTHERS,
};

static inline void cm_notify_event(struct power_supply *psy,
				   enum cm_event_types type, char *msg)
{
}

static inline void sprd_psy_put_battery_info(struct power_supply *psy,
					     struct sprd_battery_info *info)
{
	int i;

	for (i = 0; i < POWER_SUPPLY_OCV_TEMP_MAX; i++)
		if (info->ocv_table[i])
			devm_kfree(&psy->dev, info->ocv_table[i]);
	if (info->temp_table)
		devm_kfree(&psy->dev, info->temp_table);
	if (info->cap_table)
		devm_kfree(&psy->dev, info->cap_table);
	if (info->resistance_table)
		devm_kfree(&psy->dev, info->resistance_table);
}

static inline int sprd_psy_get_battery_info(struct power_supply *psy,
					    struct sprd_battery_info *info)
{
	struct device_node *battery_np;
	const char *value;
	const __be32 *list;
	int err, len, index, size;
	struct device_node *np = psy->dev.of_node;

	info->energy_full_design_uwh = -EINVAL;
	info->charge_full_design_uah = -EINVAL;
	info->voltage_min_design_uv = -EINVAL;
	info->precharge_current_ua = -EINVAL;
	info->charge_term_current_ua = -EINVAL;
	info->constant_charge_current_max_ua = -EINVAL;
	info->constant_charge_voltage_max_uv = -EINVAL;
	info->factory_internal_resistance_uohm = -EINVAL;
	info->temp_table = NULL;
	info->cap_table = NULL;
	info->resistance_table = NULL;
	info->temp_table_size = -EINVAL;
	info->cap_table_size = -EINVAL;
	info->resistance_table_size = -EINVAL;
	info->cur.sdp_cur = info->cur.sdp_limit = -EINVAL;
	info->cur.dcp_cur = info->cur.dcp_limit = -EINVAL;
	info->cur.cdp_cur = info->cur.cdp_limit = -EINVAL;
	info->cur.aca_cur = info->cur.aca_limit = -EINVAL;
	info->cur.unknown_cur = info->cur.unknown_limit = -EINVAL;
	info->cur.fchg_cur = info->cur.fchg_limit = -EINVAL;

	for (index = 0; index < POWER_SUPPLY_OCV_TEMP_MAX; index++) {
		info->ocv_table[index] = NULL;
		info->ocv_temp[index] = -EINVAL;
		info->ocv_table_size[index] = -EINVAL;
	}

	if (!np)
		return -ENXIO;

	battery_np = of_parse_phandle(np, "monitored-battery", 0);
	if (!battery_np)
		return -ENODEV;

	err = of_property_read_string(battery_np, "compatible", &value);
	if (err)
		return err;
	if (strcmp("simple-battery", value))
		return -ENODEV;

	of_property_read_u32(battery_np, "energy-full-design-microwatt-hours",
			     &info->energy_full_design_uwh);
	of_property_read_u32(battery_np, "charge-full-design-microamp-hours",
			     &info->charge_full_design_uah);
	of_property_read_u32(battery_np, "voltage-min-design-microvolt",
			     &info->voltage_min_design_uv);
	of_property_read_u32(battery_np, "precharge-current-microamp",
			     &info->precharge_current_ua);
	of_property_read_u32(battery_np, "charge-term-current-microamp",
			     &info->charge_term_current_ua);
	of_property_read_u32(battery_np, "constant_charge_current_max_microamp",
			     &info->constant_charge_current_max_ua);
	of_property_read_u32(battery_np, "constant_charge_voltage_max_microvolt",
			     &info->constant_charge_voltage_max_uv);
	of_property_read_u32(battery_np, "factory-internal-resistance-micro-ohms",
			     &info->factory_internal_resistance_uohm);
	of_property_read_u32_index(battery_np, "charge-sdp-current-microamp", 0, &info->cur.sdp_cur);
	of_property_read_u32_index(battery_np, "charge-sdp-current-microamp", 1, &info->cur.sdp_limit);
	of_property_read_u32_index(battery_np, "charge-dcp-current-microamp", 0, &info->cur.dcp_cur);
	of_property_read_u32_index(battery_np, "charge-dcp-current-microamp", 1, &info->cur.dcp_limit);
	of_property_read_u32_index(battery_np, "charge-cdp-current-microamp", 0, &info->cur.cdp_cur);
	of_property_read_u32_index(battery_np, "charge-cdp-current-microamp", 1, &info->cur.cdp_limit);
	of_property_read_u32_index(battery_np, "charge-aca-current-microamp", 0, &info->cur.aca_cur);
	of_property_read_u32_index(battery_np, "charge-aca-current-microamp", 1, &info->cur.aca_limit);
	of_property_read_u32_index(battery_np, "charge-unknown-current-microamp", 0, &info->cur.unknown_cur);
	of_property_read_u32_index(battery_np, "charge-unknown-current-microamp", 1, &info->cur.unknown_limit);
	of_property_read_u32_index(battery_np, "charge-fchg-current-microamp", 0, &info->cur.fchg_cur);
	of_property_read_u32_index(battery_np, "charge-fchg-current-microamp", 1, &info->cur.fchg_limit);

	len = of_property_count_u32_elems(battery_np, "ocv-capacity-celsius");
	if (len < 0 && len != -EINVAL)
		return len;
	if (len > POWER_SUPPLY_OCV_TEMP_MAX) {
		dev_err(&psy->dev, "Too many temperature values\n");
		return -EINVAL;
	}
	if (len > 0)
		of_property_read_u32_array(battery_np, "ocv-capacity-celsius",
					   info->ocv_temp, len);

	for (index = 0; index < len; index++) {
		struct power_supply_battery_ocv_table *table;
		char *propname;
		int i, tab_len;

		propname = kasprintf(GFP_KERNEL, "ocv-capacity-table-%d", index);
		list = of_get_property(battery_np, propname, &size);
		if (!list || !size) {
			dev_err(&psy->dev, "failed to get %s\n", propname);
			kfree(propname);
			sprd_psy_put_battery_info(psy, info);
			return -EINVAL;
		}
		kfree(propname);
		tab_len = size / (2 * sizeof(__be32));
		info->ocv_table_size[index] = tab_len;
		table = info->ocv_table[index] =
			devm_kcalloc(&psy->dev, tab_len, sizeof(*table), GFP_KERNEL);
		if (!table) {
			sprd_psy_put_battery_info(psy, info);
			return -ENOMEM;
		}
		for (i = 0; i < tab_len; i++) {
			table[i].ocv = be32_to_cpu(*list++);
			table[i].capacity = be32_to_cpu(*list++);
		}
	}

	list = of_get_property(battery_np, "voltage-temp-table", &size);
	if (!list || !size)
		return 0;
	info->temp_table_size = size / (2 * sizeof(__be32));
	info->temp_table = devm_kcalloc(&psy->dev, info->temp_table_size,
					sizeof(*info->temp_table), GFP_KERNEL);
	if (!info->temp_table) {
		sprd_psy_put_battery_info(psy, info);
		return -ENOMEM;
	}
	for (index = 0; index < info->temp_table_size; index++) {
		info->temp_table[index].vol = be32_to_cpu(*list++);
		info->temp_table[index].temp = be32_to_cpu(*list++);
	}

	list = of_get_property(battery_np, "capacity-temp-table", &size);
	if (!list || !size)
		return 0;
	info->cap_table_size = size / (2 * sizeof(__be32));
	info->cap_table = devm_kcalloc(&psy->dev, info->cap_table_size,
				       sizeof(*info->cap_table), GFP_KERNEL);
	if (!info->cap_table) {
		sprd_psy_put_battery_info(psy, info);
		return -ENOMEM;
	}
	for (index = 0; index < info->cap_table_size; index++) {
		info->cap_table[index].temp = be32_to_cpu(*list++);
		info->cap_table[index].cap = be32_to_cpu(*list++);
	}

	list = of_get_property(battery_np, "resistance-temp-table", &size);
	if (!list || !size)
		return 0;
	info->resistance_table_size = size / (2 * sizeof(__be32));
	info->resistance_table = devm_kcalloc(&psy->dev, info->resistance_table_size,
					      sizeof(*info->resistance_table), GFP_KERNEL);
	if (!info->resistance_table) {
		sprd_psy_put_battery_info(psy, info);
		return -ENOMEM;
	}
	for (index = 0; index < info->resistance_table_size; index++) {
		info->resistance_table[index].temp = be32_to_cpu(*list++);
		info->resistance_table[index].resistance = be32_to_cpu(*list++);
	}

	return 0;
}

static inline struct power_supply_battery_ocv_table *
sprd_psy_find_ocv2cap_table(struct sprd_battery_info *info, int temp, int *table_len)
{
	int best_temp_diff = INT_MAX, best_index = 0, temp_diff, i;

	if (!info->ocv_table[0])
		return NULL;
	for (i = 0; i < POWER_SUPPLY_OCV_TEMP_MAX; i++) {
		temp_diff = abs(info->ocv_temp[i] - temp);
		if (temp_diff < best_temp_diff) {
			best_temp_diff = temp_diff;
			best_index = i;
		}
	}
	*table_len = info->ocv_table_size[best_index];
	return info->ocv_table[best_index];
}

static inline int sprd_psy_ocv2cap_simple(struct power_supply_battery_ocv_table *table,
					  int table_len, int ocv)
{
	int i, cap, tmp;

	for (i = 0; i < table_len; i++)
		if (ocv > table[i].ocv)
			break;

	if (i > 0 && i < table_len) {
		tmp = (table[i - 1].capacity - table[i].capacity) *
			(ocv - table[i].ocv);
		tmp /= table[i - 1].ocv - table[i].ocv;
		cap = tmp + table[i].capacity;
	} else if (i == 0) {
		cap = table[0].capacity;
	} else {
		cap = table[table_len - 1].capacity;
	}

	return cap;
}

static inline int sprd_psy_batinfo_ocv2cap(struct sprd_battery_info *info,
					   int ocv, int temp)
{
	struct power_supply_battery_ocv_table *table;
	int table_len;

	table = sprd_psy_find_ocv2cap_table(info, temp, &table_len);
	if (!table)
		return -EINVAL;
	return sprd_psy_ocv2cap_simple(table, table_len, ocv);
}

#endif /* _SPRD_PSY_COMPAT_H_ */
