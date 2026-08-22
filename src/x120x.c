// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * x120x.c - Power supply driver for SupTronics/Geekworm UPS HAT series
 *
 * Fully supported hardware (identical GPIO and I2C interface):
 *
 *   X1200, X1201, X1202, X1203, X1205, X1206  (Raspberry Pi 5, bottom-mount
 *                                               via pogo pins)
 *   X1207  (Raspberry Pi 5, PoE-powered, pogo pins)
 *   X1208  (Raspberry Pi 5, UPS + NVMe combo, pogo pins)
 *   X1209  (Raspberry Pi 5/4B/3B+/3B, 40-pin GPIO header)
 *
 * Experimental support (untested — board=x728v2 / x728v1 / x708 / x729):
 *
 *   X728 V2.x  (all Pi models, 40-pin header, GPIO26 power-off pulse,
 *               GPIO16 charge control on V2.5 only)
 *   X728 V1.x  (all Pi models, 40-pin header, GPIO13 power-off pulse)
 *   X708       (Pi 4/3, 40-pin header, GPIO13 power-off pulse;
 *               GPIO16 is fan speed — NOT charge control)
 *   X729       (all Pi models, 40-pin header, GPIO26 power-off pulse,
 *               DS1307 RTC and OLED handled by separate kernel drivers)
 *
 * All boards share the MAX17043 fuel gauge on I2C (address 0x36).
 * The X120x series exposes four signals to the Raspberry Pi:
 *
 *   GPIO2 / GPIO3  I2C SDA/SCL to MAX17043 fuel gauge (address 0x36)
 *   GPIO6          AC-present: high = mains OK, low = on battery
 *   GPIO16         Charge control: low = charging enabled (default),
 *                                  high = charging disabled
 *                  NOTE: on X708 GPIO16 controls fan speed, not charging.
 *                        It is never touched by this driver on X708.
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
 * Signed-off-by: Edvard Fielding <mor-lock@users.noreply.github.com>
 *
 * DISCLAIMER: This software is provided "as is" without warranty of any
 * kind.  The author is not liable for any damages arising from its use.
 * This driver interacts with battery hardware; validate correct operation
 * on your specific hardware before relying on it.  USE AT YOUR OWN RISK.
 */

#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
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
	"BCM GPIO for charge control: low=enabled high=disabled (default 16). "
	"Ignored on X708 where GPIO16 is fan speed.");

/*
 * board — selects the board variant.  Controls which GPIOs are claimed
 * and whether a power-off pulse is needed after OS shutdown.
 *
 * Supported values:
 *   "x120x"  (default) — SupTronics X120x series; no power-off GPIO
 *   "x728v2" — Geekworm X728 V2.x / X729; GPIO26 power-off pulse
 *   "x728v1" — Geekworm X728 V1.x;        GPIO13 power-off pulse
 *   "x708"   — Geekworm X708;              GPIO13 power-off pulse,
 *                                           GPIO16 is fan — skip charge ctrl
 *
 * Boards other than x120x are EXPERIMENTAL and untested.
 */
static char *board = "x120x";
module_param(board, charp, 0444);
MODULE_PARM_DESC(board,
	"Board variant: x120x (default), x728v2, x728v1, x708, x729. "
	"Boards other than x120x are EXPERIMENTAL.");

/* Power-off GPIO numbers per board variant (BCM) */
#define X728V2_GPIO_POWEROFF	26
#define X728V1_GPIO_POWEROFF	13
#define X708_GPIO_POWEROFF	13

/*
 * Battery pack energy parameters.
 *
 * battery_mah — total pack capacity in mAh (default 1000).
 *               Set this to your actual pack capacity so that UPower
 *               and desktop environments can display meaningful energy
 *               values and time-to-empty / time-to-full estimates.
 *               Example: 4× 5000 mAh cells → battery_mah=20000
 *
 * Written to /etc/modprobe.d/x120x.conf by the installer.
 */
static int battery_mah = 1000;
module_param(battery_mah, int, 0444);
MODULE_PARM_DESC(battery_mah,
	"Total battery pack capacity in mAh (default 1000)");

/*
 * Charge threshold parameters for Long Life / conservation mode.
 * conservation_end/start are the stop/resume thresholds used in Long
 * Life mode.  Fast mode uses a fixed 100%/95% band instead (see the
 * hysteresis block in x120x_poll_work).
 */
static int conservation_start = 78;
module_param(conservation_start, int, 0444);
MODULE_PARM_DESC(conservation_start,
	"SoC %% at which charging resumes in Long Life mode (default 78). "
	"Set at load time via modprobe.d; change it at runtime through the "
	"charge_control_start_threshold sysfs property, which validates "
	"and locks the update.");

static int conservation_end = 80;
module_param(conservation_end, int, 0444);
MODULE_PARM_DESC(conservation_end,
	"SoC %% at which charging stops in Long Life mode (default 80). "
	"Set at load time via modprobe.d; change it at runtime through the "
	"charge_control_end_threshold sysfs property, which validates "
	"and locks the update.");

/*
 * conservation_mode_default — persists charge mode across reboots.
 * 0 = Fast (default), 1 = Long Life.
 * Automatically updated when charge_type is written via sysfs, and
 * persisted to /etc/modprobe.d/x120x.conf by a udev rule installed
 * by the installer.
 */
static int conservation_mode_default;
module_param(conservation_mode_default, int, 0444);
MODULE_PARM_DESC(conservation_mode_default,
	"Start in Long Life mode (1) or Fast mode (0, default). "
	"Set at load time via modprobe.d; at runtime the mode follows "
	"charge_type sysfs writes (the driver updates this internally and a "
	"udev rule persists it to modprobe.d), not this read-only param.");

/*
 * Kernel-side undervoltage poweroff.  When the hard voltage floor
 * latches, the driver calls orderly_poweroff() directly rather than
 * relying on the UPower/logind userspace chain, which is unreliable on
 * common targets (systemd < 255 ignores logind's HandleLowBattery;
 * UPower may be D-Bus-inactive).  capacity_level=CRITICAL is asserted
 * regardless, so a healthy userspace chain still acts first (earlier,
 * at the 5 %% SoC line).
 */
static int vfloor_poweroff = 1;
module_param(vfloor_poweroff, int, 0444);
MODULE_PARM_DESC(vfloor_poweroff,
	"Call orderly_poweroff() when the on-battery voltage floor latches "
	"(1 = default/enabled, 0 = leave shutdown to userspace UPower/logind). "
	"capacity_level=CRITICAL is asserted either way.");

static int vmin_critical_mv = 3100;
module_param(vmin_critical_mv, int, 0444);
MODULE_PARM_DESC(vmin_critical_mv,
	"On-battery terminal-voltage floor in mV (default 3100).  Held for "
	"20 s it forces CRITICAL and, unless vfloor_poweroff=0, a kernel "
	"poweroff.  Clamped to [2500, 4100] at load.");

static int vfloor_poweroff_dry_run;
module_param(vfloor_poweroff_dry_run, int, 0444);
MODULE_PARM_DESC(vfloor_poweroff_dry_run,
	"1 = log the poweroff decision at emerg instead of calling "
	"orderly_poweroff(), to test the trigger path without shutting down "
	"(default 0).  Test: modprobe ... vmin_critical_mv=4300 "
	"vfloor_poweroff_dry_run=1, unplug AC, watch dmesg, replug.");

/*
 * soc_source — where state-of-charge is derived from.
 *
 *   "voltage" (default): derive SoC from cell voltage via a generic NMC
 *              open-circuit-voltage curve while on battery / at rest.  The
 *              fuel gauge on these boards (a MAX17043-style clone) over-reads
 *              the discharge near full; voltage is accurate there and the
 *              pack's IR drop is negligible (~5 mV).  While charging the
 *              driver automatically defers to the gauge, because charge
 *              current pushes terminal voltage well above the true OCV.
 *   "gauge":   use the raw fuel-gauge SOC register unconditionally.
 *
 * Written to /etc/modprobe.d/x120x.conf by the installer (--soc-source).
 */
static char *soc_source = "voltage";
module_param(soc_source, charp, 0444);
MODULE_PARM_DESC(soc_source,
	"State-of-charge source: \"voltage\" (default, NMC OCV model on "
	"battery, auto-defers to the gauge while charging) or \"gauge\" "
	"(raw MAX17043 SOC register).");

static int pack_resistance_mohm;	/* 0 = use the built-in default */
module_param(pack_resistance_mohm, int, 0444);
MODULE_PARM_DESC(pack_resistance_mohm,
	"Pack DC resistance in milliohms for the voltage observer (valid "
	"10-100; 0 = built-in default 30).  The installer sets this from the "
	"populated cell count (R_board + R_cell/parallel); it only affects "
	"the transient power/rate estimate — the OCV feedback self-anchors "
	"steady SoC — so a wrong value is never unsafe.");

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
/* NOTE: 0% is a valid real reading after deep discharge — do not
 * treat it as implausible.  Only values >100 are truly impossible. */
#define MAX17043_SOC_MIN_PLAUSIBLE	0
#define MAX17043_SOC_MAX_PLAUSIBLE	100

/* -------------------------------------------------------------------------
 * SoC thresholds for CAPACITY_LEVEL
 *
 * Percentage-based thresholds align with UPower's default PercentageAction
 * and PercentageCritical settings so the full chain works consistently:
 * low SoC → capacity_level=Critical → UPower warning-level=action →
 * logind → systemctl poweroff.
 * ---------------------------------------------------------------------- */

/* Dead battery detection thresholds (mirrors Fafnir powerd.py defaults) */
#define X120X_DEAD_BAT_UV		3100000	/* 3.10 V in µV                            */
#define X120X_DEAD_BAT_CONFIRM_US	(600LL * USEC_PER_SEC) /* 10 min window    */
#define X120X_DEAD_BAT_MAX_RISE_UV_H	10000	/* 10 mV/h max rise — still dead           */
#define X120X_DEAD_BAT_SOC_MAX		2	/* only below this SoC %                   */

/*
 * Hard terminal-voltage floor: on battery, a raw cell voltage at/below this,
 * held for the confirm window, forces CAPACITY_LEVEL=CRITICAL regardless of the
 * SoC estimate — the last-ditch backstop against an over-reading SoC.  The
 * confirm window rejects transient load-spike sags (raw V, not EMA).
 */
#define X120X_VMIN_CONFIRM_US		(20LL * USEC_PER_SEC) /* 20 s sustained before firing */

#define X120X_SOC_CRITICAL_PCT	 5	/* CRITICAL below this % → logind poweroff */
#define X120X_SOC_LOW_PCT	10	/* LOW below this % → desktop warning      */
#define X120X_SOC_FULL_PCT	95	/* FULL above this %                        */
#define X120X_FAST_RESUME_PCT	95	/* Fast mode: resume charging at/below this % */
#define X120X_CHG_FULL_DEBOUNCE_MS 3600000 /* gauge must read full this long (1 h) before the 100% charge-off (backstop) */

/*
 * Charge-IC self-termination detector.  GPIO16 only *enables* a Li-ion charger
 * IC that runs its own CC/CV/terminate; at full it stops itself (current tapers
 * below its done-threshold) while GPIO stays asserted, and the terminal drops
 * by the vanished IR/overpotential (~15-30 mV).  So: while charging a 100%
 * target, once V has climbed into the CV zone (>= ARM), a drop of >= VDROP off
 * its running peak = the IC self-terminated = pack full -> inhibit at the real
 * termination instead of trusting the laggy/optimistic gauge=100.
 */
#define X120X_CHG_VDROP_ARM_UV	4200000	/* V must exceed 4.20 V (near-full CV) to arm */
#define X120X_CHG_VDROP_UV	  12000	/* >=12 mV drop off the charge peak = full     */
#define X120X_VDROP_INHIBIT_DELAY_MS 1800000 /* keep charging 30 min after the V-drop before inhibiting */

/*
 * Boot charge-probe.  Rather than trust the gauge to say "full" at reload, on a
 * full-target grid boot actively probe the charger: hold it on for _CHG_MS to
 * let the IC reveal whether it is delivering current (the terminal IR-lifts),
 * then toggle it off for _REST_MS and measure the drop off the reveal-phase
 * peak.  A drop >= _DROP_UV means current was flowing -> not full -> let it top
 * up and terminate on the V-drop; no drop means the pack was already full ->
 * inhibit without a needless top-off (which a reloaded-full pack would never
 * terminate via the V-drop, having run no charge).  Windows are generous vs the
 * ~16 s EMA tau so the drop settles; _DROP_UV sits below the CV surface-charge
 * relaxation so a genuine charge is never missed (erring toward a small top-off).
 */
#define X120X_EDGE_BLANK_MS	 1500	/* after a charger/grid edge, freeze V̄ this long then snap (IR step settle) */
#define X120X_PROBE_CHG_MS	40000	/* hold charger on to reveal charging */
#define X120X_PROBE_REST_MS	25000	/* then inhibit and watch for the drop */
#define X120X_PROBE_DROP_UV	 10000	/* >=10 mV drop over the rest = was charging */

/*
 * Observer overshoot cap.  The OCV tables are extended above 100 % (continuing
 * the top slope) so the observer can track the CV-charge overpotential region
 * instead of railing at the 100 % clamp (which masked the real top-off charge
 * power).  Internal SoC/energy may exceed 100 % up to this cap; the OCV feedback
 * self-limits it to ~101-102 % at the CV terminal, so the cap is only a fault
 * backstop.  The reported CAPACITY %/256 stay clamped to 100 (display).
 */
#define X120X_SOC_MAX_256	(110 * 256)	/* internal SoC may reach 110 % */

/* Manufacturer and model name strings */
#define X120X_MANUFACTURER		"SupTronics"
#define X120X_MODEL_NAME		"X120x"	/* overridden at runtime for X728/X708 */

/* Design voltage limits (Li-ion cell, fixed constants) */
#define X120X_VOLTAGE_MAX_DESIGN_UV	4200000	/* 4.20 V — full charge    */
#define X120X_VOLTAGE_MIN_DESIGN_UV	3200000	/* 3.20 V — safe shutdown  */

/* Mark battery absent after this many consecutive I2C failures */
#define X120X_MAX_ERRORS	5

/* Hardware polling interval */
#define X120X_POLL_MS		500

