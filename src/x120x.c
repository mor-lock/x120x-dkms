// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * x120x.c - Power supply driver for SupTronics X120x UPS HAT series
 *
 * Supported hardware (all share identical GPIO and I2C interface):
 *
 *   X1200, X1201, X1202, X1203, X1205, X1206  (Raspberry Pi 5, bottom-mount
 *                                               via pogo pins)
 *   X1207  (Raspberry Pi 5, PoE-powered, pogo pins)
 *   X1208  (Raspberry Pi 5, UPS + NVMe combo, pogo pins)
 *   X1209  (Raspberry Pi 5/4B/3B+/3B, 40-pin GPIO header)
 *
 * The boards expose four signals to the Raspberry Pi:
 *
 *   GPIO2 / GPIO3  I2C SDA/SCL to MAX17043 fuel gauge (address 0x36)
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
 * Register map (MAX17043 as used on X120x boards)
 * ------------------------------------------------
 * NOTE: The register layout used on SupTronics X120x boards differs
 * from the MAX17043 datasheet.  On these boards, as confirmed by
 * SupTronics' own software (github.com/suptronics/x120x), the
 * registers are mapped as follows:
 *
 *   0x02-0x03  VCELL   12-bit ADC, upper 12 bits, 1.25 mV/LSB
 *   0x04-0x05  SOC     16-bit fixed-point: [15:8] integer %, [7:0] /256
 *   0x06-0x07  VERSION chip ID
 *   0x0C-0x0D  CONFIG  alert threshold, sleep, alert flag
 *   0xFE-0xFF  COMMAND power-on reset / quick-start
 *
 * The datasheet defines VCELL at 0x00 and SOC at 0x02, but this driver
 * uses 0x02 and 0x04 to match observed hardware behaviour on all known
 * X120x board revisions.
 *
 *   VCELL conversion: uV = (raw >> 4) * 1250
 *   SOC  conversion: pct = raw >> 8  (integer part only)
 *
 * Device tree instantiation (preferred)
 * --------------------------------------
 *   &i2c1 {
 *       x120x: ups@36 {
 *           compatible = "suptronics,x120x";
 *           reg = <0x36>;
 *           ac-present-gpios  = <&gpio 6  GPIO_ACTIVE_HIGH>;
 *           charge-ctrl-gpios = <&gpio 16 GPIO_ACTIVE_HIGH>;
 *       };
 *   };
 *
 * Module parameter instantiation (no DT overlay required for I2C)
 * --------------------------------------------------------
 *   modprobe x120x i2c_bus=1
 *
 *   NOTE: GPIO6 and GPIO16 require the device tree overlay on kernel 6.12+.
 *   The legacy integer GPIO API was removed in kernel 6.12.  Without the
 *   overlay the driver loads and reads I2C correctly but ac_online will
 *   always be 0 and charge_type will be read-only.
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

/*
 * Battery pack energy parameters.
 *
 * battery_mah  — total pack capacity in mAh (default 1000).
 *                Set this to your actual pack capacity so that UPower
 *                and desktop environments can display meaningful energy
 *                values and time-to-empty / time-to-full estimates.
 *                Example: 4× 5000 mAh cells → battery_mah=20000
 *
 * voltage_empty_mv — cell voltage at shutdown threshold in mV (default 3200).
 *                    Used as the CAPACITY_LEVEL=Critical trigger voltage so that
 *                    systemd-logind can initiate a clean shutdown.  Also sets the
 *                    energy_empty floor for energy accounting.  Raise this value
 *                    (e.g. 4100) to test the shutdown path without draining the
 *                    battery to a dangerous level.
 *
 * All three are written to /etc/modprobe.d/x120x.conf by the installer.
 */
static int battery_mah = 1000;
module_param(battery_mah, int, 0444);
MODULE_PARM_DESC(battery_mah,
	"Total battery pack capacity in mAh (default 1000)");

static int voltage_empty_mv = 3200;
module_param(voltage_empty_mv, int, 0444);
MODULE_PARM_DESC(voltage_empty_mv,
	"Cell voltage at Critical/shutdown threshold in mV (default 3200). "
	"Raise temporarily (e.g. 4100) to test the logind shutdown path.");

/*
 * Charge threshold parameters for Long life / conservation mode.
 * Only active when charge_type is set to Long life.  In Fast mode
 * GPIO16 is held low (charging always enabled) and these are ignored.
 */
static int conservation_start = 75;
module_param(conservation_start, int, 0644);
MODULE_PARM_DESC(conservation_start,
	"SoC %% at which charging resumes in Long Life mode (default 75)");

