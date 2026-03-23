// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * x120x.c — Power supply driver for Suptronics X120x UPS HAT series
 *
 * Supported hardware:
 *   X1200, X1201, X1202, X1203, X1205, X1206 (and compatible variants)
 *
 * The X120x connects to the Raspberry Pi via pogo pins (not the 40-pin
 * header) and exposes four signals:
 *
 *   GPIO2 / GPIO3  I²C bus (SDA/SCL) to MAX17043 fuel gauge
 *   GPIO6          AC-present: high = mains OK, low = on battery
 *   GPIO16         Charge control: low = charging enabled (default),
 *                                  high = charging disabled
 *
 * This driver registers three power_supply devices:
 *
 *   x120x-battery  MAX17043: capacity, voltage, status, capacity_level
 *   x120x-ac       GPIO6:    online (mains present)
 *   x120x-charger  GPIO16:   charge_type (FAST / LONGLIFE), writeable
 *
 * The charger device exposes POWER_SUPPLY_CHARGE_TYPE_LONGLIFE when
 * charging is disabled (GPIO16 high) and POWER_SUPPLY_CHARGE_TYPE_FAST
 * when charging is enabled (GPIO16 low).  Writing LONGLIFE disables
 * charging; writing FAST re-enables it.  This matches the convention
 * used by UPower's battery conservation mode (EnableChargeThreshold
 * D-Bus method) and integrates natively with the "preserve battery"
 * toggle in GNOME 48+ Settings and KDE Plasma Power Management.
 *
 * Register map (MAX17043/MAX17044)
 * ─────────────────────────────────
 *   0x00-0x01  VCELL    12-bit ADC, upper 12 bits, 1.25 mV/LSB
 *   0x02-0x03  SOC      16-bit fixed-point: [15:8] integer %, [7:0] /256
 *   0x04-0x05  MODE     quick-start, sleep
 *   0x06-0x07  VERSION  chip ID
 *   0x0C-0x0D  CONFIG   alert threshold, sleep, alert flag
 *   0xFE-0xFF  COMMAND  power-on reset
 *
 *   VCELL conversion: uV = (raw >> 4) * 1250
 *   SOC  conversion: pct = raw >> 8  (integer part only)
 *
 * Device tree instantiation (preferred)
 * ──────────────────────────────────────
 *   &i2c1 {
 *       x120x: ups@36 {
 *           compatible = "suptronics,x120x";
 *           reg = <0x36>;
 *           ac-present-gpios  = <&gpio 6  GPIO_ACTIVE_HIGH>;
 *           charge-ctrl-gpios = <&gpio 16 GPIO_ACTIVE_HIGH>;
 *       };
 *   };
 *
 * Module parameter instantiation (no DT overlay required)
 * ─────────────────────────────────────────────────────────
 *   modprobe x120x i2c_bus=1 gpio_ac=6 gpio_charge_ctrl=16
 *
 * Copyright (C) 2026 Edvard Fielding <mor-lock@users.noreply.github.com>
 *
 * DISCLAIMER: This software is provided "as is" without warranty of any
 * kind.  The author is not liable for any damages arising from its use.
 * This driver interacts with battery hardware; validate correct operation
 * on your specific hardware before relying on it.  USE AT YOUR OWN RISK.
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/* -------------------------------------------------------------------------
 * Module parameters
 * ---------------------------------------------------------------------- */

static int i2c_bus = 1;
module_param(i2c_bus, int, 0444);
MODULE_PARM_DESC(i2c_bus, "I2C bus number (default 1)");

static int i2c_addrs[4] = { 0x36, 0x55, 0x32, 0x62 };
static int i2c_addrs_count = 4;
module_param_array(i2c_addrs, int, &i2c_addrs_count, 0444);
MODULE_PARM_DESC(i2c_addrs,
	"Fuel gauge I2C addresses to probe in order (default 0x36,0x55,0x32,0x62)");

static int gpio_ac = 6;
module_param(gpio_ac, int, 0444);
MODULE_PARM_DESC(gpio_ac,
	"BCM GPIO for AC-present signal, active-high (default 6)");

static int gpio_charge_ctrl = 16;
module_param(gpio_charge_ctrl, int, 0444);
MODULE_PARM_DESC(gpio_charge_ctrl,
	"BCM GPIO for charge control: low=enabled high=disabled (default 16)");