/*
 * Unconditional power_supply_changed() heartbeat interval.
 * Ensures gnome-power-statistics and other UPower clients receive
 * a continuous stream of data points even when the battery is
 * floating (SoC stable, charger disabled, rate near zero).
 * Expressed in poll ticks: 30 s / 0.5 s = 60 ticks.
 */
#define X120X_HEARTBEAT_TICKS	60


/* -------------------------------------------------------------------------
 * Voltage → state-of-charge model (generic NMC / NCA)
 *
 * These SupTronics boards are fixed-4.2 V Li-ion chargers (terminal 4.23 V),
 * so the only cells they can charge are 4.2 V-class NMC/NCA, which share a
 * near-identical open-circuit-voltage curve.  LiFePO4 is out of scope — a
 * 4.2 V charger would overcharge it — and is not supported by the hardware.
 *
 * The table is rested-OCV vs SoC.  On battery the pack is a low-impedance
 * parallel bank under light load, so the IR drop is only a few mV and the
 * terminal voltage tracks OCV closely.  The model is used on battery / at
 * rest only; while charging, terminal voltage is pushed well above OCV
 * (measured +5..7 % SoC on hardware), so the poll loop uses the gauge.
 *
 * Values are a generic-NMC starting point (seeded from the Molicel P50B
 * curve); refine per-pack against a measured full discharge if desired.
 * Piecewise-linear, interpolated to 1/256 % to match capacity_256.
 * ---------------------------------------------------------------------- */
enum x120x_soc_source { X120X_SOC_SRC_VOLTAGE, X120X_SOC_SRC_GAUGE };

struct x120x_ocv_point {
	int uv;		/* cell voltage in µV      */
	int soc256;	/* SoC × 256 (0 .. 25600)  */
};

/*
 * Full-charge anchor.  X120X_OCV_FULL_UV is the terminal voltage of a freshly
 * charged pack and defines 100% in the discharge table below.  We anchor 100%
 * at the *charged* 4.20 V, not the rested ~4.16 V, on purpose: the discharge
 * OCV table is built from full-start discharges, so the surface-charge
 * relaxation at the top (the fast 4.20 -> 4.13 V sag in the first minutes off
 * the charger) is already baked into the measured 96-100% shape.  That makes a
 * separate RC surface-charge compensation redundant -- the curve carries the
 * transient itself -- so there isn't one.
 */
#define X120X_OCV_FULL_UV        4200000  /* full-charge terminal, µV = 100%      */

/*
 * Physical clamp on the observer power P = I*V.  Right off the charger the
 * surface charge relaxes, so the modelled SoC can fall fast for a minute with
 * little real energy leaving, yielding a transient that reads well above the
 * true load.  A drain never exceeds the Pi+HAT ceiling and a charge never
 * exceeds the supply, so bound P to physical limits.
 */
#define X120X_POWER_MIN_UW   (-12000000) /* max plausible drain, -12 W          */
#define X120X_POWER_MAX_UW    (16000000) /* max plausible charge, +16 W         */

/*
 * Energy scale.  The pack's rated capacity (battery_mah) is measured by the
 * cell datasheet down to 2.5 V at 3.6 V nominal; we only ever use the window
 * from full (X120X_OCV_FULL_UV) down to the 0% cutoff (bottom of the discharge
 * OCV table, 3.2 V), which holds a fixed fraction of rated.  So:
 *
 *   ENERGY_FULL_DESIGN = battery_mah × X120X_NOMINAL_MV        (rated, to 2.5 V)
 *   ENERGY_FULL        = design × X120X_USABLE_PERMILLE / 1000 (usable window)
 *
 * ENERGY_FULL (usable) is also the scale for energy_now and the observer power
 * estimate, so power reads true watts and SoC is linear in energy.  The usable
 * fraction is coulomb-measured: full-range discharge + metered recharge cycles
 * closed the round trip at 63-65.4 Wh usable for this 4×P50B (20 Ah) pack; the
 * 0.900 mid-range value = 64.8/72.0 Wh.  e_full only scales the transient rate
 * (the OCV feedback self-anchors steady SoC), so this need not be exact; refine
 * per pack if desired.
 */
#define X120X_NOMINAL_MV          3600   /* datasheet nominal cell voltage, mV */
#define X120X_USABLE_PERMILLE      900   /* usable (4.20→3.2 V) ÷ rated, ×1000 (coulomb-measured: 64.8/72.0 Wh) */
#define X120X_R_UOHM            30000   /* pack DC resistance, uohm (R_cell 19 + R1 11, GITT-measured) */
#define X120X_R_CHG_UOHM        16000   /* charge-direction R, uohm (IR-step/coulomb; cell lifts ~1.5x fewer mV/A charging) */
#define X120X_R_DIS_UOHM        24000   /* discharge-direction R, uohm (IR-step/coulomb) */
/*
 * Cold-boot seed: on the first sample the observer holds the charger OFF for
 * X120X_SEED_SETTLE_MS and seeds SoC continuously from the terminal voltage via
 * the OCV curve.  On grid the held-off pack rests, so the terminal relaxes to
 * true OCV and the seed converges to the real SoC (no IR needed); off grid the
 * Pi is draining the pack, so the nominal drain IR (X120X_SEED_DIS_UW) is added
 * back.  Two shortcuts skip the settle: a full pack on a 100% charge target
 * (gauge=100) pins straight to full, and a near-empty pack
 * (V <= X120X_SEED_EMPTY_UV) seeds 0% and charges at once — never held off.
 * The steady observer is IR-free and self-anchoring.
 */
#define X120X_SEED_DIS_UW      5000000   /* nominal drain added back on an off-grid seed, +5 W */
#define X120X_SEED_SETTLE_MS    300000   /* boot: hold charger off & seed from OCV this long (5 min) */
#define X120X_SEED_EMPTY_UV    3100000   /* boot: at/below this, seed 0% and charge now (no settle) */

/*
 * Open-circuit-voltage (OCV) curves.  SoC = remaining usable energy vs the
 * cell rest (open-circuit) voltage.  Used by the recursive voltage observer:
 * I = (OCV(SoC) - V)/R, P = I*V, energy integrates (see the poll loop).
 *
 * TWO branches, selected by charge direction, because NMC has a real OCV
 * hysteresis: at a given SoC the rested voltage sits higher just after
 * charging than just after discharging (~115 mV at low SoC, collapsing to ~0
 * above 80%).  The observer uses the discharge table on battery / at rest and
 * the charge table while charging, so the estimate no longer front-loads on
 * the charge leg (a single mean curve read the charge current ~half a
 * hysteresis too high at low SoC).
 *
 * Both tables are the ENERGY-TRUE rested OCV (not load-biased): the mid-range
 * (25-77%) comes from an ECM characterization cycle with settled rest points
 * on BOTH branches, coulomb-anchored; the discharge low-SoC knee (<9%) is a
 * deep-discharge shape scaled to the usable floor (0% = 3.20 V); the charge
 * branch below 23% is the discharge branch plus the measured hysteresis.
 * Molicel INR21700-P50B ×4, 1S4P; 100% = 4.20 V terminal (discharge) /
 * 4.213 V (charge, surface-charge top).  56 points, 1% steps through the knee
 * then 2%.  R is X120X_R_UOHM by default (overridable via
 * pack_resistance_mohm).  NMC-calibrated at ~room temperature; there is
 * no ambient/battery temp sensor so no temp compensation, but SoC is
 * voltage-anchored and shutdown is a fixed voltage floor, so out-of-range
 * temperature degrades accuracy gracefully, never unsafely (it self-reflects a
 * cold pack's reduced capacity and cannot over-discharge a cell).  Other NMC
 * transfers within a few %, LFP is unsupported (flat plateau).
 */
static const struct x120x_ocv_point x120x_ocv_discharge[] = {
	{ 3200000,   0 * 256 },
	{ 3222666,   1 * 256 },
	{ 3245599,   2 * 256 },
	{ 3269438,   3 * 256 },
	{ 3292276,   4 * 256 },
	{ 3313208,   5 * 256 },
	{ 3353747,   6 * 256 },
	{ 3368042,   7 * 256 },
	{ 3382337,   8 * 256 },
	{ 3391339,   9 * 256 },
	{ 3400340,  10 * 256 },
	{ 3412620,  12 * 256 },
	{ 3434801,  14 * 256 },
	{ 3466452,  16 * 256 },
	{ 3482517,  18 * 256 },
	{ 3497110,  20 * 256 },
	{ 3508801,  22 * 256 },
	{ 3528638,  24 * 256 },
	{ 3564820,  26 * 256 },
	{ 3579528,  28 * 256 },
	{ 3580028,  30 * 256 },
	{ 3583796,  32 * 256 },
	{ 3610730,  34 * 256 },
	{ 3621047,  36 * 256 },
	{ 3636515,  38 * 256 },
	{ 3650968,  40 * 256 },
	{ 3678177,  42 * 256 },
	{ 3694833,  44 * 256 },
	{ 3717506,  46 * 256 },
	{ 3747281,  48 * 256 },
	{ 3780706,  50 * 256 },
	{ 3799321,  52 * 256 },
	{ 3814820,  54 * 256 },
	{ 3827400,  56 * 256 },
	{ 3849048,  58 * 256 },
	{ 3869174,  60 * 256 },
	{ 3887933,  62 * 256 },
	{ 3888433,  64 * 256 },
	{ 3888933,  66 * 256 },
	{ 3913395,  68 * 256 },
	{ 3944255,  70 * 256 },
	{ 3970094,  72 * 256 },
	{ 3994260,  74 * 256 },
	{ 4016096,  76 * 256 },
	{ 4024820,  78 * 256 },
	{ 4037907,  80 * 256 },
	{ 4055756,  82 * 256 },
	{ 4064806,  84 * 256 },
	{ 4080038,  86 * 256 },
	{ 4080538,  88 * 256 },
	{ 4097000,  90 * 256 },
	{ 4109000,  92 * 256 },
	{ 4115000,  94 * 256 },
	{ 4138000,  96 * 256 },
	{ 4160000,  98 * 256 },
	{ 4206000, 100 * 256 },
};

static const struct x120x_ocv_point x120x_ocv_charge[] = {
	{ 3315000,   0 * 256 },
	{ 3337666,   1 * 256 },
	{ 3360599,   2 * 256 },
	{ 3384438,   3 * 256 },
	{ 3407276,   4 * 256 },
	{ 3428208,   5 * 256 },
	{ 3445662,   6 * 256 },
	{ 3457521,   7 * 256 },
	{ 3467964,   8 * 256 },
	{ 3479393,   9 * 256 },
	{ 3491690,  10 * 256 },
	{ 3517578,  12 * 256 },
	{ 3543700,  14 * 256 },
	{ 3570008,  16 * 256 },
	{ 3570508,  18 * 256 },
	{ 3571008,  20 * 256 },
	{ 3571508,  22 * 256 },
	{ 3592583,  24 * 256 },
	{ 3617150,  26 * 256 },
	{ 3639276,  28 * 256 },
	{ 3661034,  30 * 256 },
	{ 3681921,  32 * 256 },
	{ 3701039,  34 * 256 },
	{ 3721132,  36 * 256 },
	{ 3740720,  38 * 256 },
	{ 3757885,  40 * 256 },
	{ 3772947,  42 * 256 },
	{ 3789241,  44 * 256 },
	{ 3805304,  46 * 256 },
	{ 3819244,  48 * 256 },
	{ 3834572,  50 * 256 },
	{ 3848818,  52 * 256 },
	{ 3860559,  54 * 256 },
	{ 3873465,  56 * 256 },
	{ 3887996,  58 * 256 },
	{ 3904520,  60 * 256 },
	{ 3923515,  62 * 256 },
	{ 3941217,  64 * 256 },
	{ 3955809,  66 * 256 },
	{ 3972176,  68 * 256 },
	{ 3989805,  70 * 256 },
	{ 4007910,  72 * 256 },
	{ 4027157,  74 * 256 },
	{ 4045896,  76 * 256 },
	{ 4060336,  78 * 256 },
	{ 4074194,  80 * 256 },
	{ 4081769,  82 * 256 },
	{ 4089052,  84 * 256 },
	{ 4093102,  86 * 256 },
	{ 4098340,  88 * 256 },
	{ 4104284,  90 * 256 },
	{ 4113137,  92 * 256 },
	{ 4125844,  94 * 256 },
	{ 4143683,  96 * 256 },
	{ 4176539,  98 * 256 },
	{ 4207671, 100 * 256 },
};

/**
 * x120x_ocv_to_soc256() - map a cell voltage to SoC × 256 over an OCV table.
 * @t:  OCV table (charge or discharge), ascending in both fields.
 * @n:  number of table entries.
 * @uv: cell voltage in microvolts.
 *
 * Linear interpolation, clamped low at 0 but *extrapolated* above the top
 * point along the top segment's slope up to X120X_SOC_MAX_256, so the observer
 * can represent the CV-charge overpotential region as SoC > 100 % rather than
 * railing at 100 %.
 */
static int x120x_ocv_to_soc256(const struct x120x_ocv_point *t, int n, int uv)
{
	int i;

	if (uv <= t[0].uv)
		return 0;
	if (uv >= t[n - 1].uv) {
		/* extrapolate above 100 % along the last segment's slope */
		s64 dv = t[n - 1].uv - t[n - 2].uv;
		s64 ds = t[n - 1].soc256 - t[n - 2].soc256;
		int s = t[n - 1].soc256 +
			(int)div_s64(ds * (uv - t[n - 1].uv), dv);
		return min(s, X120X_SOC_MAX_256);
	}

	for (i = 1; i < n; i++) {
		if (uv <= t[i].uv) {
			s64 dv = t[i].uv - t[i - 1].uv;
			s64 ds = t[i].soc256 - t[i - 1].soc256;

			return t[i - 1].soc256 +
			       (int)div_s64(ds * (uv - t[i - 1].uv), dv);
		}
	}
	return 100 * 256;
}

/**
 * x120x_voltage_soc256() - SoC x256 from a cell voltage via the OCV table.
 * @uv:       open-circuit cell voltage in microvolts (IR-corrected by caller).
 * @charging: true = use the charge-branch table, false = discharge branch.
 */
static int x120x_voltage_soc256(int uv, bool charging)
{
	return charging
		? x120x_ocv_to_soc256(x120x_ocv_charge,
				      ARRAY_SIZE(x120x_ocv_charge), uv)
		: x120x_ocv_to_soc256(x120x_ocv_discharge,
				      ARRAY_SIZE(x120x_ocv_discharge), uv);
}