static int conservation_end = 80;
module_param(conservation_end, int, 0644);
MODULE_PARM_DESC(conservation_end,
	"SoC %% at which charging stops in Long Life mode (default 80)");

/*
 * conservation_mode_default — persists charge mode across reboots.
 * 0 = Fast (default), 1 = Long Life.
 * Automatically updated when charge_type is written via sysfs, and
 * persisted to /etc/modprobe.d/x120x.conf by a udev rule installed
 * by the installer.
 */
static int conservation_mode_default = 0;
module_param(conservation_mode_default, int, 0644);
MODULE_PARM_DESC(conservation_mode_default,
	"Start in Long Life mode (1) or Fast mode (0, default). "
	"Updated on every charge_type sysfs write; persisted by udev rule.");

/* -------------------------------------------------------------------------
 * MAX17043 register definitions (X120x board layout)
 *
 * These offsets match SupTronics' published software for all X120x boards
 * and differ from the MAX17043 datasheet by one register pair.
 * ---------------------------------------------------------------------- */

#define MAX17043_REG_VCELL		0x02
#define MAX17043_REG_SOC		0x04
#define MAX17043_REG_VERSION		0x06
#define MAX17043_REG_CONFIG		0x0C
#define MAX17043_REG_COMMAND		0xFE

#define MAX17043_VERSION_MASK		0xFFF0
#define MAX17043_MODE_QUICKSTART	0x4000
#define MAX17043_CONFIG_ALRT		BIT(5)

/*
 * VCELL: upper 12 bits valid, 1.25 mV/LSB, kernel convention is uV.
 *   uV = (raw >> 4) * 1250
 */
#define MAX17043_VCELL_TO_UV(raw)	(((raw) >> 4) * 1250)

/* SOC: 16-bit fixed-point; integer part in [15:8], fraction in [7:0] (/256).
 * _INT gives integer percent for the capacity property.
 * _256 gives the raw 16-bit value (0..25600 for 0..100%) for full precision
 * energy and rate computations.
 */
#define MAX17043_SOC_INT(raw)		((int)((raw) >> 8))
#define MAX17043_SOC_256(raw)		((int)(raw))

/* Trigger a quick-start if initial SoC is outside this range (%) */
#define MAX17043_SOC_MIN_PLAUSIBLE	1
#define MAX17043_SOC_MAX_PLAUSIBLE	100

/* -------------------------------------------------------------------------
 * Voltage thresholds for CAPACITY_LEVEL (uV, on-battery only)
 *
 * Derived from Li-ion cell behaviour on the X120x series.  Voltage is
 * used in preference to SoC% because the MAX17043 SoC estimate degrades
 * at low charge and may oscillate when cells are failing.  Voltage is
 * the reliable ground truth at the critical end of the range.
 *
 * Thresholds apply only when ac_online == 0.  On grid, a sub-threshold
 * voltage indicates a dead or damaged pack — not normal discharge — and
 * must not trigger a spurious CRITICAL shutdown.
 * ---------------------------------------------------------------------- */

/*
 * X120X_UV_CRITICAL is derived from the voltage_empty_mv module parameter
 * so users can test the shutdown path without draining to a dangerous level.
 * X120X_UV_LOW is fixed at 200 mV above Critical.
 */
#define X120X_UV_CRITICAL	((s64)voltage_empty_mv * 1000)
#define X120X_UV_LOW		((s64)(voltage_empty_mv + 200) * 1000)
#define X120X_SOC_FULL_PCT	95		/* report FULL above this %   */

/* Mark battery absent after this many consecutive I2C failures */
#define X120X_MAX_ERRORS	5