/* -------------------------------------------------------------------------
 * MAX17043 register definitions
 * ---------------------------------------------------------------------- */

#define MAX17043_REG_VCELL		0x00
#define MAX17043_REG_SOC		0x02
#define MAX17043_REG_MODE		0x04
#define MAX17043_REG_VERSION		0x06
#define MAX17043_REG_CONFIG		0x0C
#define MAX17043_REG_COMMAND		0xFE

#define MAX17043_VERSION_MASK		0xFFF0
#define MAX17043_MODE_QUICKSTART	0x4000
#define MAX17043_CONFIG_ALRT		BIT(5)

/* VCELL: upper 12 bits valid, 1.25 mV/LSB, kernel convention is uV */
#define MAX17043_VCELL_TO_UV(raw)	(((raw) >> 4) * 1250)

/* SOC: 16-bit fixed-point; integer part in bits [15:8] */
#define MAX17043_SOC_INT(raw)		((int)((raw) >> 8))

/* Quick-start if initial SoC is outside this plausible range (%) */
#define MAX17043_SOC_MIN_PLAUSIBLE	1
#define MAX17043_SOC_MAX_PLAUSIBLE	100

/* -------------------------------------------------------------------------
 * Voltage thresholds for CAPACITY_LEVEL (uV, on-battery only)
 *
 * Derived from Li-ion cell behaviour on the X120x 21700 pack.  Cell
 * voltage is used in preference to SoC% because the MAX17043 SoC
 * estimate degrades at low charge and may oscillate when cells are
 * failing.  Voltage is the reliable ground truth at the low end.
 *
 * Thresholds apply only when ac_online == 0.  On grid, a sub-threshold
 * voltage indicates a dead or damaged pack and must not trigger a
 * spurious CRITICAL shutdown.
 * ---------------------------------------------------------------------- */

#define X120X_UV_CRITICAL	3200000		/* 3.20 V: initiate shutdown  */
#define X120X_UV_LOW		3400000		/* 3.40 V: begin wind-down    */
#define X120X_SOC_FULL_PCT	95		/* report FULL above this %   */

/* Mark battery absent after this many consecutive I2C failures */
#define X120X_MAX_ERRORS	5

/* Polling interval */
#define X120X_POLL_MS		500

/* -------------------------------------------------------------------------
 * Driver private state
 * ---------------------------------------------------------------------- */

/**
 * struct x120x_chip - per-device driver state
 * @client:		I2C client for the MAX17043 fuel gauge
 * @regmap:		register map for I2C access
 * @battery:		battery power_supply device
 * @ac:			AC adapter power_supply device
 * @charger:		charger power_supply device (GPIO16 charge control)
 * @gpio_ac:		descriptor for AC-present GPIO (GPIO6), may be NULL
 * @gpio_chrg:		descriptor for charge-control GPIO (GPIO16), may be NULL
 * @lock:		mutex protecting all cached fields
 * @voltage_uv:		last good VCELL reading in uV
 * @capacity_pct:	last good SOC reading in integer percent (0-100)
 * @ac_online:		1 if mains present, 0 if on battery
 * @charge_disabled:	true when GPIO16 is high (charging inhibited)
 * @present:		false when consecutive I2C errors exceed threshold
 * @i2c_errors:		consecutive I2C read failure counter
 * @work:		delayed work item driving the polling loop
 */
struct x120x_chip {
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct power_supply	*battery;
	struct power_supply	*ac;
	struct power_supply	*charger;
	struct gpio_desc	*gpio_ac;
	struct gpio_desc	*gpio_chrg;

	struct mutex		 lock;
	int			 voltage_uv;
	int			 capacity_pct;
	int			 ac_online;
	bool			 charge_disabled;
	bool			 present;
	int			 i2c_errors;

	struct delayed_work	 work;
};

/* -------------------------------------------------------------------------
 * regmap configuration
 *
 * MAX17043: 8-bit addresses, 16-bit big-endian values, no caching
 * (all registers reflect live hardware state).
 * ---------------------------------------------------------------------- */

static const struct regmap_config x120x_regmap_config = {
	.reg_bits		= 8,
	.val_bits		= 16,
	.val_format_endian	= REGMAP_ENDIAN_BIG,
	.max_register		= MAX17043_REG_COMMAND + 1,
	.cache_type		= REGCACHE_NONE,
};