/**
 * x120x_soc256_to_ocv() - inverse lookup: rest OCV (uV) from SoC x256.
 * @soc256:   state of charge x256 (0 .. 25600).
 * @charging: true = charge-branch OCV, false = discharge branch (hysteresis).
 *
 * Linear interpolation over the selected OCV table (ascending in both fields).
 * Used by the recursive observer to compute I = (OCV(SoC) - V)/R.
 */
static int x120x_soc256_to_ocv(int soc256, bool charging)
{
	const struct x120x_ocv_point *t = charging ? x120x_ocv_charge
						   : x120x_ocv_discharge;
	int n = charging ? ARRAY_SIZE(x120x_ocv_charge)
			 : ARRAY_SIZE(x120x_ocv_discharge);
	int i;

	if (soc256 <= t[0].soc256)
		return t[0].uv;
	if (soc256 >= t[n - 1].soc256) {
		/* extrapolate above 100 % along the last segment's slope so the
		 * inferred current stays continuous when SoC overshoots full */
		s64 ds = t[n - 1].soc256 - t[n - 2].soc256;
		s64 dv = t[n - 1].uv - t[n - 2].uv;
		return t[n - 1].uv +
		       (int)div_s64(dv * (soc256 - t[n - 1].soc256), ds);
	}

	for (i = 1; i < n; i++) {
		if (soc256 <= t[i].soc256) {
			s64 ds = t[i].soc256 - t[i - 1].soc256;
			s64 dv = t[i].uv - t[i - 1].uv;

			return t[i - 1].uv +
			       (int)div_s64(dv * (soc256 - t[i - 1].soc256), ds);
		}
	}
	return t[n - 1].uv;
}


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
 * @gpio_poweroff:	descriptor for the power-off pulse GPIO; NULL on X120x
 * @has_charge_ctrl:	true if the board has charge control (Long Life supported)
 * @lock:		mutex protecting all cached fields below
 * @voltage_uv:		last good VCELL reading in uV
 * @capacity_pct:	last good SOC reading in integer percent (0-100)
 * @capacity_256:	last good SOC as the raw 1/256-percent word (full precision)
 * @ac_online:		1 if mains present, 0 if on battery
 * @conservation_mode:	true when Long life mode is active (charge_type=LONGLIFE)
 * @charger_inhibited:	cached GPIO16 state (true = charging stopped)
 * @charge_full_since:	jiffies the gauge first read full (100%-target stop debounce); 0 = not full
 * @present:		false when consecutive I2C errors exceed threshold
 * @i2c_errors:		consecutive I2C read failure counter
 * @energy_now_uwh:	current energy in uWh (energy_full scaled by SOC)
 * @energy_full_uwh:	usable full-window energy in uWh (rated × usable frac)
 * @energy_empty_uwh:	empty-energy floor in uWh (0, for UPower)
 * @rate_prev_energy_uwh: energy at the previous SOC change (for rate calc)
 * @rate_prev_time_us:	ktime (us) at the previous SOC change
 * @energy_rate_uw:	charge/discharge rate in uW (negative = discharging)
 * @rate_last_change_us: ktime (us) of the last SOC change
 * @dead_cand_start_us:	ktime (us) the cell first dropped below the dead threshold
 * @dead_cand_uv:	cell voltage (uV) when the dead-battery window started
 * @battery_dead:	true once a dead battery is confirmed
 * @work:		delayed work item driving the polling loop
 * @heartbeat_ticks:	poll ticks left until a forced power_supply_changed()
 * @hwmon_dev:		hwmon device exposing voltage/power to sensors
 * @seed_settle_until:	jiffies to hold charger off and OCV-pin SoC (boot re-anchor); 0 = not settling
 * @prev_ac:		last poll's ac_online, for detecting a grid control-edge
 * @prev_chg_inh:	last poll's charger_inhibited, for detecting a charger control-edge
 * @blank_until:	jiffies to freeze V̄ and hold SoC after a control-edge (IR-step settle); 0 = none
 * @vdrop_since:	jiffies of the first V-drop this charge cycle; 0 = none (delays the charger inhibit)
 * @soc_src:		SoC source: voltage OCV observer vs raw fuel gauge
 * @ocv_ema_uv:		EMA of cell voltage feeding the OCV lookup (0 = uninit)
 * @ocv_model_uv:	observer OCV(SoC) estimate in uV, exposed as hwmon in1 (0 = n/a)
 * @r_uohm:		resolved pack DC resistance in uohm (param or built-in default)
 * @r_chg_uohm:		charge-direction pack DC resistance in uohm (asymmetric model)
 * @r_dis_uohm:		discharge-direction pack DC resistance in uohm (asymmetric model)
 * @charge_peak_uv:	peak EMA terminal V while charging, for the V-drop full-detect (0 = not charging)
 * @probe_until:	boot charge-probe: end of the current phase in jiffies (0 = no probe running)
 * @probe_resting:	boot charge-probe phase — false = reveal (charger on), true = measure (charger off)
 * @probe_peak_uv:	peak EMA terminal V observed during the probe reveal phase
 * @obs_primed:		false until energy_now_uwh is seeded from the OCV lookup
 * @obs_prev_us:	ktime (us) of the previous observer step, for dt
 * @power_report_uw:	charge/discharge power reported to the ABI, in uW
 * @raw_capacity_pct:	raw MAX17043 ModelGauge SoC, integer percent
 * @raw_capacity_256:	raw MAX17043 SoC in 1/256 units, for the fractional band compare
 * @vfloor_start_us:	ktime the cell first fell to/below VMIN on battery; 0 = above
 * @vfloor_critical:	true once VMIN held long enough to force SoC-independent CRITICAL
 * @vfloor_poweroff_fired:	one-shot guard: kernel orderly_poweroff() already triggered
 */
struct x120x_chip {
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct power_supply	*battery;
	struct power_supply	*ac;
	struct power_supply	*charger;
	struct gpio_desc	*gpio_ac;
	struct gpio_desc	*gpio_chrg;	/* NULL on X708 (GPIO16=fan) and boards
					 * without charge control */
	struct gpio_desc	*gpio_poweroff;	/* NULL on X120x; pulsed on shutdown */
	bool			 has_charge_ctrl;	/* false = Fast only, no Long Life  */

	struct mutex		 lock;
	int			 voltage_uv;
	int			 capacity_pct;	/* integer percent 0-100        */
	int			 capacity_256;	/* full precision: raw SOC word */
	int			 ac_online;
	bool			 conservation_mode;	/* true = Long life, threshold hysteresis active */
	bool			 charger_inhibited;	/* cached GPIO16 state: true = high (stopped) */
	unsigned long		 charge_full_since;	/* jiffies the gauge first read full (100%-target debounce); 0 = not full */
	unsigned long		 seed_settle_until;	/* jiffies: boot OCV-pin settle until here; 0 = not settling */
	bool			 prev_ac;		/* last poll's ac_online, for control-edge detection */
	bool			 prev_chg_inh;		/* last poll's charger_inhibited, for control-edge detection */
	unsigned long		 blank_until;		/* jiffies: post-edge settle blank (freeze V̄, hold) until here; 0 = none */
	unsigned long		 vdrop_since;		/* jiffies of the first V-drop this charge; 0 = none (delays inhibit) */
	bool			 present;
	int			 i2c_errors;

	/*
	 * Recursive voltage observer (soc_source=voltage): SoC is the running
	 * energy_now_uwh integral of P = I*V, I = (OCV(SoC) - V)/R over the
	 * charge/discharge OCV tables.  It self-anchors to the curve (drift-
	 * free) and yields power for free.  ocv_model_uv caches OCV(SoC) for
	 * the hwmon in1 channel (0 in gauge mode).
	 */
	enum x120x_soc_source	 soc_src;
	int			 ocv_ema_uv;
	int			 ocv_model_uv;
	int			 r_uohm;
	int			 r_chg_uohm;		/* charge-direction pack R, uohm (asymmetric) */
	int			 r_dis_uohm;		/* discharge-direction pack R, uohm (asymmetric) */
	int			 charge_peak_uv;	/* V-drop full-detect: peak EMA V while charging */
	unsigned long		 probe_until;		/* boot charge-probe: current phase end (0 = no probe) */
	bool			 probe_resting;		/* probe: false = reveal (charger on), true = measure (off) */
	int			 probe_peak_uv;		/* peak EMA V during the probe reveal phase */
	bool			 obs_primed;
	s64			 obs_prev_us;
	int			 power_report_uw;	/* power reported to the ABI      */
	int			 raw_capacity_pct;	/* raw MAX17043 ModelGauge SoC % */
	int			 raw_capacity_256;	/* raw MAX17043 SoC, 1/256 % — for the fractional band compare */

	/* Energy tracking for UPower / desktop environment integration */
	s64			 energy_now_uwh;	 /* µWh = energy_full × soc%/100 */
	s64			 energy_full_uwh;	 /* µWh usable = rated × frac    */
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

	/*
	 * Dead battery detection.
	 *
	 * A battery is considered dead if, while on grid power, the cell
	 * voltage remains below 3.10 V for ≥ 10 minutes with no meaningful
	 * voltage rise (< 10 mV/h).  This matches the scenario reported by
	 * multiple X120x users: battery fully discharged, charger reconnected,
	 * but cells will not accept charge and voltage stays stuck near zero.
	 *
	 * Parameters mirror those used by Fafnir powerd.py:
	 *   threshold : 3.10 V
	 *   window    : 600 s
	 *   max rise  : 10 mV/h
	 *   soc ceil  : 2 %
	 */
	s64			 dead_cand_start_us;	/* ktime when below threshold   */
	int			 dead_cand_uv;		/* voltage when cand. started   */
	bool			 battery_dead;		/* confirmed dead battery       */
	s64			 vfloor_start_us;	/* ktime V first <= VMIN on battery; 0 = above */
	bool			 vfloor_critical;	/* VMIN held → SoC-independent CRITICAL */
	bool			 vfloor_poweroff_fired;	/* one-shot: kernel poweroff already triggered */

	struct delayed_work	 work;
	int			 heartbeat_ticks;	/* counts down to forced notify */

	/* hwmon device — exposes voltage and power to sensors/node_exporter */
	struct device		*hwmon_dev;
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

/**
 * x120x_gpio_get() - read a GPIO through the descriptor API
 * @desc: GPIO descriptor, may be NULL
 *
 * May sleep; chip->lock is a mutex, so calling with it held is safe.
 *
 * Return: the GPIO value (0/1), or 0 when @desc is NULL — the safe
 * default for a GPIO the board does not provide.
 */
static int x120x_gpio_get(struct gpio_desc *desc)
{
	if (!desc)
		return 0;	/* GPIO not available: safe default */
	return gpiod_get_value_cansleep(desc);
}

/**
 * x120x_gpio_set() - write a GPIO through the descriptor API
 * @desc: GPIO descriptor; a NULL descriptor is silently ignored
 * @val: value to drive (0 or 1)
 *
 * May sleep; chip->lock is a mutex, so calling with it held is safe.
 */
static void x120x_gpio_set(struct gpio_desc *desc, int val)
{
	if (desc)
		gpiod_set_value_cansleep(desc, val);
}

/* -------------------------------------------------------------------------
 * Polling work item
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Power-off hook (X728 / X708 / X729 only)
 *
 * The UPS board on these variants does not cut power automatically when
 * Linux halts (unlike X120x, which is handled by POWER_OFF_ON_HALT=1 in
 * the bootloader EEPROM).  Instead, a GPIO pulse of ~3 seconds tells
 * the UPS to cut power.
 *
 * We register a SYS_OFF_MODE_POWER_OFF_PREPARE handler rather than the
 * legacy pm_power_off function pointer:
 *
 *   - PREPARE-mode handlers are allowed to sleep (POWER_OFF mode is
 *     not), so we can use msleep for the 3-second pulse rather than
 *     busy-waiting with mdelay.
 *
 *   - The sys-off API supports stacking; we don't unconditionally
 *     clobber a power-off handler installed by another driver.
 *
 *   - devm_register_sys_off_handler ties unregistration to device
 *     lifetime, so remove() no longer has to track and undo the global
 *     pointer manually.
 *
 * The handler runs once, drives the GPIO high for 3 seconds, then
 * releases it.  The UPS hardware sees the pulse and proceeds to cut
 * its 5V rail once the kernel completes its shutdown sequence.
 *
 * EXPERIMENTAL: this entire path is only reached on x728/x708/x729
 * boards, which the author has not tested.
 * ---------------------------------------------------------------------- */

/**
 * x120x_do_poweroff() - sys-off handler: pulse the UPS power-off GPIO
 * @data: sys-off callback data carrying the chip pointer
 *
 * Registered with SYS_OFF_MODE_POWER_OFF_PREPARE on board variants
 * whose UPS needs a GPIO pulse to cut power after shutdown (see the
 * section comment above).  Drives the GPIO high for 3 s, then
 * releases it; the UPS cuts its 5 V rail once the kernel completes
 * its shutdown sequence.
 *
 * Return: NOTIFY_DONE always — without a power-off GPIO there is
 * nothing to do, and after the pulse the shutdown continues.
 */
static int x120x_do_poweroff(struct sys_off_data *data)
{
	struct x120x_chip *chip = data->cb_data;

	if (!chip || !chip->gpio_poweroff)
		return NOTIFY_DONE;

	dev_info(&chip->client->dev,
		 "pulsing power-off GPIO for 3 s\n");
	gpiod_set_value_cansleep(chip->gpio_poweroff, 1);
	msleep(3000);
	gpiod_set_value_cansleep(chip->gpio_poweroff, 0);

	return NOTIFY_DONE;
}

/**
 * x120x_poll_work() - periodic poll: read the gauge, drive the charger
 * @work: the work_struct embedded in struct x120x_chip
 *
 * Runs every X120X_POLL_MS (and immediately on probe, resume, and
 * external-power change).  Reads VCELL and SOC, updates the cached
 * state under chip->lock, runs dead-battery detection, applies the
 * two-threshold charge hysteresis to the charge-control GPIO, and
 * emits power_supply_changed() for whichever supplies changed.
 * Reschedules itself.
 */
static void x120x_poll_work(struct work_struct *work)
{
	struct x120x_chip *chip =
		container_of(work, struct x120x_chip, work.work);
	unsigned int vcell_raw, soc_raw;
	int new_uv, new_pct, new_256, new_ac, ret;
	bool seeding = false;	/* true during the boot OCV-pin settle (charger held off) */
	bool edge = false;	/* true this poll: charger/grid state just changed */
	bool blanking = false;	/* true during the post-edge settle blank (V̄ frozen, hold) */
	bool new_present;
	bool bat_changed = false, ac_changed = false, chrg_changed = false;
	/* Snapshots of shared chip state taken under the lock and used
	 * in the unlocked hysteresis / notification region below.
	 */
	bool conservation_mode_snap = false;
	int  capacity_pct_snap = 0;
	int  poweroff_req = 0;	/* 0=none 1=real 2=dry — decided under lock, acted after unlock */
	int  raw_soc_snap      = 0;
	int  capacity_256_snap = 0;
	int  raw_soc_256_snap  = 0;
	int  ocv_model_uv      = 0;	/* observer OCV(SoC), µV; 0 in gauge mode */

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
		conservation_mode_snap = chip->conservation_mode;
		capacity_pct_snap      = chip->capacity_pct;
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
		conservation_mode_snap = chip->conservation_mode;
		capacity_pct_snap      = chip->capacity_pct;
		mutex_unlock(&chip->lock);
		goto notify;
	}