/* Hardware polling interval */
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
 * @lock:		mutex protecting all cached fields below
 * @voltage_uv:		last good VCELL reading in uV
 * @capacity_pct:	last good SOC reading in integer percent (0-100)
 * @ac_online:		1 if mains present, 0 if on battery
 * @conservation_mode:	true when Long life mode is active (charge_type=LONGLIFE)
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
	int			 capacity_pct;	/* integer percent 0-100        */
	int			 capacity_256;	/* full precision: raw SOC word */
	int			 ac_online;
	bool			 conservation_mode;	/* true = Long life, threshold hysteresis active */
	bool			 present;
	int			 i2c_errors;

	/* Energy tracking for UPower / desktop environment integration */
	s64			 energy_now_uwh;	 /* µWh = energy_full × soc%/100 */
	s64			 energy_full_uwh;	 /* µWh = battery_mah × 3700 mV  */
	s64			 energy_empty_uwh;	 /* µWh = 0 (UPower floor)        */
	/*
	 * Rate estimation: event-driven, one sample per SOC register change.
	 * Each time capacity_256 changes we compute the rate from the delta
	 * since the previous change.  Changes less than 10 s apart are
	 * discarded as noise.  Sign: negative = discharging, positive = charging.
	 */
	s64			 rate_prev_energy_uwh;	/* energy at last SOC change    */
	s64			 rate_prev_time_us;	/* ktime_us at last SOC change  */
	int			 energy_rate_uw;		/* µW, updated on each event    */
	s64			 rate_last_change_us;		/* ktime_us of last SOC change  */

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

	ret = regmap_write(chip->regmap, MAX17043_REG_COMMAND,
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
 * clearing the flag on probe keeps the chip in a known clean state.
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
 * Kernel 6.12 removed the legacy integer-based GPIO API entirely.
 * This driver requires the descriptor API; GPIOs must be provided via
 * device tree (use the supplied overlay) or via gpiod lookups.
 * gpiod_get/set_value_cansleep() may sleep and must not be called
 * under a spinlock.  We use a mutex for chip->lock so this is safe.
 * ---------------------------------------------------------------------- */

static int x120x_gpio_get(struct gpio_desc *desc)
{
	if (!desc)
		return 0;	/* GPIO not available: safe default */
	return gpiod_get_value_cansleep(desc);
}

static void x120x_gpio_set(struct gpio_desc *desc, int val)
{
	if (desc)
		gpiod_set_value_cansleep(desc, val);
}

/* -------------------------------------------------------------------------
 * Polling work item
 * ---------------------------------------------------------------------- */

static void x120x_poll_work(struct work_struct *work)
{
	struct x120x_chip *chip =
		container_of(work, struct x120x_chip, work.work);
	unsigned int vcell_raw, soc_raw;
	int new_uv, new_pct, new_256, new_ac, ret;
	bool new_present;
	bool bat_changed, ac_changed, chrg_changed;

	/* ----------------------------------------------------------------
	 * Read fuel gauge.  On failure, increment the error counter and
	 * mark battery absent once the threshold is exceeded so userspace
	 * is not left reading stale values indefinitely.
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
		ac_changed   = false;
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

	new_present       = true;
	new_uv            = MAX17043_VCELL_TO_UV(vcell_raw);
	new_pct           = clamp(MAX17043_SOC_INT(soc_raw), 0, 100);
	new_256           = MAX17043_SOC_256(soc_raw); /* raw, unclamped for rate */
	new_ac            = x120x_gpio_get(chip->gpio_ac);
	if (new_ac < 0)
		new_ac = 0;	/* unreadable: assume on battery (safe) */
	mutex_lock(&chip->lock);
	chip->i2c_errors  = 0;
	bat_changed       = (chip->present      != new_present  ||
			     chip->voltage_uv   != new_uv       ||
			     chip->capacity_pct != new_pct      ||
			     chip->capacity_256 != new_256);
	ac_changed        = (chip->ac_online    != new_ac);
	/* conservation_mode is set only by set_property, never read back from GPIO */

	chip->present         = new_present;
	chip->voltage_uv      = new_uv;
	chip->capacity_pct    = new_pct;
	chip->capacity_256    = new_256;
	chip->ac_online       = new_ac;

	/*
	 * Grid state change: any rate computed across the transition is
	 * meaningless (charging → discharging or vice versa).  Zero the
	 * rate immediately and snapshot the current SOC and timestamp as
	 * the baseline for the next measurement on the new side.
	 * The 10 s minimum guard will then apply to the first new sample.
	 */
	{
		/*
		 * energy_full  = battery_mah × 3700 mV (nominal)
		 * energy_empty = 0  (floor — lets UPower use energy_now /
		 *                    energy_full directly for percentage)
		 * energy_now   = energy_full × soc% / 100
		 */
		s64 e_full  = (s64)battery_mah * 3700;
		/* Use full 16-bit SOC precision (0..25600 = 0..100%) */
		s64 e_now   = div_s64(e_full * new_256, 25600);
		ktime_t now = ktime_get();
		s64 now_us  = ktime_to_us(now);

		if (ac_changed) {
			chip->energy_rate_uw       = 0;
			chip->rate_prev_energy_uwh = e_now;
			chip->rate_prev_time_us    = now_us;
			chip->rate_last_change_us  = now_us;
		}

		/*
		 * Event-driven rate estimation.
		 *
		 * We only compute a new rate when the SOC register changes.
		 * Changes less than 10 s apart are discarded — they indicate
		 * noise or a rapid double-update from the chip rather than a
		 * genuine new measurement.
		 *
		 * rate (µW) = ΔE (µWh) / Δt (µs) × 3600 × 1e6
		 * Sign: negative = discharging, positive = charging.
		 */
		if (new_256 != chip->capacity_256) {
			s64 dt = now_us - chip->rate_prev_time_us;

			if (chip->rate_prev_time_us != 0 &&
			    dt >= 10LL * USEC_PER_SEC) {
				s64 de = e_now - chip->rate_prev_energy_uwh;
				/*
				 * Clamp dt to 90 s maximum.  If the zero-rate
				 * timeout fired between two SOC changes (e.g. at
				 * t=90s with the next change at t=120s), using
				 * the full 120s window would dilute the rate with
				 * a period of zero current.  Clamping to 90s gives
				 * a rate representative of the most recent active
				 * charging window rather than the full silent gap.
				 */
				if (dt > 90LL * USEC_PER_SEC)
					dt = 90LL * USEC_PER_SEC;
				chip->energy_rate_uw = (int)div_s64(
					de * 3600LL * USEC_PER_SEC, dt);
			}

			/* Always update prev on any SOC change, even discarded */
			chip->rate_prev_energy_uwh  = e_now;
			chip->rate_prev_time_us     = now_us;
			chip->rate_last_change_us   = now_us;
		} else if (chip->rate_last_change_us != 0 &&
			   now_us - chip->rate_last_change_us >
			   90LL * USEC_PER_SEC) {
			/*
			 * SOC unchanged for >90 s: charge current is zero
			 * (battery full, or negligible load).  Zero the rate
			 * but do NOT update rate_prev_time_us — if SOC resumes
			 * changing, dt will be computed from the last real
			 * measurement and then clamped to 90 s (see above).
			 */
			chip->energy_rate_uw      = 0;
			chip->rate_last_change_us = 0; /* disarm until next change */
		}

		chip->energy_full_uwh  = e_full;
		chip->energy_empty_uwh = 0;
		chip->energy_now_uwh   = e_now;
	}

	mutex_unlock(&chip->lock);

notify:
	/*
	 * Emit uevents only when state actually changed to avoid storms.
	 * An AC change also implies battery STATUS changed, so force
	 * bat_changed in that case.
	 */
	/*
	 * Long life / conservation mode hysteresis.
	 *
	 * When conservation_mode is active, control GPIO16 based on SoC%:
	 *   capacity >= conservation_end   → stop charging  (GPIO16 high)
	 *   capacity <= conservation_start → resume charging (GPIO16 low)
	 *
	 * In Fast mode GPIO16 is always low (set immediately in set_property).
	 * We re-read the GPIO after any change so chrg_changed fires and
	 * UPower / sysfs sees the updated charge_type / status.
	 */
	if (chip->conservation_mode && chip->gpio_chrg) {
		int gpio_val = x120x_gpio_get(chip->gpio_chrg);
		int new_gpio_val = gpio_val;

		if (chip->capacity_pct >= conservation_end)
			new_gpio_val = 1; /* stop charging */
		else if (chip->capacity_pct <= conservation_start)
			new_gpio_val = 0; /* resume charging */

		if (new_gpio_val != gpio_val) {
			x120x_gpio_set(chip->gpio_chrg, new_gpio_val);
			dev_dbg(&chip->client->dev,
				"conservation: %s charging at %d%%\n",
				new_gpio_val ? "stopped" : "resumed",
				chip->capacity_pct);
			chrg_changed = true;
			bat_changed  = true;
		}
	}

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
 * power_supply callbacks - battery
 * ---------------------------------------------------------------------- */

static enum power_supply_property x120x_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_SCOPE,
};

static int x120x_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	int ac_online, capacity_pct, voltage_uv, energy_rate_uw;
	s64 energy_now_uwh, energy_full_uwh;
	bool present, conservation_mode;

	mutex_lock(&chip->lock);
	ac_online        = chip->ac_online;
	capacity_pct     = chip->capacity_pct;
	voltage_uv       = chip->voltage_uv;
	present          = chip->present;
	conservation_mode = chip->conservation_mode;
	energy_now_uwh  = chip->energy_now_uwh;
	energy_full_uwh = chip->energy_full_uwh;
	energy_rate_uw  = chip->energy_rate_uw;
	mutex_unlock(&chip->lock);

	/*
	 * In conservation mode, GPIO16 is managed by the hysteresis loop.
	 * When GPIO16 is low (charging resumed below start threshold) the
	 * battery is actively charging even though conservation_mode is true.
	 * Read the actual GPIO state to report the correct status.
	 */
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS: {
		bool chrg_inhibited = conservation_mode && chip->gpio_chrg &&
				      x120x_gpio_get(chip->gpio_chrg);
		if (!present)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		else if (!ac_online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (chrg_inhibited)
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (capacity_pct >= X120X_SOC_FULL_PCT)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	}

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
		/*
		 * Use cell voltage rather than SoC% for level classification.
		 * The MAX17043 SoC estimate degrades at low charge — it may
		 * report 0% while voltage is still safe, or oscillate when
		 * cells are failing.  Voltage is the reliable ground truth.
		 *
		 * Thresholds apply only on battery: on grid, a low voltage
		 * indicates a dead pack, not normal discharge, and must not
		 * trigger a spurious CRITICAL shutdown.
		 */
		if (!present) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		} else if (!ac_online && voltage_uv > 0 &&
			   voltage_uv <= X120X_UV_CRITICAL) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		} else if (!ac_online && voltage_uv > 0 &&
			   voltage_uv <= X120X_UV_LOW) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		} else if (ac_online &&
			   capacity_pct >= X120X_SOC_FULL_PCT) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		} else {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		}
		break;

	case POWER_SUPPLY_PROP_ENERGY_NOW:
		/*
		 * energy_now in µWh.  The power_supply ABI uses µWh as the
		 * unit for energy properties (confusingly named _NOW/_FULL).
		 * Clamp to [energy_empty, energy_full] to avoid impossible
		 * values from rounding.
		 */
		val->intval = (int)clamp(energy_now_uwh, (s64)0, energy_full_uwh);
		break;

	case POWER_SUPPLY_PROP_ENERGY_FULL:
		val->intval = (int)energy_full_uwh;
		break;

	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = (int)energy_full_uwh;
		break;

	case POWER_SUPPLY_PROP_ENERGY_EMPTY:
		val->intval = 0;
		break;

	case POWER_SUPPLY_PROP_POWER_NOW:
		/*
		 * Instantaneous power in µW.  Positive = charging,
		 * negative = discharging.  Derived from the smoothed
		 * energy_rate computed in the polling loop.
		 */
		val->intval = energy_rate_uw;
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
 * power_supply callbacks - AC adapter
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
 * power_supply callbacks - charger (GPIO16 charge control)
 *
 * GPIO16 polarity: low = charging enabled, high = charging disabled.
 *
 * charge_type mapping:
 *   FAST      GPIO16 low  - normal charging enabled
 *   LONGLIFE  GPIO16 high - conservation mode (charging inhibited)
 *
 * This convention is compatible with UPower's EnableChargeThreshold
 * D-Bus method and the battery preservation UI in GNOME 48+ and KDE
 * Plasma.  UPower writes LONGLIFE to enable conservation mode and
 * FAST to disable it.
 * ---------------------------------------------------------------------- */