/* -------------------------------------------------------------------------
 * Chip helpers
 * ---------------------------------------------------------------------- */

/**
 * x120x_quick_start() - restart SoC estimation from open-circuit voltage
 * @chip: driver state
 *
 * Forces the MAX17043 to restart its model-based SoC calculation from
 * the current open-circuit voltage.  Called at probe when the initial
 * reading is implausible (stuck at 0 or saturated at 255).
 *
 * Return: 0 on success, negative errno on error.
 */
static int x120x_quick_start(struct x120x_chip *chip)
{
	int ret;

	ret = regmap_write(chip->regmap, MAX17043_REG_MODE,
			   MAX17043_MODE_QUICKSTART);
	if (ret)
		dev_warn(&chip->client->dev,
			 "quick-start command failed: %d\n", ret);
	else
		dev_dbg(&chip->client->dev, "quick-start issued\n");
	return ret;
}

/**
 * x120x_clear_alert() - clear the ALRT flag in the CONFIG register
 * @chip: driver state
 *
 * The ALRT flag is set by the chip when SoC crosses the configured alert
 * threshold.  The X120x does not wire the ALRT pin to the Pi, but
 * clearing the flag on probe keeps the chip in a known state.
 *
 * Return: 0 on success, negative errno on error.
 */
static int x120x_clear_alert(struct x120x_chip *chip)
{
	unsigned int config;
	int ret;

	ret = regmap_read(chip->regmap, MAX17043_REG_CONFIG, &config);
	if (ret)
		return ret;
	if (!(config & MAX17043_CONFIG_ALRT))
		return 0;
	return regmap_write(chip->regmap, MAX17043_REG_CONFIG,
			    config & ~MAX17043_CONFIG_ALRT);
}

/* -------------------------------------------------------------------------
 * GPIO helpers
 *
 * Use the descriptor API (DT path) when a descriptor is available;
 * fall back to the legacy number-based API for the module-parameter
 * instantiation path.  gpiod_get/set_value_cansleep() may sleep and
 * must not be called under a spinlock.  We use a mutex for chip->lock
 * so this is safe from the workqueue context.
 * ---------------------------------------------------------------------- */

static int x120x_gpio_get(struct gpio_desc *desc, int legacy_num)
{
	if (desc)
		return gpiod_get_value_cansleep(desc);
	return gpio_get_value(legacy_num);
}

static void x120x_gpio_set(struct gpio_desc *desc, int legacy_num, int val)
{
	if (desc)
		gpiod_set_value_cansleep(desc, val);
	else
		gpio_set_value(legacy_num, val);
}

/* -------------------------------------------------------------------------
 * Polling work item
 * ---------------------------------------------------------------------- */