	new_present       = true;
	new_uv            = MAX17043_VCELL_TO_UV(vcell_raw);
	new_ac            = x120x_gpio_get(chip->gpio_ac);
	if (new_ac < 0)
		new_ac = 0;	/* unreadable: assume on battery (safe) */

	if (chip->soc_src == X120X_SOC_SRC_VOLTAGE) {
		/*
		 * Recursive voltage observer (see the OCV table comment).  The
		 * running energy_now_uwh integral IS the SoC state:
		 *   SoC = energy_now / energy_full
		 *   I   = (OCV(SoC) - V) / R      (discharge +, charge -)
		 *   P   = I * V ;  energy_now -= P * dt
		 * The OCV feedback self-anchors it (drift-free) and compensates
		 * load implicitly, so the steady discharge needs no IR term; P is
		 * the true battery power for free.  The charge/discharge OCV
		 * tables are selected by charge direction (hysteresis).  (A
		 * nominal-IR seed handles cold-boot, and a gauge=100 anchor at
		 * driver start recognises an already-full pack; there is no
		 * runtime pin — the OCV feedback bounds the top on its own.)
		 * V is the EMA-smoothed terminal (raw MAX17043 V is quantised).
		 */
		s64 e_full = div_s64((s64)battery_mah * X120X_NOMINAL_MV *
				     X120X_USABLE_PERMILLE, 1000);
		s64 obs_now_us = ktime_to_us(ktime_get());
		s64 i_ua, p_uw, de_uwh, dt_us;
		int soc256, ocv_uv;
		/*
		 * Charge branch active = on grid AND the charger is not
		 * inhibited.  Picks the charge-hysteresis OCV table while
		 * charging and the discharge table on battery / at float.  Only
		 * the OCV lookup switches; energy_now_uwh (the SoC state) stays
		 * continuous across the flip, so SoC never steps — only the
		 * rate does, and only where it should.
		 */
		bool charging = new_ac && !chip->charger_inhibited;

		/*
		 * Known control edge: the charger toggled or the grid flipped
		 * since last poll.  The terminal steps by I·R about a poll later
		 * (the command precedes the physics), so blank a short window
		 * then snap V̄ to the settled terminal — the IR step lands in the
		 * current immediately, not 16 s later via the EMA.  Between edges
		 * the EMA (τ ≈ 16 s, α = 1/32; +16 rounds the shift to nearest)
		 * smooths the quantised MAX17043 reading.
		 */
		edge = (new_ac != chip->prev_ac) ||
		       (chip->charger_inhibited != chip->prev_chg_inh);
		chip->prev_ac      = new_ac;
		chip->prev_chg_inh = chip->charger_inhibited;
		if (edge)
			chip->blank_until = jiffies +
				msecs_to_jiffies(X120X_EDGE_BLANK_MS);
		blanking = chip->blank_until &&
			   time_before(jiffies, chip->blank_until);

		if (chip->ocv_ema_uv == 0)
			chip->ocv_ema_uv = new_uv;
		else if (blanking)
			;			/* freeze V̄ through the command→IR skew */
		else if (chip->blank_until) {	/* blank just expired: snap */
			chip->ocv_ema_uv  = new_uv;
			chip->blank_until = 0;
		} else
			chip->ocv_ema_uv += (new_uv - chip->ocv_ema_uv + 16) >> 5;

		/*
		 * Seed / settle (see the X120X_SEED_* block up top).  Decide the
		 * strategy on the first sample; then across the settle window the
		 * charger is held off (see charge hysteresis) and SoC is re-seeded
		 * from the OCV curve each poll (just below the integral), so a
		 * rested pack reads true SoC and a wrong seed can't trip a charge.
		 */
		if (!chip->obs_primed) {
			/*
			 * Boot re-anchor.  Always seed SoC from the terminal's
			 * discharge OCV so a boot in any state (including off grid)
			 * starts sane.  If not near-empty, also arm a 5-min settle:
			 * on grid it holds the charger off and OCV-pins SoC while
			 * the terminal relaxes to its true rested OCV — an accurate
			 * re-anchor for a partial pack.  Off grid the seed stands
			 * and the discharge OCV feedback self-corrects.  No probe.
			 * (A full pack's terminal stays rail-high and reads a touch
			 * over 100 %; the next V-drop anchors it exactly.)
			 */
			chip->energy_now_uwh =
				chip->ocv_ema_uv <= X120X_SEED_EMPTY_UV ? 0 :
				div_s64(e_full *
					x120x_voltage_soc256(chip->ocv_ema_uv, false),
					25600);
			if (chip->ocv_ema_uv > X120X_SEED_EMPTY_UV)
				chip->seed_settle_until = jiffies +
					msecs_to_jiffies(X120X_SEED_SETTLE_MS);
			chip->obs_prev_us = obs_now_us;
			chip->obs_primed  = true;
		}

		/*
		 * Boot settle window: OCV-pin only while on grid.  If grid is
		 * absent at boot or drops during the window, seeding falls false
		 * and we integrate the discharge normally (self-correcting and
		 * stable) instead of pinning — the settle then stays aborted.
		 */
		seeding = chip->seed_settle_until &&
			  time_before(jiffies, chip->seed_settle_until) && new_ac;
		if (!seeding)
			chip->seed_settle_until = 0;


		/* dt since last step; guard gaps (resume, missed polls). */
		dt_us = obs_now_us - chip->obs_prev_us;
		chip->obs_prev_us = obs_now_us;
		if (dt_us <= 0 || dt_us > 5LL * USEC_PER_SEC)
			dt_us = (s64)X120X_POLL_MS * 1000;

		/* OCV-lookup SoC may exceed 100 % (extended tables): the observer
		 * tracks the CV overpotential region instead of railing at full. */
		soc256 = clamp((int)div_s64(chip->energy_now_uwh * 25600, e_full),
			       0, X120X_SOC_MAX_256);
		ocv_uv = x120x_soc256_to_ocv(soc256, charging);
		ocv_model_uv = ocv_uv;		/* cache for hwmon in1 (committed under lock) */

		/* I (µA) = (OCV - V)/R : (µV / µΩ) = A, ×1e6 = µA.  R is
		 * direction-dependent (asymmetric): charging (terminal above
		 * OCV) lifts ~1.5× fewer mV/A than discharging. */
		i_ua = div_s64((s64)(ocv_uv - chip->ocv_ema_uv) * 1000000LL,
			       ocv_uv < chip->ocv_ema_uv ? chip->r_chg_uohm
							 : chip->r_dis_uohm);
		/* P (µW) = I(µA) × V(µV) / 1e6 ; bound to physical limits. */
		p_uw = div_s64(i_ua * chip->ocv_ema_uv, 1000000LL);
		p_uw = clamp_t(s64, p_uw, -(s64)X120X_POWER_MAX_UW,
			       -(s64)X120X_POWER_MIN_UW);
		/*
		 * Hold only during an edge blank (terminal mid-step) or the boot
		 * settle — force p=0 so a half-stepped terminal or the settle
		 * can't inject a bogus current.  The float now runs normal
		 * discharge integration (self-correcting via OCV feedback); the
		 * edge-snap already gives it the regime-transition step directly,
		 * so the power reflects the IR step, not the EMA lag.  A better
		 * dedicated float scheme is still TODO.
		 */
		if (blanking || seeding)
			p_uw = 0;
		/* dE (µWh) = P(µW) × dt(µs) / 3.6e9 ; discharge (P>0) lowers E.
		 * div64_s64: the divisor 3.6e9 exceeds s32, so div_s64 (32-bit
		 * divisor) would wrap it — use the 64-bit-divisor variant. */
		de_uwh = div64_s64(p_uw * dt_us, 3600LL * USEC_PER_SEC);
		{
			s64 prev_e = chip->energy_now_uwh;
			/* Upper bound is the 110 % overshoot cap, not e_full: the
			 * extended OCV self-limits the CV overshoot to ~101-102 %,
			 * so energy only rails here on a fault.  Not railing at
			 * e_full is what stops the top-off charge power being masked
			 * as 0 W (the old clamp-at-100 bug). */
			s64 e_max  = div_s64(e_full * X120X_SOC_MAX_256, 25600);
			s64 new_e  = clamp_t(s64, prev_e - de_uwh, 0, e_max);

			/*
			 * If the state railed (full/empty), the excess dE did
			 * not actually flow — report only the power delivered so
			 * a full pack floating on grid reads ~0 W, not a phantom
			 * charge from V sitting just above the 100% OCV.
			 */
			if (new_e != prev_e - de_uwh && dt_us > 0)
				p_uw = div_s64((prev_e - new_e) * 3600LL *
					       USEC_PER_SEC, dt_us);
			chip->energy_now_uwh = new_e;
		}

		/*
		 * No runtime gauge=100 pin.  The observer settles to its own OCV
		 * equilibrium at the top: while charging it tracks into the CV
		 * overpotential region as SoC > 100 % (extended OCV, up to the
		 * X120X_SOC_MAX_256 cap) instead of railing at full — so the real
		 * top-off charge power is reported, not masked as 0 W.  When the
		 * charger IC self-terminates the terminal drops, the observer eases
		 * back to ~100 %/OCV equilibrium, and the V-drop detector in the
		 * charge-control path inhibits.  Drift is OCV-bounded (validated
		 * <0.1 % over a 17 h float); the gauge=100 anchor survives only at
		 * driver start (seed block above) so a freshly-booted full pack
		 * reads 100 % immediately.  Internal SoC may exceed 100 %; the
		 * *reported* CAPACITY %/256 (new_pct/new_256) stay clamped to 100.
		 */

		new_256 = clamp((int)div_s64(chip->energy_now_uwh * 25600, e_full),
				0, 100 * 256);
		new_pct = clamp(new_256 >> 8, 0, 100);

		/* Battery power for the rate/ABI (negative = discharging). */
		chip->energy_rate_uw = clamp((int)-p_uw, X120X_POWER_MIN_UW,
					     X120X_POWER_MAX_UW);

		if (seeding) {
			/*
			 * Boot settle: OCV-pin.  The charger is held off (charge
			 * control), so the terminal relaxes toward its true rested
			 * OCV over the window — pin SoC to the discharge-branch OCV
			 * of the terminal each poll (seeding is on-grid only).  This
			 * is the re-anchor; it reports 0 W and holds no phantom.
			 */
			int seed = x120x_voltage_soc256(chip->ocv_ema_uv, false);

			chip->energy_now_uwh = div_s64(e_full * seed, 25600);
			new_256 = clamp((int)div_s64(chip->energy_now_uwh *
						     25600, e_full), 0, 100 * 256);
			new_pct = clamp(new_256 >> 8, 0, 100);
			chip->energy_rate_uw = 0;
		}
	} else {
		new_pct = clamp(MAX17043_SOC_INT(soc_raw), 0, 100);
		new_256 = MAX17043_SOC_256(soc_raw); /* raw, unclamped for rate */
	}
	mutex_lock(&chip->lock);
	chip->i2c_errors  = 0;
	bat_changed       = (chip->present      != new_present  ||
			     chip->voltage_uv   != new_uv       ||
			     chip->capacity_pct != new_pct      ||
			     chip->capacity_256 != new_256);
	ac_changed        = (chip->ac_online    != new_ac);
	/* conservation_mode is set only by set_property, never read back from GPIO */

