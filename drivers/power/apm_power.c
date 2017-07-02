/*
 * Copyright © 2007 Anton Vorontsov <cbou@mail.ru>
 * Copyright © 2007 Eugeny Boger <eugenyboger@dgap.mipt.ru>
 *
 * Author: Eugeny Boger <eugenyboger@dgap.mipt.ru>
 *
 * Use consistent with the GNU GPL is permitted,
 * provided that this copyright notice is
 * preserved in its entirety in all copies and derived works.
 */

#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/apm-emulation.h>

#define PSY_PROP(psy, prop, val) psy->get_property(psy, \
			 POWER_SUPPLY_PROP_##prop, val)

#define _MPSY_PROP(prop, val) main_battery->get_property(main_battery, \
							 prop, val)

#define MPSY_PROP(prop, val) _MPSY_PROP(POWER_SUPPLY_PROP_##prop, val)

static struct power_supply *main_battery;

static void find_main_battery(void)
{
	struct device *dev;
	struct power_supply *bat = NULL;
	struct power_supply *max_charge_bat = NULL;
	struct power_supply *max_energy_bat = NULL;
	union power_supply_propval full;
	int max_charge = 0;
	int max_energy = 0;

	main_battery = NULL;

	list_for_each_entry(dev, &power_supply_class->devices, node) {
		bat = dev_get_drvdata(dev);

		if (bat->use_for_apm) {
			/* nice, we explicitly asked to report this battery. */
			main_battery = bat;
			return;
		}

		if (!PSY_PROP(bat, CHARGE_FULL_DESIGN, &full) ||
				!PSY_PROP(bat, CHARGE_FULL, &full)) {
			if (full.intval > max_charge) {
				max_charge_bat = bat;
				max_charge = full.intval;
			}
		} else if (!PSY_PROP(bat, ENERGY_FULL_DESIGN, &full) ||
				!PSY_PROP(bat, ENERGY_FULL, &full)) {
			if (full.intval > max_energy) {
				max_energy_bat = bat;
				max_energy = full.intval;
			}
		}
	}

	if ((max_energy_bat && max_charge_bat) &&
			(max_energy_bat != max_charge_bat)) {
		/* try guess battery with more capacity */
		if (!PSY_PROP(max_charge_bat, VOLTAGE_MAX_DESIGN, &full)) {
			if (max_energy > max_charge * full.intval)
				main_battery = max_energy_bat;
			else
				main_battery = max_charge_bat;
		} else if (!PSY_PROP(max_energy_bat, VOLTAGE_MAX_DESIGN,
								  &full)) {
			if (max_charge > max_energy / full.intval)
				main_battery = max_charge_bat;
			else
				main_battery = max_energy_bat;
		} else {
			/* give up, choice any */
			main_battery = max_energy_bat;
		}
	} else if (max_charge_bat) {
		main_battery = max_charge_bat;
	} else if (max_energy_bat) {
		main_battery = max_energy_bat;
	} else {
		/* give up, try the last if any */
		main_battery = bat;
	}
}

static int calculate_time(int status, int using_charge)
{
	union power_supply_propval full;
	union power_supply_propval empty;
	union power_supply_propval cur;
	union power_supply_propval I;
	enum power_supply_property full_prop;
	enum power_supply_property full_design_prop;
	enum power_supply_property empty_prop;
	enum power_supply_property empty_design_prop;
	enum power_supply_property cur_avg_prop;
	enum power_supply_property cur_now_prop;

	if (MPSY_PROP(CURRENT_AVG, &I)) {
		/* if battery can't report average value, use momentary */
		if (MPSY_PROP(CURRENT_NOW, &I))
			return -1;
	}

	if (using_charge) {
		full_prop = POWER_SUPPLY_PROP_CHARGE_FULL;
		full_design_prop = POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN;
		empty_prop = POWER_SUPPLY_PROP_CHARGE_EMPTY;
		empty_design_prop = POWER_SUPPLY_PROP_CHARGE_EMPTY;
		cur_avg_prop = POWER_SUPPLY_PROP_CHARGE_AVG;
		cur_now_prop = POWER_SUPPLY_PROP_CHARGE_NOW;
	} else {
		full_prop = POWER_SUPPLY_PROP_ENERGY_FULL;
		full_design_prop = POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN;
		empty_prop = POWER_SUPPLY_PROP_ENERGY_EMPTY;
		empty_design_prop = POWER_SUPPLY_PROP_CHARGE_EMPTY;
		cur_avg_prop = POWER_SUPPLY_PROP_ENERGY_AVG;
		cur_now_prop = POWER_SUPPLY_PROP_ENERGY_NOW;
	}

	if (_MPSY_PROP(full_prop, &full)) {
		/* if battery can't report this property, use design value */
		if (_MPSY_PROP(full_design_prop, &full))
			return -1;
	}

	if (_MPSY_PROP(empty_prop, &empty)) {
		/* if battery can't report this property, use design value */
		if (_MPSY_PROP(empty_design_prop, &empty))
			empty.intval = 0;
	}

	if (_MPSY_PROP(cur_avg_prop, &cur)) {
		/* if battery can't report average value, use momentary */
		if (_MPSY_PROP(cur_now_prop, &cur))
			return -1;
	}

	if (status == POWER_SUPPLY_STATUS_CHARGING)
		return ((cur.intval - full.intval) * 60L) / I.intval;
	else
		return -((cur.intval - empty.intval) * 60L) / I.intval;
}