static enum power_supply_property x120x_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static int x120x_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	bool conservation_mode;
	int ac_online;

	mutex_lock(&chip->lock);
	conservation_mode = chip->conservation_mode;
	ac_online         = chip->ac_online;
	mutex_unlock(&chip->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = ac_online;
		break;

	case POWER_SUPPLY_PROP_STATUS:
		if (!ac_online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (conservation_mode && chip->gpio_chrg &&
			 x120x_gpio_get(chip->gpio_chrg))
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;

	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = conservation_mode
			? POWER_SUPPLY_CHARGE_TYPE_LONGLIFE
			: POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;

	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		val->intval = conservation_start;
		break;

	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = conservation_end;
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

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		if (val->intval < 0 || val->intval > 99)
			return -EINVAL;
		conservation_start = val->intval;
		return 0;

	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		if (val->intval < 1 || val->intval > 100)
			return -EINVAL;
		conservation_end = val->intval;
		return 0;

	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		break;	/* handled below */

	default:
		return -EINVAL;
	}

	/* CHARGE_TYPE handling */
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

	mutex_lock(&chip->lock);
	chip->conservation_mode   = disable;
	conservation_mode_default = disable ? 1 : 0;
	if (!disable) {
		/* Fast mode: always enable charging immediately */
		x120x_gpio_set(chip->gpio_chrg, 0);
	}
	/* In Long life mode GPIO16 is managed by the polling loop */
	mutex_unlock(&chip->lock);

	dev_dbg(&chip->client->dev, "charge_type set to %s\n",
		disable ? "Long life (conservation mode)" : "Fast");

	power_supply_changed(chip->battery);
	return 0;
}