	/*
	 * Snapshot old_256 before overwriting chip->capacity_256.
	 * The rate estimator below compares new_256 vs old_256 to detect
	 * a SOC change.  If we updated chip->capacity_256 first, the
	 * comparison would always be equal and no rate would ever be computed.
	 */
	{
		int old_256 = chip->capacity_256;

		chip->present         = new_present;
		chip->voltage_uv      = new_uv;
		chip->capacity_pct    = new_pct;
		chip->capacity_256    = new_256;
		chip->ac_online       = new_ac;
		/* raw MAX17043 ModelGauge SoC, exposed via the raw_capacity
		 * sysfs attr and used for the gauge=100 top anchor */
		chip->raw_capacity_pct = clamp(MAX17043_SOC_INT(soc_raw), 0, 100);
		chip->raw_capacity_256 = clamp(MAX17043_SOC_256(soc_raw), 0, 100 * 256);
		chip->ocv_model_uv     = ocv_model_uv;	/* observer OCV → hwmon in1 */

		/*
		 * energy_full  = battery_mah × 3600 mV × usable-fraction
		 * energy_empty = 0  (floor — lets UPower use energy_now /
		 *                    energy_full directly for percentage)
		 * energy_now   = energy_full × soc% / 100 (usable scale)
		 */
		/*
		 * e_full here is the USABLE-window energy (rated × usable
		 * fraction), not the rated design energy.  Using it as the
		 * scale makes power_now = e_full × dSoC/dt read true watts and
		 * SoC linear in energy.  ENERGY_FULL_DESIGN (rated) is reported
		 * separately in get_property.
		 */
		s64 e_full  = div_s64((s64)battery_mah * X120X_NOMINAL_MV *
				      X120X_USABLE_PERMILLE, 1000);
		/* Use full 16-bit SOC precision (0..25600 = 0..100%) */
		s64 e_now   = div_s64(e_full * new_256, 25600);
		ktime_t now = ktime_get();
		s64 now_us  = ktime_to_us(now);

		/*
		 * Charge/discharge power = dE/dt.  Two estimators branch on
		 * soc_source below: a simple windowed one for the smooth
		 * voltage SoC, then the event-driven one for the gauge.
		 *
		 * Gauge (event-driven): recompute only when the SOC register
		 * changes.
		 * Changes less than 10 s apart are discarded — they indicate
		 * noise or a rapid double-update from the chip rather than a
		 * genuine new measurement.
		 *
		 * rate (µW) = ΔE (µWh) / Δt (µs) × 3600 × 1e6
		 * Sign: negative = discharging, positive = charging.
		 */
		if (chip->soc_src == X120X_SOC_SRC_VOLTAGE) {
			/*
			 * The recursive observer above already updated
			 * energy_now_uwh and set energy_rate_uw (P = I*V);
			 * nothing to do here for the voltage source.
			 */
		} else if (new_256 != old_256) {
			s64 dt = now_us - chip->rate_prev_time_us;

			if (chip->rate_prev_time_us != 0 &&
			    dt >= 10LL * USEC_PER_SEC) {
				s64 de = e_now - chip->rate_prev_energy_uwh;
				/*
				 * Spike rejection: discard the rate update if dt
				 * exceeds the 90 s clamp window.
				 *
				 * When the fuel gauge SoC register is stuck for an
				 * extended period and then jumps by multiple LSBs in
				 * a single tick, dividing that accumulated ΔE by a
				 * clamped dt produces a large transient spike — the
				 * rate appears far higher than reality for one sample.
				 * This is visible as sharp vertical spikes in
				 * gnome-power-statistics rate graphs.
				 *
				 * If dt > 90 s the SoC was frozen long enough that
				 * any single-tick ΔE cannot be trusted as a
				 * per-interval measurement.  Keep the previous rate
				 * estimate rather than emitting a spike.  The baseline
				 * (rate_prev_energy_uwh, rate_prev_time_us) is still
				 * updated so the next tick has a fresh reference.
				 */
				if (dt <= 90LL * USEC_PER_SEC) {
					chip->energy_rate_uw = (int)div_s64(
						de * 3600LL * USEC_PER_SEC, dt);
				}
				/* else: dt was clamped — keep previous estimate */
			}

			/* Always update prev on any SOC change, even discarded */
			chip->rate_prev_energy_uwh  = e_now;
			chip->rate_prev_time_us     = now_us;
			chip->rate_last_change_us   = now_us;
		} else if (chip->rate_last_change_us != 0 &&
			   now_us - chip->rate_last_change_us >
			   90LL * USEC_PER_SEC) {
			/*
			 * SOC unchanged for >90 s: rate is unmeasurably small
			 * (battery floating or negligible load).  Report zero —
			 * we have no way to measure the true self-discharge rate.
			 * Do NOT update rate_prev_time_us — if SOC resumes
			 * changing, dt will be computed from the last real
			 * measurement and clamped to 90 s (see above).
			 */
			chip->energy_rate_uw      = 0;
			chip->rate_last_change_us = 0; /* disarm until next change */
		}

		chip->energy_full_uwh  = e_full;
		chip->energy_empty_uwh = 0;
		/* voltage observer owns energy_now_uwh; only the gauge derives it */
		if (chip->soc_src != X120X_SOC_SRC_VOLTAGE)
			chip->energy_now_uwh = e_now;

		/*
		 * POWER_NOW = the freshly-computed rate, no ABI-side smoothing.
		 * The voltage observer's energy_rate_uw is already V-EMA-smoothed
		 * (τ ≈ 16 s, recomputed every poll) and the gauge supplies its own
		 * event-driven smoothness, so neither path needs an output
		 * filter.  v0.4.8 served energy_rate_uw directly; the later
		 * per-poll EMA (commit 9239c60, 2-min → 8.5-min) only added slew
		 * lag that grossly under-reported real load steps — dropped.
		 */
		chip->power_report_uw = chip->energy_rate_uw;

		/*
		 * Dead battery detection: on grid, voltage stuck below
		 * X120X_DEAD_BAT_UV for ≥ X120X_DEAD_BAT_CONFIRM_US with
		 * no meaningful voltage rise.  Only applies when SoC is
		 * very low (≤ X120X_DEAD_BAT_SOC_MAX %) to avoid false
		 * positives on healthy batteries at rest.
		 */
		if (new_ac && new_uv > 0 &&
		    new_uv < X120X_DEAD_BAT_UV &&
		    new_pct <= X120X_DEAD_BAT_SOC_MAX) {
			if (chip->dead_cand_start_us == 0) {
				/* Start candidate window */
				chip->dead_cand_start_us = now_us;
				chip->dead_cand_uv       = new_uv;
			} else {
				s64 window = now_us - chip->dead_cand_start_us;

				if (window >= X120X_DEAD_BAT_CONFIRM_US) {
					s64 delta_uv = (s64)new_uv - chip->dead_cand_uv;
					s64 window_s = div_s64(window, USEC_PER_SEC);
					s64 rise_uv_h = window_s > 0
						? div_s64(delta_uv * 3600LL, window_s)
						: 0;
					if (rise_uv_h < X120X_DEAD_BAT_MAX_RISE_UV_H) {
						if (!chip->battery_dead) {
							chip->battery_dead = true;
							dev_warn(&chip->client->dev,
								"battery appears dead: "
								"%d mV on grid for %lld s "
								"with <10 mV/h rise\n",
								new_uv / 1000,
								div_s64(window, USEC_PER_SEC));
							bat_changed = true;
						}
					}
				}
			}
		} else {
			/* Condition no longer met — reset candidate window */
			if (chip->dead_cand_start_us != 0 || chip->battery_dead) {
				chip->dead_cand_start_us = 0;
				chip->dead_cand_uv       = 0;
				if (chip->battery_dead) {
					chip->battery_dead = false;
					dev_info(&chip->client->dev,
						 "battery dead flag cleared\n");
					bat_changed = true;
				}
			}
		}

		/*
		 * Hard voltage floor (SoC-independent safety backstop): on
		 * battery, a raw terminal voltage at/below the floor (vmin_critical_mv)
		 * held for X120X_VMIN_CONFIRM_US forces CRITICAL regardless of the
		 * SoC estimate — the last line of defence if the observer ever
		 * reads high while the pack is genuinely empty.  The confirm
		 * window rejects transient load-spike sags.
		 */
		{
			int vmin_uv = vmin_critical_mv * 1000;

			if (!new_ac && new_uv > 0 && new_uv <= vmin_uv) {
				if (chip->vfloor_start_us == 0) {
					chip->vfloor_start_us = now_us;
				} else if (now_us - chip->vfloor_start_us >=
						X120X_VMIN_CONFIRM_US &&
					   !chip->vfloor_critical) {
					chip->vfloor_critical = true;
					bat_changed = true;
					dev_warn(&chip->client->dev,
						 "terminal %d mV <= %d mV on battery for %lld s — CRITICAL (SoC-independent floor)\n",
						 new_uv / 1000, vmin_critical_mv,
						 div_s64(now_us - chip->vfloor_start_us,
							 USEC_PER_SEC));
					if (vfloor_poweroff) {
						if (vfloor_poweroff_dry_run)
							poweroff_req = 2;
						else if (!chip->vfloor_poweroff_fired) {
							chip->vfloor_poweroff_fired = true;
							poweroff_req = 1;
						}
					}
				}
			} else if (chip->vfloor_start_us || chip->vfloor_critical) {
				chip->vfloor_start_us = 0;
				chip->vfloor_critical = false;
				bat_changed = true;
			}
		}
	} /* end chip state update and rate estimation */

	conservation_mode_snap = chip->conservation_mode;
	capacity_pct_snap      = chip->capacity_pct;
	raw_soc_snap           = chip->raw_capacity_pct;
	capacity_256_snap      = chip->capacity_256;
	raw_soc_256_snap       = chip->raw_capacity_256;

	/*
	 * Charge hysteresis.
	 *
	 * GPIO16 is driven from two SoC thresholds, so the charger does not
	 * micro-cycle at the top and the pack is allowed to self-discharge
	 * back down to the resume point before topping up again:
	 *   capacity >= end_thr   → stop charging   (GPIO16 high)
	 *   capacity <= start_thr → resume charging (GPIO16 low)
	 *   in between            → hold the current state (hysteresis band)
	 *
	 * Fast mode:      end_thr = 100, start_thr = X120X_FAST_RESUME_PCT
	 *                 (95).  Long Life mode: end_thr = conservation_end
	 *                 (80), start_thr = conservation_start (78).
	 *
	 * Safety: chip->charger_inhibited starts false (charging enabled),
	 * and any SoC at or below start_thr forces the charger on.  So the
	 * charger is always on at boot, after a deep discharge, and in any
	 * low-SoC state — it is never harmful to charge too much, but it is
	 * harmful to leave a low battery uncharged.  Only an in-band reading
	 * (start_thr < SoC < end_thr) holds the previous state.
	 *
	 * On battery (no AC) GPIO16 is irrelevant — the charger cannot
	 * run without input power — but we manage the cached state
	 * regardless so sysfs reads stay consistent.
	 *
	 * Performed under chip->lock so the read-modify-write of GPIO16
	 * and chip->charger_inhibited is atomic with respect to
	 * x120x_charger_set_property when the user toggles charge_type.
	 * gpiod_set_value_cansleep is safe to call under a mutex.
	 */
	if (chip->gpio_chrg) {
		int start_thr, end_thr, soc_band, soc_band_256;
		bool full_target, want_inhibit, vdrop_full = false;

		if (conservation_mode_snap) {
			/* Long Life: user-configured band */
			end_thr   = conservation_end;
			start_thr = conservation_start;
		} else {
			/* Fast: float-protection band at the top */
			end_thr   = 100;
			start_thr = X120X_FAST_RESUME_PCT;
		}

		/* Defensive: never let a misconfigured band invert */
		if (start_thr >= end_thr)
			start_thr = end_thr - 1;

		/*
		 * A 100% stop target — Fast mode, or a Long Life band configured
		 * all the way up to 100% — keys on the RAW MAX17043 gauge: the
		 * fused SoC only approaches full asymptotically at CV, so keying
		 * the stop on it would never fire and the charger would float at
		 * CV forever, whereas the raw gauge reliably reaches 100 at a full
		 * pack.  Any sub-100 Long Life band keys on the reported
		 * (observer) SoC, which is accurate mid-range.
		 */
		full_target = (end_thr >= 100);
		soc_band     = full_target ? raw_soc_snap     : capacity_pct_snap;
		/*
		 * Compare at 1/256-% (fractional), not the floored integer.  On the
		 * rising cut edge floor() already lands on the true threshold, but
		 * on the falling resume edge it fires ~1% high (floor(78.9)=78), so
		 * the integer band would resume at true ~start+1.  The fractional
		 * word makes both edges hit the true SoC — an exact [start, end].
		 */
		soc_band_256 = full_target ? raw_soc_256_snap : capacity_256_snap;

		/*
		 * Charge-IC self-termination detector (voltage model, 100 %
		 * target, on grid, charger currently on).  Track the peak of the
		 * EMA terminal while charging; once it has climbed into the CV
		 * zone (>= ARM), a drop of >= VDROP off that peak means the IC
		 * stopped delivering current — the pack is full.  Fires at the
		 * real termination, ahead of the gauge=100 debounce.  The EMA
		 * (tau ~16 s) rejects load transients; gated on new_ac so a grid
		 * loss (which also drops V) can never look like "full".
		 */
		if (chip->soc_src == X120X_SOC_SRC_VOLTAGE && full_target &&
		    new_ac && !chip->charger_inhibited && !chip->probe_until) {
			if (chip->ocv_ema_uv > chip->charge_peak_uv)
				chip->charge_peak_uv = chip->ocv_ema_uv;
			if (chip->charge_peak_uv >= X120X_CHG_VDROP_ARM_UV &&
			    chip->charge_peak_uv - chip->ocv_ema_uv >=
			    X120X_CHG_VDROP_UV)
				vdrop_full = true;
		} else {
			chip->charge_peak_uv = 0;	/* reset when not charging */
		}

		/*
		 * Two-threshold hysteresis.  Stop edge:
		 *
		 *  - 100% target: hold the charger on for
		 *    X120X_CHG_FULL_DEBOUNCE_MS after the gauge first reads full,
		 *    so the CV taper tops the pack off and a single transient
		 *    100% reading can't cut early.
		 *  - sub-100 Long Life band: nothing to top off past the band, so
		 *    cut the charger immediately when the observer SoC reaches
		 *    end_thr — a debounce here would overshoot the band top by the
		 *    several % the charger delivers while it waits.
		 *
		 * Resume at start_thr; hold state in between.  Defaulting to the
		 * held state in-band (plus resume at/below start_thr) keeps the
		 * charger on at boot and after a deep discharge.
		 */
		if (soc_band_256 >= end_thr * 256) {
			if (full_target) {
				if (!chip->charge_full_since)
					chip->charge_full_since = jiffies;
				want_inhibit = chip->charger_inhibited ||
					time_after_eq(jiffies,
						chip->charge_full_since +
						msecs_to_jiffies(X120X_CHG_FULL_DEBOUNCE_MS));
			} else {
				chip->charge_full_since = 0;
				want_inhibit = true;
			}
		} else {
			chip->charge_full_since = 0;
			if (soc_band_256 <= start_thr * 256)
				want_inhibit = false;
			else
				want_inhibit = chip->charger_inhibited;
		}

		/*
		 * Seed settle overrides the band: hold the charger off so the
		 * pack rests and the observer seeds from true OCV.
		 */
		if (seeding)
			want_inhibit = true;

		/*
		 * Charge-IC self-termination: inhibit at the real full, ahead of
		 * (and independent of) the gauge=100 band edge.  Clear the gauge
		 * debounce so a later gauge=100 doesn't re-arm anything.
		 */
		/*
		 * Latch the V-drop.  The IC self-termination is the one on-grid
		 * "full" reference we trust.  Latch it (it survives the detector
		 * re-arming across the hold) until the pack drains back to the
		 * resume band = a new charge cycle.
		 */
		if (vdrop_full && !chip->vdrop_since)
			chip->vdrop_since = jiffies;
		if (soc_band_256 <= start_thr * 256)
			chip->vdrop_since = 0;
		if (chip->vdrop_since) {
			chip->charge_full_since = 0;
			/*
			 * Keep the charger on until >=30 min past the drop, then
			 * inhibit.  No hard SoC pin: during the saturation hold the
			 * observer integrates up to ~full on the (near-CV) terminal
			 * smoothly — no jump — and the float then integrates on from
			 * there.  Overrides any band cut for the duration.
			 */
			want_inhibit = time_after_eq(jiffies, chip->vdrop_since +
				msecs_to_jiffies(X120X_VDROP_INHIBIT_DELAY_MS));
		}

		/*
		 * Boot charge-probe: overrides the band and drives the charger.
		 * Reveal phase — hold the charger on and track the peak EMA V.
		 * Measure phase — hold it off; at the end a drop >= _DROP_UV off
		 * that peak means current was flowing (not full) -> enable so it
		 * tops up and terminates on the V-drop; no drop means already
		 * full -> inhibit.  Either outcome is carried forward by the
		 * band's held state once the probe clears.
		 */
		if (chip->probe_until) {
			if (!chip->probe_resting) {
				if (chip->ocv_ema_uv > chip->probe_peak_uv)
					chip->probe_peak_uv = chip->ocv_ema_uv;
				want_inhibit = false;	/* reveal: keep charging */
				if (time_after_eq(jiffies, chip->probe_until)) {
					chip->probe_resting = true;
					chip->probe_until   = jiffies +
					   msecs_to_jiffies(X120X_PROBE_REST_MS);
				}
			} else if (time_after_eq(jiffies, chip->probe_until)) {
				/* measure done: decide full vs charging */
				int drop = chip->probe_peak_uv -
					   chip->ocv_ema_uv;

				want_inhibit = drop < X120X_PROBE_DROP_UV;
				chip->probe_until       = 0;
				chip->probe_peak_uv     = 0;
				chip->charge_full_since = 0;
				/*
				 * Charging: the probe seeded SoC off the still-lifted
				 * terminal (25 s doesn't fully relax the surface
				 * charge), which reads > 100 % and would leave the
				 * observer parked at equilibrium — masking the top-off
				 * power.  Cap the seed just under full so it tracks the
				 * remaining charge and reports the real power; it re-
				 * converges to the CV terminal within a poll or two.
				 */
				if (!want_inhibit) {
					s64 cap = div_s64(chip->energy_full_uwh *
						(100 * 256 - 256), 25600);
					if (chip->energy_now_uwh > cap)
						chip->energy_now_uwh = cap;
				}
				dev_info(&chip->client->dev,
					 "charge-probe: %s (drop %d uV)\n",
					 want_inhibit ? "full -> inhibit"
						      : "charging -> enable",
					 drop);
			} else {
				want_inhibit = true;	/* measure: charger off */
			}
		}

		if (want_inhibit != chip->charger_inhibited) {
			x120x_gpio_set(chip->gpio_chrg, want_inhibit ? 1 : 0);
			chip->charger_inhibited = want_inhibit;
			dev_dbg(&chip->client->dev,
				"%s mode: %s charging at %d%% (band SoC)\n",
				conservation_mode_snap ? "conservation" : "float",
				want_inhibit ? "stopped" : "resumed",
				soc_band);
			chrg_changed = true;
			bat_changed  = true;
		}
	}