static int calculate_capacity(int using_charge)
{
	enum power_supply_property full_prop, empty_prop;
	enum power_supply_property full_design_prop, empty_design_prop;
	enum power_supply_property now_prop, avg_prop;
	union power_supply_propval empty, full, cur;
	int ret;

	if (using_charge) {
		full_prop = POWER_SUPPLY_PROP_CHARGE_FULL;
		empty_prop = POWER_SUPPLY_PROP_CHARGE_EMPTY;
		full_design_prop = POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN;
		empty_design_prop = POWER_SUPPLY_PROP_CHARGE_EMPTY_DESIGN;
		now_prop = POWER_SUPPLY_PROP_CHARGE_NOW;
		avg_prop = POWER_SUPPLY_PROP_CHARGE_AVG;
	} else {
		full_prop = POWER_SUPPLY_PROP_ENERGY_FULL;
		empty_prop = POWER_SUPPLY_PROP_ENERGY_EMPTY;
		full_design_prop = POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN;
		empty_design_prop = POWER_SUPPLY_PROP_ENERGY_EMPTY_DESIGN;
		now_prop = POWER_SUPPLY_PROP_ENERGY_NOW;
		avg_prop = POWER_SUPPLY_PROP_ENERGY_AVG;
	}

	if (_MPSY_PROP(full_prop, &full)) {
		/* if battery can't report this property, use design value */
		if (_MPSY_PROP(full_design_prop, &full))
			return -1;
	}

	if (_MPSY_PROP(avg_prop, &cur)) {
		/* if battery can't report average value, use momentary */
		if (_MPSY_PROP(now_prop, &cur))
			return -1;
	}

	if (_MPSY_PROP(empty_prop, &empty)) {
		/* if battery can't report this property, use design value */
		if (_MPSY_PROP(empty_design_prop, &empty))
			empty.intval = 0;
	}

	if (full.intval - empty.intval)
		ret =  ((cur.intval - empty.intval) * 100L) /
		       (full.intval - empty.intval);
	else
		return -1;

	if (ret > 100)
		return 100;
	else if (ret < 0)
		return 0;

	return ret;
}

static void apm_battery_apm_get_power_status(struct apm_power_info *info)
{
	union power_supply_propval status;
	union power_supply_propval capacity, time_to_full, time_to_empty;

	down(&power_supply_class->sem);
	find_main_battery();
	if (!main_battery) {
		up(&power_supply_class->sem);
		return;
	}

	/* status */

	if (MPSY_PROP(STATUS, &status))
		status.intval = POWER_SUPPLY_STATUS_UNKNOWN;

	/* ac line status */

	if ((status.intval == POWER_SUPPLY_STATUS_CHARGING) ||
	    (status.intval == POWER_SUPPLY_STATUS_NOT_CHARGING) ||
	    (status.intval == POWER_SUPPLY_STATUS_FULL))
		info->ac_line_status = APM_AC_ONLINE;
	else
		info->ac_line_status = APM_AC_OFFLINE;

	/* battery life (i.e. capacity, in percents) */

	if (MPSY_PROP(CAPACITY, &capacity) == 0) {
		info->battery_life = capacity.intval;
	} else {
		/* try calculate using energy */
		info->battery_life = calculate_capacity(0);
		/* if failed try calculate using charge instead */
		if (info->battery_life == -1)
			info->battery_life = calculate_capacity(1);
	}

	/* charging status */

	if (status.intval == POWER_SUPPLY_STATUS_CHARGING) {
		info->battery_status = APM_BATTERY_STATUS_CHARGING;
	} else {
		if (info->battery_life > 50)
			info->battery_status = APM_BATTERY_STATUS_HIGH;
		else if (info->battery_life > 5)
			info->battery_status = APM_BATTERY_STATUS_LOW;
		else
			info->battery_status = APM_BATTERY_STATUS_CRITICAL;
	}
	info->battery_flag = info->battery_status;

	/* time */

	info->units = APM_UNITS_MINS;

	if (status.intval == POWER_SUPPLY_STATUS_CHARGING) {
		if (!MPSY_PROP(TIME_TO_FULL_AVG, &time_to_full) ||
				!MPSY_PROP(TIME_TO_FULL_NOW, &time_to_full)) {
			info->time = time_to_full.intval / 60;
		} else {
			info->time = calculate_time(status.intval, 0);
			if (info->time == -1)
				info->time = calculate_time(status.intval, 1);
		}
	} else {
		if (!MPSY_PROP(TIME_TO_EMPTY_AVG, &time_to_empty) ||
			      !MPSY_PROP(TIME_TO_EMPTY_NOW, &time_to_empty)) {
			info->time = time_to_empty.intval / 60;
		} else {
			info->time = calculate_time(status.intval, 0);
			if (info->time == -1)
				info->time = calculate_time(status.intval, 1);
		}
	}

	up(&power_supply_class->sem);
}

static int __init apm_battery_init(void)
{
	printk(KERN_INFO "APM Battery Driver\n");

	apm_get_power_status = apm_battery_apm_get_power_status;
	return 0;
}

static void __exit apm_battery_exit(void)
{
	apm_get_power_status = NULL;
}

module_init(apm_battery_init);
module_exit(apm_battery_exit);

MODULE_AUTHOR("Eugeny Boger <eugenyboger@dgap.mipt.ru>");
MODULE_DESCRIPTION("APM emulation driver for battery monitoring class");
MODULE_LICENSE("GPL");