static int x120x_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	return (psp == POWER_SUPPLY_PROP_CHARGE_TYPE ||
		psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD ||
		psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD) ? 1 : 0;
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

	/* Poll immediately on resume so sysfs reflects current state */
	schedule_delayed_work(&chip->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(x120x_pm_ops, x120x_suspend, x120x_resume);

/* -------------------------------------------------------------------------
 * Probe / remove
 * ---------------------------------------------------------------------- */

static int x120x_probe(struct i2c_client *client)
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

	chip->client  = client;
	chip->present = true;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	/* -- regmap -------------------------------------------------------- */
	chip->regmap = devm_regmap_init_i2c(client, &x120x_regmap_config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	/* -- Verify chip identity ----------------------------------------- */
	ret = regmap_read(chip->regmap, MAX17043_REG_VERSION, &version);
	if (ret) {
		dev_err(dev, "failed to read chip version: %d\n", ret);
		return ret;
	}
	dev_info(dev, "MAX1704x at 0x%02x version 0x%03x\n",
		 client->addr, version & MAX17043_VERSION_MASK);

	/* -- GPIO6: AC present -------------------------------------------- */
	chip->gpio_ac = devm_gpiod_get_optional(dev, "ac-present", GPIOD_IN);
	if (IS_ERR(chip->gpio_ac)) {
		ret = PTR_ERR(chip->gpio_ac);
		dev_err(dev, "failed to get ac-present GPIO: %d\n", ret);
		return ret;
	}
	if (!chip->gpio_ac)
		dev_warn(dev,
			 "ac-present GPIO not found - ac_online will always be 0\n"
			 "Install the device tree overlay: dtoverlay=x120x\n");

	/* -- GPIO16: charge control --------------------------------------- */
	/*
	 * Request as output-low so charging remains enabled across a
	 * driver reload, preserving the hardware default.
	 */
	/* Restore charge mode from module parameter (survives reboot) */
	chip->conservation_mode = !!conservation_mode_default;

	chip->gpio_chrg = devm_gpiod_get_optional(dev, "charge-ctrl",
						   GPIOD_OUT_LOW);
	if (IS_ERR(chip->gpio_chrg)) {
		ret = PTR_ERR(chip->gpio_chrg);
		dev_err(dev, "failed to get charge-ctrl GPIO: %d\n", ret);
		return ret;
	}
	if (!chip->gpio_chrg)
		dev_warn(dev,
			 "charge-ctrl GPIO not found - charge_type will be read-only\n"
			 "Install the device tree overlay: dtoverlay=x120x\n");

	/* -- Initial chip setup ------------------------------------------- */
	ret = x120x_clear_alert(chip);
	if (ret)
		dev_warn(dev, "failed to clear ALRT flag: %d\n", ret);

	/*
	 * If the initial SoC is implausible the chip has not converged.
	 * Issue a quick-start and allow 150 ms for the estimate to settle.
	 */
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

	/* -- Register power_supply devices -------------------------------- */

	/*
	 * supplied_to wires the notification chain so that when the AC
	 * adapter or charger device calls power_supply_changed(), the
	 * battery's external_power_changed callback fires immediately.
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

	/* -- Start polling ------------------------------------------------ */
	INIT_DELAYED_WORK(&chip->work, x120x_poll_work);
	schedule_delayed_work(&chip->work, 0);

	dev_info(dev,
		 "x120x UPS ready (battery=%s ac=%s charger=%s)\n",
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
		.pm             = pm_sleep_ptr(&x120x_pm_ops),
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
 * to work on stock Raspberry Pi OS without any DT or config.txt changes,
 * which is the expected experience for most users.
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
			"(tried %d candidate address(es))\n",
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
MODULE_DESCRIPTION("SupTronics X120x UPS HAT power supply driver");
MODULE_LICENSE("GPL v2");

/*
 * DISCLAIMER
 *
 * THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
 * NON-INFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY - INCLUDING BUT
 * NOT LIMITED TO LOSS OF DATA, HARDWARE DAMAGE, FINANCIAL LOSS, OR
 * CONSEQUENTIAL DAMAGES OF ANY KIND - WHETHER IN AN ACTION OF CONTRACT,
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