	mutex_unlock(&chip->lock);

notify:
	/*
	 * Emit uevents only when state actually changed to avoid storms.
	 * power_supply_changed() must NOT be called under chip->lock — it
	 * can call back into our get_property handlers which take the
	 * same lock.
	 */

	if (chrg_changed)
		power_supply_changed(chip->charger);
	if (ac_changed)
		power_supply_changed(chip->ac);
	/*
	 * Notify battery consumers immediately on any real change, or
	 * unconditionally every X120X_HEARTBEAT_TICKS poll ticks (~30 s).
	 *
	 * On AC state change we deliberately skip the immediate battery
	 * notification and let the next poll tick deliver it instead.
	 * This gives the I2C values one poll interval (500 ms) to settle
	 * before UPower reads them, preventing the spurious 0.000/unknown
	 * history entries that corrupt gnome-power-statistics graphs.
	 */
	if (bat_changed || --chip->heartbeat_ticks <= 0) {
		power_supply_changed(chip->battery);
		chip->heartbeat_ticks = X120X_HEARTBEAT_TICKS;
	}

	/*
	 * Kernel-side undervoltage poweroff, decided under the lock above.
	 * Called here (never under chip->lock) because orderly_poweroff()
	 * runs the OS shutdown; on x728 the sys-off handler then pulses the
	 * power-off GPIO, on x120x the EEPROM POWER_OFF_ON_HALT cuts the
	 * rail at halt.  force=false keeps it graceful — the floor leaves
	 * margin before boost-UVLO, and a clean unmount protects the disk.
	 */
	if (poweroff_req == 1) {
		dev_emerg(&chip->client->dev,
			  "undervoltage floor reached on battery — initiating orderly poweroff\n");
		orderly_poweroff(false);
	} else if (poweroff_req == 2) {
		dev_emerg(&chip->client->dev,
			  "DRY RUN: undervoltage floor reached — would call orderly_poweroff()\n");
	}

	schedule_delayed_work(&chip->work, msecs_to_jiffies(X120X_POLL_MS));
}

/* -------------------------------------------------------------------------
 * power_supply callbacks - battery
 * ---------------------------------------------------------------------- */

static enum power_supply_property x120x_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_SCOPE,
};

/**
 * x120x_battery_get_property() - power_supply getter for the battery
 * @psy: the x120x-battery power_supply
 * @psp: property being read
 * @val: result
 *
 * Serves every property from the cached state maintained by the poll
 * loop — no I2C traffic on the sysfs read path.
 *
 * Return: 0 on success, -EINVAL for unsupported properties.
 */
static int x120x_battery_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	int ac_online, capacity_pct, capacity_256, voltage_uv, energy_rate_uw;
	s64 energy_now_uwh, energy_full_uwh;
	bool present, conservation_mode, battery_dead, charger_inhibited, vfloor_critical;

	mutex_lock(&chip->lock);
	ac_online        = chip->ac_online;
	capacity_pct     = chip->capacity_pct;
	capacity_256     = chip->capacity_256;
	voltage_uv       = chip->voltage_uv;
	present          = chip->present;
	conservation_mode = chip->conservation_mode;
	charger_inhibited = chip->charger_inhibited;
	energy_now_uwh  = chip->energy_now_uwh;
	energy_full_uwh = chip->energy_full_uwh;
	energy_rate_uw  = chip->power_report_uw;	/* power for the ABI */
	battery_dead    = chip->battery_dead;
	vfloor_critical = chip->vfloor_critical;
	mutex_unlock(&chip->lock);

	/*
	 * conservation_mode + charger_inhibited fully describe the charger
	 * state from this snapshot.  No need to touch the GPIO directly:
	 * the cached value is what the poll loop and set_property write
	 * under the same lock, so it cannot disagree with conservation_mode.
	 */
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS: {
		bool chrg_inhibited = conservation_mode && charger_inhibited;

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

	case POWER_SUPPLY_PROP_HEALTH:
		if (!present)
			val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
		else if (battery_dead)
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = present ? 1 : 0;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = voltage_uv;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = X120X_VOLTAGE_MAX_DESIGN_UV;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = X120X_VOLTAGE_MIN_DESIGN_UV;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = capacity_pct;
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		/*
		 * SoC%-based level classification.  Thresholds align with
		 * UPower's default PercentageAction (2%) and PercentageCritical
		 * (5%) so the shutdown chain fires consistently:
		 *   capacity < X120X_SOC_CRITICAL_PCT → CRITICAL
		 *     → UPower warning-level=action → logind poweroff
		 *   capacity < X120X_SOC_LOW_PCT → LOW → desktop warning
		 */
		if (!present) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		} else if (!ac_online && (capacity_pct < X120X_SOC_CRITICAL_PCT ||
					  vfloor_critical)) {
			/* Only report CRITICAL on battery — on AC the battery is
			 * charging and shutting down would cause a livelock after
			 * a deep discharge event.  vfloor_critical is the hard
			 * voltage backstop for when the SoC estimate reads high. */
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		} else if (capacity_pct < X120X_SOC_LOW_PCT) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		} else if (capacity_pct >= X120X_SOC_FULL_PCT) {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_FULL;
		} else {
			val->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		}
		break;

	case POWER_SUPPLY_PROP_CHARGE_NOW:
		/*
		 * Charge in µAh.  Uses full 16-bit SOC precision (capacity_256,
		 * range 0..25600 = 0..100%) to match the energy model and avoid
		 * losing the sub-1% fractional part.
		 *   charge_now_uah = battery_mah × 1000 × capacity_256 / 25600
		 */
		/* usable-basis charge_now (matches CHARGE_FULL below) */
		val->intval = (int)div_s64(
			(s64)battery_mah * X120X_USABLE_PERMILLE * capacity_256,
			25600);
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL:
		/* usable window: rated µAh × usable fraction */
		val->intval = (int)div_s64(
			(s64)battery_mah * 1000 * X120X_USABLE_PERMILLE, 1000);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = battery_mah * 1000; /* rated µAh (to 2.5 V) */
		break;

	case POWER_SUPPLY_PROP_CHARGE_EMPTY:
		val->intval = 0;
		break;

	case POWER_SUPPLY_PROP_ENERGY_NOW:
		/*
		 * energy_now in µWh.  The power_supply ABI uses µWh as the
		 * unit for energy properties (confusingly named _NOW/_FULL).
		 * Reported up to the observer's 110 % overshoot cap (not clamped
		 * at energy_full) so the CV-charge overshoot above 100 % is
		 * visible in userspace; UPower clamps its own icon at 100 %.
		 */
		val->intval = (int)clamp(energy_now_uwh, (s64)0,
			div_s64(energy_full_uwh * X120X_SOC_MAX_256, 25600));
		break;

	case POWER_SUPPLY_PROP_ENERGY_FULL:
		val->intval = (int)energy_full_uwh;
		break;

	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		/* rated energy to 2.5 V (battery_mah × nominal); usable
		 * ENERGY_FULL above is this × the usable fraction. */
		val->intval = (int)((s64)battery_mah * X120X_NOMINAL_MV);
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

	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = X120X_MANUFACTURER;
		break;

	case POWER_SUPPLY_PROP_MODEL_NAME:
		if (!board || !strcmp(board, "x120x"))
			val->strval = "X120x";
		else if (!strcmp(board, "x728v2") || !strcmp(board, "x728v1"))
			val->strval = "X728";
		else if (!strcmp(board, "x708"))
			val->strval = "X708";
		else if (!strcmp(board, "x729"))
			val->strval = "X729";
		else
			val->strval = X120X_MODEL_NAME;
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

/**
 * x120x_battery_external_power_changed() - upstream supply changed
 * @psy: the x120x-battery power_supply
 *
 * Called by the power_supply core when a supply feeding this battery
 * (x120x-ac) changes state.  Re-polls immediately so battery status
 * flips promptly on plug/unplug instead of waiting out the poll
 * interval.
 */
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

/**
 * x120x_ac_get_property() - power_supply getter for the AC adapter
 * @psy: the x120x-ac power_supply
 * @psp: property being read
 * @val: result
 *
 * Return: 0 on success, -EINVAL for any property other than ONLINE.
 */
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

/**
 * x120x_charger_get_property() - power_supply getter for the charger
 * @psy: the x120x-charger power_supply
 * @psp: property being read
 * @val: result
 *
 * ONLINE mirrors AC presence; STATUS distinguishes an actively
 * charging charger from one inhibited by the hysteresis band;
 * CHARGE_TYPE reports Fast vs Long Life; the two threshold
 * properties report the Long Life band even in Fast mode (Fast uses
 * a fixed band the standard sysfs interface cannot express).
 *
 * Return: 0 on success, -EINVAL for unsupported properties.
 */
static int x120x_charger_get_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	bool conservation_mode, charger_inhibited;
	int ac_online;

	mutex_lock(&chip->lock);
	conservation_mode = chip->conservation_mode;
	charger_inhibited = chip->charger_inhibited;
	ac_online         = chip->ac_online;
	mutex_unlock(&chip->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = ac_online;
		break;

	case POWER_SUPPLY_PROP_STATUS:
		if (!ac_online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (charger_inhibited)
			/* AC present but the charger is held off (Fast float-protect
			 * or Long-Life band) — not delivering charge in either mode. */
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

/**
 * x120x_charger_set_property() - power_supply setter for the charger
 * @psy: the x120x-charger power_supply
 * @psp: property being written
 * @val: value to set
 *
 * Handles the Fast / Long Life charge_type switch and the
 * conservation-band thresholds.  On boards without charge control a
 * Long Life or threshold write is rejected with -EOPNOTSUPP (a Fast
 * write is an accepted no-op); threshold writes are range-checked
 * and must keep start < end.  A charge_type write also updates
 * conservation_mode_default so the udev persistence hook can save
 * the mode across reboots.
 *
 * Return: 0 on success, -EOPNOTSUPP without charge control, -EINVAL
 * for out-of-range values or unsupported properties.
 */
static int x120x_charger_set_property(struct power_supply *psy,
				       enum power_supply_property psp,
				       const union power_supply_propval *val)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);
	bool disable;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		if (!chip->has_charge_ctrl)
			return -EOPNOTSUPP;
		if (val->intval < 0 || val->intval > 99)
			return -EINVAL;
		/*
		 * The poll loop reads both thresholds under chip->lock, so
		 * hold it here to make this read-modify-write atomic against
		 * it.  Reject a start point that meets or crosses the current
		 * stop point, which would invert the hysteresis band.  The
		 * poll loop keeps a defensive clamp as belt-and-braces.
		 */
		mutex_lock(&chip->lock);
		if (val->intval >= conservation_end) {
			mutex_unlock(&chip->lock);
			return -EINVAL;
		}
		conservation_start = val->intval;
		mutex_unlock(&chip->lock);
		return 0;

	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		if (!chip->has_charge_ctrl)
			return -EOPNOTSUPP;
		if (val->intval < 1 || val->intval > 100)
			return -EINVAL;
		mutex_lock(&chip->lock);
		if (val->intval <= conservation_start) {
			mutex_unlock(&chip->lock);
			return -EINVAL;
		}
		conservation_end = val->intval;
		mutex_unlock(&chip->lock);
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
		if (!chip->has_charge_ctrl) {
			dev_warn(&chip->client->dev,
				 "Long Life mode not supported on this board "
				 "(no charge control GPIO)\n");
			return -EOPNOTSUPP;
		}
		disable = true;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&chip->lock);
	chip->conservation_mode   = disable;
	conservation_mode_default = disable ? 1 : 0;
	if (!disable) {
		/*
		 * Switching to Fast mode: enable charging immediately so
		 * the battery starts charging without waiting for the next
		 * poll.  The poll loop will apply float-protection from the
		 * next tick onward.  Both the hardware GPIO and the cached
		 * flag are updated under the same lock so any concurrent
		 * sysfs read sees a consistent state.
		 */
		x120x_gpio_set(chip->gpio_chrg, 0);
		chip->charger_inhibited = false;
	}
	/* GPIO16 is managed by the polling loop in both modes */
	mutex_unlock(&chip->lock);

	dev_dbg(&chip->client->dev, "charge_type set to %s\n",
		disable ? "Long life (conservation mode)" : "Fast");

	/*
	 * Notify the charger supply synchronously (outside chip->lock —
	 * power_supply_changed() must not be called under it).  This fires
	 * the charger uevent on the write itself, so the udev charge-mode
	 * persistence runs immediately instead of waiting for the next
	 * poll/heartbeat cycle to emit it.
	 */
	power_supply_changed(chip->charger);
	power_supply_changed(chip->battery);
	return 0;
}