static void x120x_poll_work(struct work_struct *work)
{
	struct x120x_chip *chip =
		container_of(work, struct x120x_chip, work.work);
	unsigned int vcell_raw, soc_raw;
	int new_uv, new_pct, new_ac, ret;
	bool new_present, new_chrg_disabled;
	bool bat_changed, ac_changed, chrg_changed;

	/* ----------------------------------------------------------------
	 * Read fuel gauge over I2C.
	 * On failure increment the error counter; once it exceeds the
	 * threshold mark the battery absent so userspace is not left with
	 * stale readings.
	 * -------------------------------------------------------------- */
	ret = regmap_read(chip->regmap, MAX17043_REG_VCELL, &vcell_raw);
	if (ret) {
		dev_warn_ratelimited(&chip->client->dev,
				     "VCELL read failed: %d\n", ret);
		mutex_lock(&chip->lock);
		bat_changed = (++chip->i2c_errors >= X120X_MAX_ERRORS &&
			       chip->present);
		if (bat_changed)
			chip->present = false;
		ac_changed = false;
		chrg_changed = false;
		mutex_unlock(&chip->lock);
		goto notify;
	}

	ret = regmap_read(chip->regmap, MAX17043_REG_SOC, &soc_raw);
	if (ret) {
		dev_warn_ratelimited(&chip->client->dev,
				     "SOC read failed: %d\n", ret);
		/* VCELL succeeded so the chip is alive; update voltage only */
		mutex_lock(&chip->lock);
		chip->i2c_errors = 0;
		chip->present    = true;
		bat_changed      = (chip->voltage_uv !=
				    MAX17043_VCELL_TO_UV(vcell_raw));
		chip->voltage_uv = MAX17043_VCELL_TO_UV(vcell_raw);
		ac_changed   = false;
		chrg_changed = false;
		mutex_unlock(&chip->lock);
		goto notify;
	}

	new_present      = true;
	new_uv           = MAX17043_VCELL_TO_UV(vcell_raw);
	new_pct          = clamp(MAX17043_SOC_INT(soc_raw), 0, 100);
	new_ac           = x120x_gpio_get(chip->gpio_ac, gpio_ac);
	if (new_ac < 0)
		new_ac = 0;	/* unreadable GPIO: assume on battery (safe) */
	new_chrg_disabled = !!x120x_gpio_get(chip->gpio_chrg, gpio_charge_ctrl);

	mutex_lock(&chip->lock);
	chip->i2c_errors  = 0;
	bat_changed       = (chip->present      != new_present  ||
			     chip->voltage_uv   != new_uv       ||
			     chip->capacity_pct != new_pct);
	ac_changed        = (chip->ac_online    != new_ac);
	chrg_changed      = (chip->charge_disabled != new_chrg_disabled);

	chip->present         = new_present;
	chip->voltage_uv      = new_uv;
	chip->capacity_pct    = new_pct;
	chip->ac_online       = new_ac;
	chip->charge_disabled = new_chrg_disabled;
	mutex_unlock(&chip->lock);

notify:
	/*
	 * Emit uevents only when state actually changed to avoid storms.
	 * An AC change implies battery status (CHARGING/DISCHARGING) also
	 * changed, so force bat_changed in that case.
	 */
	if (chrg_changed)
		power_supply_changed(chip->charger);
	if (ac_changed) {
		power_supply_changed(chip->ac);
		bat_changed = true;
	}
	if (bat_changed)
		power_supply_changed(chip->battery);

	schedule_delayed_work(&chip->work, msecs_to_jiffies(X120X_POLL_MS));
}

/* -------------------------------------------------------------------------
 * power_supply callbacks — battery
 * ---------------------------------------------------------------------- */

static enum power_supply_property x120x_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_SCOPE,
};

static int x120x_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	int ac_online, capacity_pct, voltage_uv;
	bool present, charge_disabled;

	mutex_lock(&chip->lock);
	ac_online       = chip->ac_online;
	capacity_pct    = chip->capacity_pct;
	voltage_uv      = chip->voltage_uv;
	present         = chip->present;
	charge_disabled = chip->charge_disabled;
	mutex_unlock(&chip->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (!present)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (!ac_online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (charge_disabled)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (capacity_pct >= X120X_SOC_FULL_PCT)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = present ? 1 : 0;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = voltage_uv;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = capacity_pct;
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		if (!present) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		} else if (!ac_online && voltage_uv > 0 &&
			   voltage_uv <= X120X_UV_CRITICAL) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		} else if (!ac_online && voltage_uv > 0 &&
			   voltage_uv <= X120X_UV_LOW) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		} else if (ac_online && capacity_pct >= X120X_SOC_FULL_PCT) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		} else {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		}
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;

	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static void x120x_battery_external_power_changed(struct power_supply *psy)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);

	/* Re-poll immediately rather than waiting up to POLL_MS */
	mod_delayed_work(system_wq, &chip->work, 0);
}

/* -------------------------------------------------------------------------
 * power_supply callbacks — AC adapter
 * ---------------------------------------------------------------------- */

static enum power_supply_property x120x_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int x120x_ac_get_property(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;

	mutex_lock(&chip->lock);
	val->intval = chip->ac_online;
	mutex_unlock(&chip->lock);

	return 0;
}

/* -------------------------------------------------------------------------
 * power_supply callbacks — charger (GPIO16 charge control)
 *
 * GPIO16 polarity: low = charging enabled, high = charging disabled.
 *
 * charge_type mapping:
 *   FAST      GPIO16 low  — normal charging
 *   LONGLIFE  GPIO16 high — conservation mode (charging inhibited)
 *
 * This convention is compatible with UPower's EnableChargeThreshold
 * D-Bus method and the battery health preservation UI in GNOME 48+
 * and KDE Plasma.  UPower writes LONGLIFE to enable preservation and
 * FAST to disable it; userspace policy (e.g. a daemon watching SoC)
 * decides when to toggle.
 * ---------------------------------------------------------------------- */

static enum power_supply_property x120x_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
};

static int x120x_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	bool charge_disabled;
	int ac_online;

	mutex_lock(&chip->lock);
	charge_disabled = chip->charge_disabled;
	ac_online       = chip->ac_online;
	mutex_unlock(&chip->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		/* Charger is online whenever mains is present */
		val->intval = ac_online;
		break;

	case POWER_SUPPLY_PROP_STATUS:
		if (!ac_online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (charge_disabled)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;

	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = charge_disabled
			? POWER_SUPPLY_CHARGE_TYPE_LONGLIFE
			: POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int x120x_charger_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	bool disable;

	if (psp != POWER_SUPPLY_PROP_CHARGE_TYPE)
		return -EINVAL;

	switch (val->intval) {
	case POWER_SUPPLY_CHARGE_TYPE_FAST:
		disable = false;
		break;
	case POWER_SUPPLY_CHARGE_TYPE_LONGLIFE:
		disable = true;
		break;
	default:
		return -EINVAL;
	}

	x120x_gpio_set(chip->gpio_chrg, gpio_charge_ctrl, disable ? 1 : 0);

	mutex_lock(&chip->lock);
	chip->charge_disabled = disable;
	mutex_unlock(&chip->lock);

	dev_dbg(&chip->client->dev, "charging %s via GPIO16\n",
		disable ? "disabled (LONGLIFE)" : "enabled (FAST)");

	power_supply_changed(chip->battery);
	return 0;
}

static int x120x_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_TYPE ? 1 : 0;
}

/* -------------------------------------------------------------------------
 * power_supply descriptors
 * ---------------------------------------------------------------------- */

static const char * const x120x_ac_supplied_to[] = { "x120x-battery" };
static const char * const x120x_charger_supplied_to[] = { "x120x-battery" };

static const struct power_supply_desc x120x_battery_desc = {
	.name                   = "x120x-battery",
	.type                   = POWER_SUPPLY_TYPE_BATTERY,
	.properties             = x120x_battery_props,
	.num_properties         = ARRAY_SIZE(x120x_battery_props),
	.get_property           = x120x_battery_get_property,
	.external_power_changed = x120x_battery_external_power_changed,
};

static const struct power_supply_desc x120x_ac_desc = {
	.name           = "x120x-ac",
	.type           = POWER_SUPPLY_TYPE_MAINS,
	.properties     = x120x_ac_props,
	.num_properties = ARRAY_SIZE(x120x_ac_props),
	.get_property   = x120x_ac_get_property,
};

static const struct power_supply_desc x120x_charger_desc = {
	.name                   = "x120x-charger",
	.type                   = POWER_SUPPLY_TYPE_MAINS,
	.properties             = x120x_charger_props,
	.num_properties         = ARRAY_SIZE(x120x_charger_props),
	.get_property           = x120x_charger_get_property,
	.set_property           = x120x_charger_set_property,
	.property_is_writeable  = x120x_charger_property_is_writeable,
};

/* -------------------------------------------------------------------------
 * PM ops
 * ---------------------------------------------------------------------- */

static int x120x_suspend(struct device *dev)
{
	struct x120x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	cancel_delayed_work_sync(&chip->work);
	return 0;
}

static int x120x_resume(struct device *dev)
{
	struct x120x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	schedule_delayed_work(&chip->work, 0);
	return 0;
}

static SIMPLE_DEV_PM_OPS(x120x_pm_ops, x120x_suspend, x120x_resume);

/* -------------------------------------------------------------------------
 * Probe / remove
 * ---------------------------------------------------------------------- */