/**
 * x120x_charger_property_is_writeable() - sysfs writability for the charger
 * @psy: the x120x-charger power_supply
 * @psp: property being queried
 *
 * Return: nonzero for charge_type and the two thresholds when the
 * board has charge control; 0 otherwise, making the files read-only
 * exactly as the probe warning promises.
 */
static int x120x_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	struct x120x_chip *chip = power_supply_get_drvdata(psy);

	/*
	 * Without the charge-control GPIO (no overlay, or a board that
	 * lacks it) the driver cannot honour any of these writes, so
	 * expose the sysfs files read-only as the probe warning promises.
	 */
	if (!chip->has_charge_ctrl)
		return 0;

	return psp == POWER_SUPPLY_PROP_CHARGE_TYPE ||
	       psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD ||
	       psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

/* -------------------------------------------------------------------------
 * power_supply descriptors
 * ---------------------------------------------------------------------- */

static const char * const x120x_ac_supplied_to[] = { "x120x-battery" };
static const char * const x120x_charger_supplied_to[] = { "x120x-battery" };

/*
 * raw_capacity - non-standard sysfs attribute exposing the MAX17043's own
 * ModelGauge SoC (%), separate from the observer's CAPACITY property.  It
 * feeds the gauge=100 top anchor and is logged alongside the observer SoC for
 * validation.  Added via power_supply_config.attr_grp so its lifetime is
 * managed by the power_supply core -- it does not touch the standard
 * property set or the hwmon interface.
 */
static ssize_t raw_capacity_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct x120x_chip *chip = power_supply_get_drvdata(to_power_supply(dev));
	int pct;

	mutex_lock(&chip->lock);
	pct = chip->raw_capacity_pct;
	mutex_unlock(&chip->lock);

	return sysfs_emit(buf, "%d\n", pct);
}
static DEVICE_ATTR_RO(raw_capacity);

static struct attribute *x120x_battery_extra_attrs[] = {
	&dev_attr_raw_capacity.attr,
	NULL,
};
ATTRIBUTE_GROUPS(x120x_battery_extra);

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
 * hwmon interface
 *
 * Exposes battery measurements to the standard Linux hardware monitoring
 * subsystem, making the driver compatible with sensors(1), Prometheus
 * node_exporter, collectd, Grafana, and any other tool that reads the
 * standard hwmon sysfs interface — without requiring custom configuration.
 *
 * Channels and units follow Documentation/hwmon/sysfs-interface.rst:
 *
 *   in0     — cell voltage in millivolts          (label: "cell_voltage")
 *             Direct hardware reading from MAX17043 VCELL register.
 *             hwmon unit: mV.  node_exporter: node_hwmon_in_volts.
 *             Carries voltage safety limits:
 *               in0_lcrit       — 3.00 V on-battery critical floor.
 *               in0_min         — 3.20 V design minimum (0 % SoC / empty).
 *               in0_lcrit_alarm — 1 once the floor has been held long
 *                                 enough to force CRITICAL and drive the
 *                                 OS shutdown chain.
 *
 *   in1     — observer OCV estimate in millivolts  (label: "cell_ocv")
 *             The voltage model's rested open-circuit-voltage estimate at
 *             the current SoC — what the terminal would relax to at rest.
 *             in0 − in1 is therefore the live IR drop (I·R).  Present only
 *             with the default voltage model; hidden under soc_source=gauge.
 *             hwmon unit: mV.  node_exporter: node_hwmon_in_volts (in1).
 *
 *   curr1   — charge/discharge current in milliamps (label: "battery_current")
 *             Derived: I (mA) = power_rate (µW) / voltage (µV) × 1000.
 *             Sign convention (hwmon ABI): positive = charging,
 *             negative = discharging.
 *             hwmon unit: mA.  node_exporter: node_hwmon_curr_amps.
 *             NOTE: derived estimate — see power1 note below.
 *
 *   power1  — charge/discharge power in microwatts (label: "battery_power")
 *             Sign convention: positive = charging, negative = discharging.
 *             hwmon unit: µW.  node_exporter: node_hwmon_power_watt.
 *             NOTE: not a direct measurement (the MAX17043 has no current
 *             sense).  With the voltage model it is the observer's net
 *             battery power P = I·V; with soc_source=gauge it is derived
 *             from SoC slope × pack capacity × nominal voltage and lags
 *             during rapid transitions and before the gauge has converged.
 *
 *   energy1 — cumulative energy in microjoules      (label: "battery_energy")
 *             Derived: E (µJ) = energy_now (µWh) × 3600.
 *             Represents current stored energy relative to empty, not
 *             cumulative energy delivered since boot.
 *             hwmon unit: µJ.  node_exporter: node_hwmon_energy_joules.
 *
 * node_exporter (--collector.hwmon, enabled by default) exposes these as:
 *   node_hwmon_in_volts{chip="x120x",sensor="in0"}          (terminal)
 *   node_hwmon_in_volts{chip="x120x",sensor="in1"}          (observer OCV)
 *   node_hwmon_curr_amps{chip="x120x",sensor="curr1"}
 *   node_hwmon_power_watt{chip="x120x",sensor="power1"}
 *   node_hwmon_energy_joules{chip="x120x",sensor="energy1"}
 * ---------------------------------------------------------------------- */

/**
 * x120x_hwmon_is_visible() - select which hwmon channels exist
 * @data: driver data (struct x120x_chip *) — used to gate in1 by soc_source
 * @type: hwmon sensor type
 * @attr: attribute within @type
 * @channel: channel index
 *
 * Return: 0444 for the supported read-only channels (in0 with its voltage
 * limits, in1 observer OCV under the voltage model only, curr1, power1,
 * energy1 and their labels), 0 to hide everything else.
 */
static umode_t x120x_hwmon_is_visible(const void *data,
				       enum hwmon_sensor_types type,
				       u32 attr, int channel)
{
	const struct x120x_chip *chip = data;

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
		case hwmon_in_label:
			/*
			 * in0 = terminal VCELL (always).  in1 = the observer's
			 * OCV(SoC) estimate, which only exists with the voltage
			 * model — hide it under soc_source=gauge.
			 */
			if (channel == 1 &&
			    chip->soc_src != X120X_SOC_SRC_VOLTAGE)
				return 0;
			return 0444;
		case hwmon_in_lcrit:
		case hwmon_in_min:
		case hwmon_in_lcrit_alarm:
			/* voltage safety limits belong to in0 only */
			return channel == 0 ? 0444 : 0;
		default:
			break;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
		case hwmon_curr_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_power:
		switch (attr) {
		case hwmon_power_input:
		case hwmon_power_label:
			return 0444;
		default:
			break;
		}
		break;
	case hwmon_energy:
		switch (attr) {
		case hwmon_energy_input:
		case hwmon_energy_label:
			return 0444;
		default:
			break;
		}
		break;
	default:
		break;
	}
	return 0;
}

/**
 * x120x_hwmon_read() - hwmon numeric read callback
 * @dev: hwmon device
 * @type: hwmon sensor type
 * @attr: attribute within @type
 * @channel: channel index
 * @val: result, in hwmon canonical units
 *
 * Serves values from the cached driver state (no I2C traffic), with
 * the unit conversions documented in the section comment above.
 *
 * Return: 0 on success, -EOPNOTSUPP for unsupported channels.
 */
static int x120x_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct x120x_chip *chip = dev_get_drvdata(dev);
	int voltage_uv, energy_rate_uw, ocv_model_uv;
	bool vfloor_critical;
	s64 energy_now_uwh;

	mutex_lock(&chip->lock);
	voltage_uv      = chip->voltage_uv;
	ocv_model_uv    = chip->ocv_model_uv;
	vfloor_critical = chip->vfloor_critical;
	energy_rate_uw  = chip->power_report_uw;	/* power for the ABI */
	energy_now_uwh  = chip->energy_now_uwh;
	mutex_unlock(&chip->lock);

	switch (type) {
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			/* in0: terminal VCELL; in1: observer OCV(SoC), mV */
			*val = (channel == 1 ? ocv_model_uv : voltage_uv) / 1000;
			return 0;
		case hwmon_in_lcrit:
			/* on-battery critical floor (SoC-independent backstop) */
			*val = vmin_critical_mv;	/* mV; default 3100 */
			return 0;
		case hwmon_in_min:
			/* design-minimum / empty (0 % SoC) operating voltage */
			*val = X120X_VOLTAGE_MIN_DESIGN_UV / 1000;	/* 3.20 V */
			return 0;
		case hwmon_in_lcrit_alarm:
			/* 1 once the floor has been held long enough to force
			 * CAPACITY_LEVEL=CRITICAL and drive OS shutdown */
			*val = vfloor_critical;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_curr:
		/*
		 * curr1_input: derived current in mA.
		 * I (mA) = P (µW) / V (µV) × 1000
		 * Sign: positive = charging, negative = discharging.
		 * Guard against division by zero on an unread or dead battery.
		 */
		if (attr != hwmon_curr_input)
			return -EOPNOTSUPP;
		if (voltage_uv <= 0) {
			*val = 0;
		} else {
			*val = (long)div_s64(
				(s64)energy_rate_uw * 1000, voltage_uv);
		}
		return 0;

	case hwmon_power:
		/*
		 * power1_input: derived power in µW.
		 * Sign: positive = charging, negative = discharging.
		 */
		if (attr != hwmon_power_input)
			return -EOPNOTSUPP;
		*val = energy_rate_uw;
		return 0;

	case hwmon_energy:
		/*
		 * energy1_input: current stored energy in µJ.
		 * µJ = µWh × 3600.  Represents energy stored relative to
		 * empty; useful for graphing state-of-energy over time.
		 */
		if (attr != hwmon_energy_input)
			return -EOPNOTSUPP;
		*val = (long)(energy_now_uwh * 3600);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

/**
 * x120x_hwmon_read_string() - hwmon label read callback
 * @dev: hwmon device (unused)
 * @type: hwmon sensor type
 * @attr: attribute within @type
 * @channel: channel index
 * @str: receives a pointer to the constant label string
 *
 * Return: 0 on success, -EOPNOTSUPP for channels without a label.
 */
static int x120x_hwmon_read_string(struct device *dev,
				    enum hwmon_sensor_types type,
				    u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_in:
		if (attr == hwmon_in_label) {
			*str = channel == 1 ? "cell_ocv" : "cell_voltage";
			return 0;
		}
		break;
	case hwmon_curr:
		if (attr == hwmon_curr_label) {
			*str = "battery_current";
			return 0;
		}
		break;
	case hwmon_power:
		if (attr == hwmon_power_label) {
			*str = "battery_power";
			return 0;
		}
		break;
	case hwmon_energy:
		if (attr == hwmon_energy_label) {
			*str = "battery_energy";
			return 0;
		}
		break;
	default:
		break;
	}
	return -EOPNOTSUPP;
}

static const struct hwmon_ops x120x_hwmon_ops = {
	.is_visible  = x120x_hwmon_is_visible,
	.read        = x120x_hwmon_read,
	.read_string = x120x_hwmon_read_string,
};

static const struct hwmon_channel_info * const x120x_hwmon_info[] = {
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_LCRIT |
			   HWMON_I_MIN | HWMON_I_LCRIT_ALARM,
			   HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_LABEL),
	HWMON_CHANNEL_INFO(energy,
			   HWMON_E_INPUT | HWMON_E_LABEL),
	NULL,
};

static const struct hwmon_chip_info x120x_hwmon_chip_info = {
	.ops  = &x120x_hwmon_ops,
	.info = x120x_hwmon_info,
};

/* -------------------------------------------------------------------------
 * PM ops
 * ---------------------------------------------------------------------- */

/**
 * x120x_suspend() - PM callback: stop polling before suspend
 * @dev: the I2C client device
 *
 * Cancels the poll work synchronously so no I2C transfer is in
 * flight when the controller suspends.
 *
 * Return: 0 always.
 */
static int x120x_suspend(struct device *dev)
{
	struct x120x_chip *chip = i2c_get_clientdata(to_i2c_client(dev));

	cancel_delayed_work_sync(&chip->work);
	return 0;
}

/**
 * x120x_resume() - PM callback: restart polling after resume
 * @dev: the I2C client device
 *
 * Return: 0 always.
 */
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

/*
 * Set true by probe() on success; consulted by x120x_init() to decide
 * whether to skip the manual i2c_client fallback path when DT has
 * already bound the driver.  See the comment block at x120x_init().
 */
static bool x120x_probe_bound;