static int x120x_probe(struct i2c_client *client,
		       const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct x120x_chip *chip;
	struct power_supply_config bat_cfg = {};
	struct power_supply_config ac_cfg  = {};
	struct power_supply_config chr_cfg = {};
	unsigned int version, soc_raw;
	int soc_pct, ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->client = client;
	chip->present = true;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	/* ── regmap ─────────────────────────────────────────────────── */
	chip->regmap = devm_regmap_init_i2c(client, &x120x_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	/* ── Verify chip identity ───────────────────────────────────── */
	ret = regmap_read(chip->regmap, MAX17043_REG_VERSION, &version);
	if (ret) {
		dev_err(dev, "failed to read chip version: %d\n", ret);
		return ret;
	}
	dev_info(dev, "MAX1704x at 0x%02x version 0x%03x\n",
		 client->addr, version & MAX17043_VERSION_MASK);

	/* ── GPIO6 — AC present ─────────────────────────────────────── */
	chip->gpio_ac = devm_gpiod_get_optional(dev, "ac-present", GPIOD_IN);
	if (IS_ERR(chip->gpio_ac)) {
		ret = PTR_ERR(chip->gpio_ac);
		dev_err(dev, "failed to get ac-present GPIO: %d\n", ret);
		return ret;
	}
	if (!chip->gpio_ac) {
		ret = devm_gpio_request_one(dev, gpio_ac,
					    GPIOF_IN, "x120x-ac-present");
		if (ret)
			dev_warn(dev,
				 "GPIO %d (AC-present) unavailable: %d — "
				 "ac_online will always be 0\n",
				 gpio_ac, ret);
	}

	/* ── GPIO16 — charge control ────────────────────────────────── */
	/*
	 * Request as output-low so the hardware default (charging enabled)
	 * is preserved across a driver reload.
	 */
	chip->gpio_chrg = devm_gpiod_get_optional(dev, "charge-ctrl",
						   GPIOD_OUT_LOW);
	if (IS_ERR(chip->gpio_chrg)) {
		ret = PTR_ERR(chip->gpio_chrg);
		dev_err(dev, "failed to get charge-ctrl GPIO: %d\n", ret);
		return ret;
	}
	if (!chip->gpio_chrg) {
		ret = devm_gpio_request_one(dev, gpio_charge_ctrl,
					    GPIOF_OUT_INIT_LOW,
					    "x120x-charge-ctrl");
		if (ret)
			dev_warn(dev,
				 "GPIO %d (charge-ctrl) unavailable: %d — "
				 "charge_type will be read-only\n",
				 gpio_charge_ctrl, ret);
	}

	/* ── Initial chip setup ─────────────────────────────────────── */
	ret = x120x_clear_alert(chip);
	if (ret)
		dev_warn(dev, "failed to clear ALRT flag: %d\n", ret);

	ret = regmap_read(chip->regmap, MAX17043_REG_SOC, &soc_raw);
	if (!ret) {
		soc_pct = MAX17043_SOC_INT(soc_raw);
		if (soc_pct < MAX17043_SOC_MIN_PLAUSIBLE ||
		    soc_pct > MAX17043_SOC_MAX_PLAUSIBLE) {
			dev_info(dev,
				 "initial SoC %d%% is implausible, "
				 "issuing quick-start\n", soc_pct);
			x120x_quick_start(chip);
			msleep(150);
		}
	}

	/* ── Register power_supply devices ─────────────────────────── */

	/*
	 * supplied_to wires the notification chain so that when the AC
	 * adapter or charger calls power_supply_changed(), the battery's
	 * external_power_changed callback is invoked and it re-polls
	 * immediately rather than waiting up to POLL_MS.
	 */
	ac_cfg.drv_data        = chip;
	ac_cfg.supplied_to     = (char **)x120x_ac_supplied_to;
	ac_cfg.num_supplicants = ARRAY_SIZE(x120x_ac_supplied_to);

	chip->ac = devm_power_supply_register(dev, &x120x_ac_desc, &ac_cfg);
	if (IS_ERR(chip->ac)) {
		ret = PTR_ERR(chip->ac);
		dev_err(dev, "failed to register AC supply: %d\n", ret);
		return ret;
	}

	chr_cfg.drv_data        = chip;
	chr_cfg.supplied_to     = (char **)x120x_charger_supplied_to;
	chr_cfg.num_supplicants = ARRAY_SIZE(x120x_charger_supplied_to);

	chip->charger = devm_power_supply_register(dev, &x120x_charger_desc,
						   &chr_cfg);
	if (IS_ERR(chip->charger)) {
		ret = PTR_ERR(chip->charger);
		dev_err(dev, "failed to register charger supply: %d\n", ret);
		return ret;
	}

	bat_cfg.drv_data = chip;

	chip->battery = devm_power_supply_register(dev, &x120x_battery_desc,
						   &bat_cfg);
	if (IS_ERR(chip->battery)) {
		ret = PTR_ERR(chip->battery);
		dev_err(dev, "failed to register battery supply: %d\n", ret);
		return ret;
	}

	/* ── Start polling ──────────────────────────────────────────── */
	INIT_DELAYED_WORK(&chip->work, x120x_poll_work);
	schedule_delayed_work(&chip->work, 0);

	dev_info(dev, "x120x UPS ready (battery=%s ac=%s charger=%s)\n",
		 x120x_battery_desc.name,
		 x120x_ac_desc.name,
		 x120x_charger_desc.name);

	return 0;
}

static void x120x_remove(struct i2c_client *client)
{
	struct x120x_chip *chip = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chip->work);
}