/**
 * x120x_probe() - bind the driver to the fuel gauge
 * @client: I2C client, from device tree or the x120x_init() fallback
 *
 * Validates module parameters, verifies a MAX1704x responds,
 * configures board-variant behaviour (power-off GPIO, charge-control
 * availability), acquires the GPIOs, registers the three
 * power_supply devices and the hwmon device, and starts the poll
 * loop.  Every resource is devm-managed.
 *
 * Return: 0 on success, negative errno on failure.
 */
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

	/*
	 * Initialize the poll work before registering any power_supply.
	 * The battery's external_power_changed callback calls
	 * mod_delayed_work() on chip->work, and the power-supply core can
	 * deliver a deferred "changed" event from the earlier AC/charger
	 * registrations as soon as the battery registers — before the old
	 * init site (down by "Start polling") would have run.  The work
	 * item must exist before any supply is registered.
	 */
	INIT_DELAYED_WORK(&chip->work, x120x_poll_work);

	i2c_set_clientdata(client, chip);

	/*
	 * Clamp battery_mah to a sane range.  ENERGY_FULL_DESIGN is
	 * battery_mah * 3600 (nominal mV) and is cast to int; capping at
	 * 500000 mAh keeps that product (~1.8e9) below INT_MAX, so a bogus
	 * module-param value cannot overflow it.
	 */
	if (battery_mah < 1 || battery_mah > 500000) {
		dev_warn(dev, "battery_mah=%d out of range [1, 500000]; clamping\n",
			 battery_mah);
		battery_mah = clamp(battery_mah, 1, 500000);
	}

	/*
	 * Clamp the undervoltage floor: comfortably above boost-UVLO and
	 * below a full cell, so neither a typo nor a hostile modprobe.d
	 * value can disable protection (too low) or power the box off at
	 * rest (too high).
	 */
	if (vmin_critical_mv < 2500 || vmin_critical_mv > 4100) {
		dev_warn(dev, "vmin_critical_mv=%d out of range [2500, 4100]; clamping\n",
			 vmin_critical_mv);
		vmin_critical_mv = clamp(vmin_critical_mv, 2500, 4100);
	}

	/*
	 * Validate the conservation band against the same rules the sysfs
	 * store paths enforce (start 0-99, end 1-100, start < end).  The
	 * module parameters arrive unvalidated from /etc/modprobe.d, and a
	 * bad band (e.g. conservation_end=200) would otherwise load
	 * silently and never trigger the stop threshold.
	 */
	if (conservation_start < 0 || conservation_start > 99 ||
	    conservation_end < 1 || conservation_end > 100 ||
	    conservation_start >= conservation_end) {
		dev_warn(dev,
			 "invalid conservation band start=%d end=%d (need start 0-99, end 1-100, start < end); using defaults 78/80\n",
			 conservation_start, conservation_end);
		conservation_start = 78;
		conservation_end = 80;
	}

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

	/* -- SoC source: voltage OCV model (default) or raw gauge --------- */
	if (soc_source && !strcmp(soc_source, "gauge")) {
		chip->soc_src = X120X_SOC_SRC_GAUGE;
	} else {
		chip->soc_src = X120X_SOC_SRC_VOLTAGE;
		if (soc_source && strcmp(soc_source, "voltage"))
			dev_warn(dev,
				 "unknown soc_source \"%s\", using \"voltage\"\n",
				 soc_source);
	}
	dev_info(dev, "SoC source: %s\n",
		 chip->soc_src == X120X_SOC_SRC_GAUGE ?
		 "gauge (raw MAX17043 register)" :
		 "voltage (recursive OCV observer: I=(OCV(SoC)-V)/R, "
		 "energy-integrating, self-anchoring; power = I*V)");

	/*
	 * Resolve the observer's pack resistance: 0 uses the built-in
	 * default; a value in [10, 100] mohm overrides it (the installer
	 * derives it from the populated cell count); anything else warns and
	 * falls back.  Only the transient power/rate depends on it.
	 */
	if (pack_resistance_mohm == 0) {
		chip->r_uohm = X120X_R_UOHM;
	} else if (pack_resistance_mohm >= 10 && pack_resistance_mohm <= 100) {
		chip->r_uohm = pack_resistance_mohm * 1000;
	} else {
		dev_warn(dev,
			 "pack_resistance_mohm=%d out of range [10, 100]; using built-in %d mohm\n",
			 pack_resistance_mohm, X120X_R_UOHM / 1000);
		chip->r_uohm = X120X_R_UOHM;
	}
	/*
	 * Asymmetric R: an explicit pack_resistance_mohm param pins both
	 * directions to that single value; otherwise use the direction-
	 * dependent IR-step/coulomb-measured pair (charge lower than discharge).
	 */
	if (pack_resistance_mohm) {
		chip->r_chg_uohm = chip->r_dis_uohm = chip->r_uohm;
	} else {
		chip->r_chg_uohm = X120X_R_CHG_UOHM;
		chip->r_dis_uohm = X120X_R_DIS_UOHM;
	}
	dev_info(dev, "pack resistance: %d/%d mohm chg/dis (%s)\n",
		 chip->r_chg_uohm / 1000, chip->r_dis_uohm / 1000,
		 pack_resistance_mohm ? "module param" : "built-in asymmetric");

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
	/* ── Board variant setup ------------------------------------------ */
	/*
	 * Configure board-specific behaviour: power-off GPIO, charge control
	 * availability, and experimental warning.
	 */
	{
		int poweroff_gpio = -1;
		bool is_x120x = !board || !strcmp(board, "x120x");
		bool is_x708  = !strcmp(board, "x708");

		if (!is_x120x) {
			dev_warn(dev,
				 "EXPERIMENTAL: board=%s support is untested.\n"
				 "Validate correct operation before relying on "
				 "this driver for any purpose.\n", board);
		}

		if (!strcmp(board, "x728v2") || !strcmp(board, "x729"))
			poweroff_gpio = X728V2_GPIO_POWEROFF;
		else if (!strcmp(board, "x728v1") || is_x708)
			poweroff_gpio = X728V1_GPIO_POWEROFF;
		else if (!is_x120x)
			dev_warn(dev, "unknown board variant \"%s\" — "
				 "treating as x120x\n", board);

		/*
		 * X708 GPIO16 is fan speed, not charge control.
		 * X728 V1.x and X729 have no charge control GPIO.
		 * Only x120x and x728v2 (V2.5) have charge control.
		 */
		chip->has_charge_ctrl = is_x120x ||
					!strcmp(board, "x728v2");

		if (poweroff_gpio >= 0) {
			chip->gpio_poweroff = devm_gpiod_get_index_optional(
				dev, "power-off", 0, GPIOD_OUT_LOW);
			if (IS_ERR(chip->gpio_poweroff)) {
				ret = PTR_ERR(chip->gpio_poweroff);
				dev_err(dev, "failed to get power-off GPIO: %d\n",
					ret);
				return ret;
			}
			if (chip->gpio_poweroff) {
				/*
				 * Register at default priority — we have
				 * nothing platform-specific to defer to and
				 * no reason to outrank other handlers.
				 * PREPARE mode allows sleeping (we use msleep
				 * for the 3-second pulse).  devm cleanup
				 * tears this down automatically on unbind.
				 */
				ret = devm_register_sys_off_handler(
					dev,
					SYS_OFF_MODE_POWER_OFF_PREPARE,
					SYS_OFF_PRIO_DEFAULT,
					x120x_do_poweroff,
					chip);
				if (ret) {
					dev_err(dev,
						"failed to register power-off handler: %d\n",
						ret);
					return ret;
				}
				dev_info(dev, "power-off handler registered\n");
			} else {
				dev_warn(dev,
					 "power-off GPIO not found — UPS may not "
					 "cut power after shutdown\n"
					 "Install the board device tree overlay\n");
			}
		}
	}

	/* Restore charge mode from module parameter (survives reboot) */
	if (chip->has_charge_ctrl)
		chip->conservation_mode = !!conservation_mode_default;

	/*
	 * Charge control GPIO — skipped on X708 and boards without it.
	 * Initialise LOW (charging enabled) unconditionally so the battery
	 * starts charging immediately after a deep-discharge boot, even if
	 * conservation_mode_default is set.  The poll loop will apply the
	 * correct hysteresis on its first tick.
	 */
	chip->gpio_chrg = chip->has_charge_ctrl
		? devm_gpiod_get_optional(dev, "charge-ctrl", GPIOD_OUT_LOW)
		: NULL;
	if (IS_ERR(chip->gpio_chrg)) {
		ret = PTR_ERR(chip->gpio_chrg);
		dev_err(dev, "failed to get charge-ctrl GPIO: %d\n", ret);
		return ret;
	}
	if (chip->has_charge_ctrl && !chip->gpio_chrg) {
		dev_warn(dev,
			 "charge-ctrl GPIO not found - charge_type will be read-only\n"
			 "Install the device tree overlay: dtoverlay=x120x\n");
		/*
		 * Demote to a board without charge control: the write paths
		 * gate on has_charge_ctrl, so leaving it set would let
		 * charge_type accept a Long Life write that the poll loop
		 * (which gates on the descriptor) can never enforce.
		 */
		chip->has_charge_ctrl = false;
		chip->conservation_mode = false;
	}
	/*
	 * Explicitly force the charger on at probe.  GPIOD_OUT_LOW above
	 * sets the initial state, but if the GPIO was previously latched
	 * high (charger inhibited) by a prior driver instance, the hardware
	 * pin may not reflect the new software state until we write it.
	 * Always drive it low here so charging begins immediately.
	 */
	if (chip->gpio_chrg)
		x120x_gpio_set(chip->gpio_chrg, 0);

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
		/*
		 * soc_pct is derived from an unsigned 16-bit register (>> 8),
		 * so it is always >= 0: the MIN_PLAUSIBLE (0) side never fires
		 * and is kept only for symmetry; the MAX_PLAUSIBLE (100) side
		 * is the live check.
		 */
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
	bat_cfg.attr_grp = x120x_battery_extra_groups;

	chip->battery = devm_power_supply_register(dev, &x120x_battery_desc,
						   &bat_cfg);
	if (IS_ERR(chip->battery)) {
		ret = PTR_ERR(chip->battery);
		dev_err(dev, "failed to register battery supply: %d\n", ret);
		return ret;
	}

	/* -- Start polling ------------------------------------------------ */
	/* chip->work was initialized up front, before any supply register. */
	schedule_delayed_work(&chip->work, 0);

	/* -- Register hwmon device ---------------------------------------- */
	chip->hwmon_dev = devm_hwmon_device_register_with_info(
		dev, "x120x", chip,
		&x120x_hwmon_chip_info, NULL);
	if (IS_ERR(chip->hwmon_dev)) {
		ret = PTR_ERR(chip->hwmon_dev);
		dev_warn(dev, "hwmon registration failed: %d\n", ret);
		chip->hwmon_dev = NULL;
		/* Non-fatal: power_supply interface is the primary ABI */
	}

	dev_info(dev,
		 "x120x UPS ready (battery=%s ac=%s charger=%s hwmon=%s)\n",
		 x120x_battery_desc.name,
		 x120x_ac_desc.name,
		 x120x_charger_desc.name,
		 chip->hwmon_dev ? dev_name(chip->hwmon_dev) : "disabled");

	/*
	 * Flag that some probe() invocation succeeded.  Read by
	 * x120x_init() to decide whether to skip the manual i2c_client
	 * fallback when DT has already bound us.  Once set it stays set
	 * for the lifetime of the module — that's fine, it's only
	 * consulted once during init.
	 */
	x120x_probe_bound = true;

	return 0;
}

/**
 * x120x_remove() - unbind: stop the poll loop
 * @client: I2C client being unbound
 *
 * Only the poll work needs explicit teardown; every other resource
 * is devm-managed and released by the core after this returns.
 */
static void x120x_remove(struct i2c_client *client)
{
	struct x120x_chip *chip = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&chip->work);

	/*
	 * The sys-off handler registered in probe is owned by devm and
	 * torn down automatically when the device unbinds.  Nothing to
	 * unwind manually here.
	 */
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
 *
 * When the DT overlay IS present, the i2c subsystem instantiates an
 * i2c_client at 0x36 from the DT node before our module loads, and
 * i2c_add_driver() synchronously binds that client and runs probe()
 * before it returns.  In that case the manual probe loop below should
 * be skipped entirely — otherwise the loop's first i2c_new_client_device
 * collides with the DT client (EBUSY, logged by the i2c subsystem
 * itself), then falls through to the next address and registers a
 * phantom client where no chip exists (EREMOTEIO from probe).  Neither
 * failure breaks the (DT-bound) driver but both leak scary lines into
 * the boot log.
 *
 * Detection: x120x_probe_bound is set to true by probe() on success.
 * After i2c_add_driver() returns, if the flag is set we know a DT
 * binding already happened and there is nothing for the manual
 * fallback to do.
 * ---------------------------------------------------------------------- */

static struct i2c_client *x120x_i2c_client;

/**
 * x120x_init() - module init: register the driver, with manual fallback
 *
 * Registers the I2C driver; when device tree has not bound it (no
 * overlay loaded), probes the i2c_addrs candidate addresses on bus
 * i2c_bus and instantiates the client manually.  See the section
 * comment above for the detection logic.
 *
 * Return: 0 on success — including when no gauge is found, so the
 * module stays loaded and logs why — or negative errno if driver
 * registration fails.
 */
static int __init x120x_init(void)
{
	struct i2c_adapter *adapter;
	struct i2c_board_info info = {};
	struct i2c_client *client;
	int i, ret;

	ret = i2c_add_driver(&x120x_driver);
	if (ret)
		return ret;

	/*
	 * If probe() ran successfully against a DT-instantiated client
	 * during i2c_add_driver(), the driver is already bound and we
	 * skip the manual fallback path.
	 */
	if (x120x_probe_bound)
		return 0;

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

/**
 * x120x_exit() - module exit: undo the manual client and the driver
 *
 * Unregisters the manually-instantiated client, if the init fallback
 * created one, before deleting the driver.
 */
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
MODULE_DESCRIPTION("SupTronics UPS HAT power supply driver (X120x, X728, X708, X729)");
MODULE_VERSION("0.5.23");
/*
 * "GPL" is the canonical MODULE_LICENSE string for GPL-compatible
 * modules; the precise license (GPL-2.0-or-later) is expressed by the
 * SPDX header at the top of this file, which is authoritative.
 */
MODULE_LICENSE("GPL");

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
 *
 * This project is an independent personal contribution, developed in
 * the author's own time on their own hardware.  It is not affiliated
 * with or endorsed by SupTronics, Geekworm, or the author's employer.
 */