/* -------------------------------------------------------------------------
 * I2C and OF tables
 * ---------------------------------------------------------------------- */

static const struct i2c_device_id x120x_id[] = {
	{ "x120x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, x120x_id);

static const struct of_device_id x120x_of_match[] = {
	{ .compatible = "suptronics,x120x" },
	{ }
};
MODULE_DEVICE_TABLE(of, x120x_of_match);

static struct i2c_driver x120x_driver = {
	.driver = {
		.name           = "x120x",
		.of_match_table = x120x_of_match,
		.pm             = &x120x_pm_ops,
	},
	.probe     = x120x_probe,
	.remove    = x120x_remove,
	.id_table  = x120x_id,
};

/* -------------------------------------------------------------------------
 * Module init / exit
 *
 * When no DT overlay is present the driver instantiates its own i2c_client
 * by probing the addresses in i2c_addrs[].  This allows `modprobe x120x`
 * to work on stock Raspberry Pi OS without any DT or config.txt changes.
 * ---------------------------------------------------------------------- */

static struct i2c_client *x120x_i2c_client;

static int __init x120x_init(void)
{
	struct i2c_adapter *adapter;
	struct i2c_board_info info = {};
	struct i2c_client *client;
	int i, ret;

	ret = i2c_add_driver(&x120x_driver);
	if (ret)
		return ret;

	adapter = i2c_get_adapter(i2c_bus);
	if (!adapter) {
		pr_warn("x120x: i2c-%d not available\n", i2c_bus);
		return 0;
	}

	strscpy(info.type, "x120x", sizeof(info.type));

	for (i = 0; i < i2c_addrs_count; i++) {
		info.addr = (unsigned short)i2c_addrs[i];
		client = i2c_new_client_device(adapter, &info);
		if (!IS_ERR(client)) {
			x120x_i2c_client = client;
			break;
		}
	}

	i2c_put_adapter(adapter);

	if (!x120x_i2c_client)
		pr_info("x120x: no fuel gauge found on i2c-%d "
			"(tried addrs: %d candidates)\n",
			i2c_bus, i2c_addrs_count);

	return 0;
}

static void __exit x120x_exit(void)
{
	if (x120x_i2c_client) {
		i2c_unregister_device(x120x_i2c_client);
		x120x_i2c_client = NULL;
	}
	i2c_del_driver(&x120x_driver);
}

module_init(x120x_init);
module_exit(x120x_exit);

MODULE_AUTHOR("Edvard Fielding <mor-lock@users.noreply.github.com>");
MODULE_DESCRIPTION("Suptronics X120x UPS HAT power supply driver");
MODULE_LICENSE("GPL v2");

/*
 * DISCLAIMER
 *
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
 * NON-INFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY — INCLUDING BUT
 * NOT LIMITED TO LOSS OF DATA, HARDWARE DAMAGE, FINANCIAL LOSS, OR
 * CONSEQUENTIAL DAMAGES OF ANY KIND — WHETHER IN AN ACTION OF CONTRACT,
 * TORT, OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION WITH THIS
 * SOFTWARE OR THE USE OR MISUSE THEREOF.
 *
 * This driver interacts directly with battery hardware.  Incorrect
 * operation, misconfiguration, or use on unsupported hardware may result
 * in improper charging behaviour, failure to shut down before battery
 * exhaustion, or hardware damage.  You are solely responsible for
 * validating correct operation on your specific hardware before relying
 * on this driver for any purpose.
 *
 * USE AT YOUR OWN RISK.
 */
