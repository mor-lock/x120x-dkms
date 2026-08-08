# x120x-dkms — SupTronics UPS HAT kernel driver

[![CI](https://github.com/mor-lock/x120x-dkms/actions/workflows/ci.yml/badge.svg)](https://github.com/mor-lock/x120x-dkms/actions/workflows/ci.yml)
[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](LICENSE)

A DKMS kernel driver for SupTronics UPS HAT boards on Raspberry Pi,
distributed by Geekworm.  Covers the X120x series and experimentally
the X728, X729, and X708.

This driver is an independent community project — developed on my own
time and hardware, not affiliated with or endorsed by SupTronics or
Geekworm — though Geekworm links to it from their official wiki pages
for these boards (e.g. the
[X1206 page](https://wiki.geekworm.com/X1206)).

Provides native Linux power supply integration equivalent to a laptop
battery — battery icon in the taskbar, accurate state of charge (SoC),
clean undervoltage shutdown, and selectable Long Life battery
preservation mode.  No custom scripts, no daemons, no polling
loops.

![The X120x pack as a battery icon in the Raspberry Pi OS panel tray, green and on mains — indistinguishable from a laptop battery.](docs/images/battery-tray.png)

## Getting started

If you just want to get up and running quickly, here is everything you
need in one place.

**Requirements:** Raspberry Pi OS Bookworm or later (64-bit
recommended), fully updated — run `sudo apt update && sudo apt
full-upgrade` first.  You need kernel 6.3 or newer; check with `uname
-r` (a fully-updated Bookworm is on 6.6 or 6.12).  The driver builds via
DKMS against your running kernel, so there are no pre-built binaries to
match.  (For maintainers: the 6.3 floor comes from the driver's use of
the modern one-arg i2c `.probe`, the sys-off handler framework, and the
`void` i2c `.remove`.)

Hardware assembly — battery orientation, seating the HAT on the Pi —
is covered by your board's page on the
[Geekworm wiki](https://wiki.geekworm.com/); this guide starts where
assembly ends.

### 1. Install the driver

Clone the repository:

```bash
git clone https://github.com/mor-lock/x120x-dkms.git
cd x120x-dkms
```

(No git?  Download and extract the
[latest release archive](https://github.com/mor-lock/x120x-dkms/releases/latest)
instead, and run the same commands from the extracted directory.)

Now run the install command for your board — it sets the battery pack
capacity for you.  Copy-paste the one that matches:

| Board | Cells | Install command |
|---|---|---|
| X1200 | 2× 18650 | `sudo bash install.sh --battery-mah 6000` |
| X1201 | 2× 18650 | `sudo bash install.sh --battery-mah 6000` |
| X1202 | 4× 18650 | `sudo bash install.sh --battery-mah 12000` |
| X1203 | External Li-ion | `sudo bash install.sh --battery-mah <your_capacity>` |
| X1205 | 2× 21700 | `sudo bash install.sh --battery-mah 10000` |
| X1206 | 4× 21700 | `sudo bash install.sh --battery-mah 20000` |
| X1207 | 1× 21700 (PoE) | `sudo bash install.sh --battery-mah 5000` |
| X1208 | 1× 21700 + NVMe | `sudo bash install.sh --battery-mah 5000` |
| X1209 | External Li-ion | `sudo bash install.sh --battery-mah <your_capacity>` |
| X708 *(experimental)* | External Li-ion | not yet installable — see [Experimental board support](#experimental-board-support) |
| X728 V1.x *(experimental)* | 2× 18650 | not yet installable — see [Experimental board support](#experimental-board-support) |
| X728 V2.x *(experimental)* | 2× 18650 | not yet installable — see [Experimental board support](#experimental-board-support) |
| X729 *(experimental)* | 2× 18650 | not yet installable — see [Experimental board support](#experimental-board-support) |

The table assumes 3000 mAh 18650 cells and 5000 mAh 21700 cells — check
the mAh printed on your actual cells and multiply by the cell count if
yours differ.  For the external-pack boards (X1203, X1209, and the
experimental X708) replace `<your_capacity>` with your pack's total
capacity in mAh.  The X728/X708/X729 rows are **experimental and
untested** — see [Experimental board support](#experimental-board-support) before relying on them.
`Fast` is the default, so it is omitted above; to start in Long Life
from the outset, add `--charge-mode longlife` (see *Battery conservation
mode*).

The driver defaults to **Fast** mode — it charges to 100%, which is
right for almost every UPS install.  A battery-preserving **Long Life**
mode can be enabled at any time after install — see *Battery
conservation mode*.

**State of charge** defaults to a voltage-based model (`--soc-source
voltage`): SoC comes from an energy-true NMC open-circuit-voltage curve
with runtime IR (load-sag) compensation, then *fused* with the on-board
MAX17043-*style* fuel gauge for smoothness.  The curve is calibrated so
SoC is linear in usable energy; the gauge — which craters near empty but
rejects load transients well — lends its smooth *shape* at high SoC,
while the calibrated voltage curve takes over at low SoC where the gauge
is unreliable.  The gauge's own raw reading is exposed separately as
`raw_capacity`.  See **[docs/soc-model.md](docs/soc-model.md)** for the
full algorithm.
Pass `--soc-source gauge` to use the raw fuel-gauge register instead.
The curves assume 4.2 V Li-ion (NMC/NCA) cells, which is all the
hardware can charge.

Before rebooting, make sure the power supply is connected to the
**UPS board's own power input**, not the Pi's USB-C port.  The Pi is
powered through the UPS; a charger plugged into the Pi directly will
neither charge the battery nor assert AC detection.  Your board's
page on the [Geekworm wiki](https://wiki.geekworm.com/) shows the
input's location.

Then reboot:

```bash
sudo reboot
```

On a Raspberry Pi 5 the installer also stages two required bootloader
settings — `POWER_OFF_ON_HALT=1` and `PSU_MAX_CURRENT=5000` — which take
effect at that same reboot; see *Required bootloader settings (Raspberry
Pi 5)* below for what they do and the `--skip-eeprom` opt-out.

### 2. Monitor battery state

After the reboot, the battery shows up as a standard Linux power
supply.  For a quick command-line view (works on every install,
including Lite/headless):

```bash
upower -i /org/freedesktop/UPower/devices/battery_x120x_battery
```

![upower -i output for the x120x battery: vendor SupTronics, model X120x, state fully-charged, 73.55 Wh of 74 Wh, 4.18 V, 100% capacity.](docs/images/upower-info.png)

On desktop installs a battery icon also appears in the taskbar, and
`gnome-power-statistics` shows live battery percentage, voltage,
energy, charge rate, and history graphs — all read directly from the
driver via UPower.  No configuration needed:

```bash
sudo apt install gnome-power-manager
gnome-power-statistics
```

![GNOME Power Statistics with the x120x battery selected — Vendor SupTronics, Model X120x, State Charged, Energy 73.6 of 74.0 Wh.](docs/images/power-statistics.png)

That is all that is needed for a fully working installation.  The
rest of this document covers the driver interface, hardware details,
and advanced configuration in depth.

No icon after rebooting?  See [Troubleshooting](#troubleshooting) below.

---

## Troubleshooting

If something is not working after installing and rebooting, find your
symptom below.  Every command is safe to copy-paste.

### No battery icon after reboot

First check that the driver loaded:

```bash
dmesg | grep x120x
```

Healthy output looks like:

```
x120x: loading out-of-tree module taints kernel.
x120x 1-0036: MAX1704x at 0x36 version 0x000
x120x 1-0036: x120x UPS ready (battery=x120x-battery ac=x120x-ac charger=x120x-charger hwmon=hwmon3)
```

- **Nothing at all** — the device-tree overlay is not loading.  Check
  that `dtoverlay=x120x` is present in `/boot/firmware/config.txt`
  (under the `[all]` section), and that the reboot actually happened —
  reboot again if unsure.
  - **On Ubuntu, if the driver worked and then vanished after an
    `apt upgrade`:** the update repopulated `/boot/firmware/current/overlays/`
    and deleted the overlay file (`ls` there shows no `x120x.dtbo`) even
    though `dtoverlay=x120x` is still in `config.txt`.  The installer sets
    up an apt hook that restores it automatically after each update; if you
    installed before that hook existed, re-run `sudo bash install.sh` once
    to install both the overlay and the hook, then reboot.
- **Probe or I²C errors** (e.g. `MAX1704x` not found) — the board is
  not making contact.  Power down, re-seat the Pi firmly on the UPS
  board's pogo pins, and confirm you passed the right `--board`.

### Devices exist but no icon in the taskbar

Check whether the power-supply devices are present:

```bash
ls /sys/class/power_supply/
```

If you see `x120x-battery`, `x120x-ac`, and `x120x-charger`, the driver
is fine — this is a desktop/UPower display issue.  Confirm UPower sees
the battery:

```bash
upower -e
```

Then log out and back in; some desktop panels also need the battery
indicator enabled in their panel/applet settings.

### capacity reads 0% or nonsense on the first boot

With the default `--soc-source voltage` model this is largely moot — SoC
comes from cell voltage and is meaningful immediately.  With
`--soc-source gauge` the MAX17043 clone needs a little time to converge
after first power-up — give it a few minutes.  If it stays at 0% with
the charger connected, the cells may have been deep-discharged; see
*Dead battery detection* and the deep-discharge recovery notes.

### ac_online is 0 with the charger plugged in

First check the cheapest thing: the charger must be plugged into the
UPS board's own power input, not the Pi's USB-C port.  A supply
feeding the Pi directly keeps the Pi running but never charges the
battery or asserts AC detection.

Otherwise this is almost always the GPIO6 AC-detect line floating at boot — see
[GPIO6 pull-up](#gpio6-pull-up).  If the charger LED is lit and `ac_online` stays `0`
across reboots (with `gpio=6=pu` in `config.txt`), suspect a failed
board — see [Incident 2](docs/incidents.md#incident-2--grid-return-undetected-recovery-livelock-2026-03-29) for the field-failure signature.

### Build failed / DKMS errors

Almost always missing kernel headers.  Install the ones matching your
running kernel and reinstall:

```bash
sudo apt install linux-headers-$(uname -r)
```

See [Step 1 of the manual installation](docs/manual-install.md#step-1--install-dependencies) for details.

### Opening a GitHub issue

If none of the above helps, open an issue and include the output of:

```bash
dmesg | grep x120x
dkms status
cat /proc/device-tree/model   # Pi model
cat /etc/os-release           # OS version
```

plus which UPS board you have.

Or just run `tools/collect-debug.sh` from a checkout (no root required) —
it gathers all of the above, plus the driver's sysfs state, into a single
paste-ready block.

---

## Supported hardware

All X120x models share an identical software interface and are fully
supported by this driver:

| Model  | Pi compatibility         | Connection              | Battery            |
|--------|--------------------------|-------------------------|--------------------|
| X1200  | Raspberry Pi 5           | Pogo pins               | 2× 18650           |
| X1201  | Raspberry Pi 5           | Pogo pins               | 2× 18650 (thin)    |
| X1202  | Raspberry Pi 5           | Pogo pins               | 4× 18650           |
| X1203  | Raspberry Pi 5           | Pogo pins               | External Li-ion    |
| X1205  | Raspberry Pi 5           | Pogo pins               | 2× 21700           |
| X1206  | Raspberry Pi 5           | Pogo pins               | 4× 21700           |
| X1207  | Raspberry Pi 5           | 40-pin header + pogo¹   | 1× 21700 (PoE)     |
| X1208  | Raspberry Pi 5           | 40-pin header + pogo¹   | 1× 21700 + NVMe    |
| X1209  | Raspberry Pi 5/4B/3B+/3B | 40-pin header + pogo²   | External Li-ion    |

¹ Connects via the 40-pin GPIO header.  A single additional pogo pin
  carries the power button signal to the Pi 5's PSW through-hole.

² Connects via the 40-pin GPIO header.  An optional pogo pin enables
  the power button function on Pi 5; not required on Pi 4/3.

### Tested hardware

Configurations confirmed working on real hardware.  To add a row, file a
[hardware test report](../../issues/new?template=hardware_report.yml) —
reports for other boards, Pi models, kernels, and 32-bit armhf are very
welcome.

| Board | Pi            | OS               | Driver | Kernel | Arch    | Reporter   |
|-------|---------------|------------------|--------|--------|---------|------------|
| X1206 | Raspberry Pi 5 | Raspberry Pi OS  | v0.4.8 | 6.12.x | aarch64 | maintainer |
| X1201 V1.1 | Raspberry Pi 5 | Ubuntu 26.04 LTS | v0.4.10 | — | aarch64 | [issue #5](https://github.com/mor-lock/x120x-dkms/issues/5) |
| X1209 (+ X1002 NVMe board) | Raspberry Pi 5 | Raspberry Pi OS | v0.4.3 | — | — | [issue #2](https://github.com/mor-lock/x120x-dkms/issues/2) |

### Experimental board support

The driver includes **untested, experimental** support for older Geekworm
UPS HAT boards that share the same MAX17043 fuel gauge and GPIO6 AC-detect
interface. These boards additionally require a GPIO pulse to cut power after
OS shutdown — without it the UPS stays on indefinitely after `poweroff`.

| Board | Pi support | Power-off GPIO | Charge control |
|-------|-----------|----------------|----------------|
| X728 V2.x | All Pi models | GPIO26 | GPIO16 (V2.5 only) |
| X728 V1.x | All Pi models | GPIO13 | None |
| X708 | Pi 4/3 only | GPIO13 | None (GPIO16 = fan speed) |
| X729 | All Pi models | GPIO26 | None |

**Not yet installable.** `install.sh` refuses `--board` variants other
than `x120x`: no per-board device tree overlay ships with this release,
and the `x120x` overlay has no `power-off-gpios` property — so the
power-off pulse these boards require after shutdown could never fire,
and the UPS would keep draining the pack indefinitely after `poweroff`
(the deep-discharge scenario this driver exists to prevent).  Refusing
is better than installing a setup that looks complete but silently
lacks its most important safety behaviour.  Per-board overlays are
future work and need hardware reports to validate.

If you have one of these boards and want to help develop support, the
manual path is:

1. Copy `x120x-overlay.dts` and add the board's power-off GPIO to the
   UPS node — `power-off-gpios = <&gpio 26 0>;` for X728 V2.x / X729,
   GPIO 13 for X728 V1.x / X708 (see the table above).
2. Compile and install the overlay
   (see [Manual installation](docs/manual-install.md),
   steps 5–7).
3. Run the normal installation (`sudo bash install.sh --battery-mah …`),
   add `board=x728v2` (or your variant) to the `options x120x` line in
   `/etc/modprobe.d/x120x.conf`, and reboot.

On success the driver logs `power-off handler registered` at probe;
please report results via the
[hardware test report](../../issues/new?template=hardware_report.yml)
template.

Board variants understood by the driver: `x120x` (default), `x728v2`,
`x728v1`, `x708`, `x729`.

**Important notes for experimental boards:**

- Long Life mode is only available on boards with charge control (X120x and
  X728 V2.5). On all other boards — and whenever the charge-control GPIO is
  absent from the device tree — a `Long Life` write is rejected,
  `charge_type` always reads `Fast`, and `charge_type` plus both
  `charge_control_*_threshold` files are read-only.
- The power-off GPIO pulse is registered via a sys-off handler
  (`SYS_OFF_MODE_POWER_OFF_PREPARE`) and fires after `systemctl
  poweroff`.  The DT overlay must provide the `power-off-gpios` property
  for this to work — without it a warning is logged and the UPS will not
  cut power automatically after shutdown.
- The DS1307 RTC on X728/X729 is handled by the existing mainline
  `rtc-ds1307` kernel driver, not this driver. Add `dtoverlay=i2c-rtc,ds1307`
  to `config.txt` to enable it.
- GPIO16 on the X708 controls **fan speed**, not charging. This driver never
  touches GPIO16 on X708.
- **None of these boards have been tested by the author.** Reports and
  feedback from users with this hardware are very welcome.

**Architecture note:** The driver has been developed and tested on
Raspberry Pi OS 64-bit (`aarch64`).  The X1209 also supports Pi 4B,
Pi 3B+, and Pi 3B, which can run 32-bit Raspberry Pi OS (`armhf`).
The driver contains no architecture-specific code and should build and
run correctly on `armhf` — the DKMS build system will compile for
whatever kernel is running — but this has not been tested.  Reports
from `armhf` users are welcome.

### Not supported by this driver

- **X703** — ultra-thin single-cell UPS for Pi 4 only.  Connects via
  test pins rather than the 40-pin header.  No I²C fuel gauge or GPIO
  interface accessible from the Pi.  Software shutdown not supported.
- **X735** — power management and PWM fan controller, not a UPS.  Has
  no battery fuel gauge and no I²C interface.  Nothing for this driver
  to interface with.
- **X-UPS1** — a universal stackable UPS with 12V/5V dual output and
  no Raspberry Pi GPIO integration.  No I²C fuel gauge interface.

## What it provides

After loading, three devices appear under `/sys/class/power_supply/`:

```
/sys/class/power_supply/x120x-battery/
    status                Charging | Discharging | Not charging | Full | Unknown
    health                Good | Dead | Unknown
    present               1 if battery detected
    manufacturer          SupTronics
    model_name            X120x (or X728, X708, X729 on experimental boards)
    voltage_now           cell voltage in µV
    voltage_max_design    4200000 µV (4.20 V — full charge)
    voltage_min_design    3200000 µV (3.20 V — safe shutdown floor)
    capacity              0-100 %  (fused voltage+gauge SoC)
    capacity_level        Critical (<5%) | Low (<10%) | Normal | Full (≥95%) | Unknown
    raw_capacity          0-100 %  (raw MAX17043 gauge SoC; non-standard, for diagnostics)
    charge_now            current charge in µAh
    charge_full           total pack capacity in µAh (from battery_mah)
    charge_full_design    same as charge_full
    charge_empty          0
    energy_now            current energy in µWh
    energy_full           total pack energy in µWh
    energy_full_design    same as energy_full
    energy_empty          0
    power_now             instantaneous power in µW (+ charging, − discharging)
    technology            Li-ion
    scope                 System

/sys/class/power_supply/x120x-ac/
    online          1 = mains present, 0 = on battery

/sys/class/power_supply/x120x-charger/
    online                          1 = mains present
    status                          Charging | Not charging | Discharging
    charge_type                     Fast | Long Life  (writeable)
    charge_control_start_threshold  SoC % to resume charging in Long Life mode (writeable, default 75)
    charge_control_end_threshold    SoC % to stop charging in Long Life mode (writeable, default 80)
```

A hwmon device is also registered under `/sys/class/hwmon/`:

```
/sys/class/hwmon/hwmonN/        (N assigned by kernel at load time)
    name              x120x
    in0_input         cell voltage in mV                        (read-only)
    in0_label         "cell_voltage"
    curr1_input       charge/discharge current in mA, signed    (read-only)
    curr1_label       "battery_current"
    power1_input      charge/discharge power in µW, signed      (read-only)
    power1_label      "battery_power"
    energy1_input     stored energy in µJ                       (read-only)
    energy1_label     "battery_energy"
```

Sign convention for `curr1_input` and `power1_input`: positive = charging,
negative = discharging.

The hwmon interface makes the driver visible to standard monitoring tools
without any configuration:

```bash
# lm-sensors
sensors
sensors | grep -A6 x120x

# Direct sysfs read — find the hwmon index first
N=$(grep -rl x120x /sys/class/hwmon/*/name 2>/dev/null | grep -o 'hwmon[0-9]*' | head -1)
cat /sys/class/hwmon/$N/in0_input       # voltage, mV
cat /sys/class/hwmon/$N/curr1_input     # current, mA (+ charging, - discharging)
cat /sys/class/hwmon/$N/power1_input    # power, µW
cat /sys/class/hwmon/$N/energy1_input   # stored energy, µJ
```

Prometheus `node_exporter` with `--collector.hwmon` (enabled by default)
exposes these as:

```
node_hwmon_in_volts{chip="x120x",sensor="in0"}
node_hwmon_curr_amps{chip="x120x",sensor="curr1"}
node_hwmon_power_watt{chip="x120x",sensor="power1"}
node_hwmon_energy_joules{chip="x120x",sensor="energy1"}
```

**Notes on derived channels:** `in0_input` (voltage) is a direct hardware
reading from the MAX17043 VCELL register.  The remaining three channels are
derived: `power1_input` is computed from the rate of change of SoC ×
pack capacity × nominal voltage; `curr1_input` is further derived as
power ÷ voltage; `energy1_input` is SoC% × pack energy capacity.  The
MAX17043 does not measure current directly.  Values are accurate during
steady charge/discharge but lag during rapid transitions and at very low
SoC before the fuel gauge model has converged.

### UPower integration

UPower reads these devices automatically:

```bash
upower -e
upower -i /org/freedesktop/UPower/devices/battery_x120x_battery
```

### Battery conservation mode

Lithium-ion cells wear out in two ways: **cycle aging** (charge and
discharge cycles) and **calendar aging** (time spent sitting at high
state of charge, especially near 100%).  A UPS battery sees very few
cycles — it charges once and then sits on mains for weeks or months
between outages — so calendar aging at full charge is the dominant wear
mechanism for always-on systems.  Conservation mode slows it by holding
the battery at a lower resting state of charge.  Note, though, that on a
standby UPS slower aging does **not** translate into more backup runtime
(see [Choosing a profile: runtime vs. longevity](docs/battery-profiles.md#choosing-a-profile-runtime-vs-longevity)) — which is why `Fast`
is the default, and conservation mode is aimed mainly at frequently
cycled builds.

The driver supports two charge modes, selectable via `charge_type`:

- **`Fast`** (default) — charges to 100%, disables the charger, and
  re-enables it once SoC falls to 95%.  This 100%/95% hysteresis band
  lets the pack drain down a little (the X1206 has a small standby draw
  on the battery rail) before topping up, instead of micro-cycling
  against the full-charge cutoff.  Cells rest at or near
  full voltage, so calendar aging continues at its normal rate.  Best
  when the priority is maximum backup capacity at the moment an outage
  begins.
- **`Long Life`** — charges to `charge_control_end_threshold` (default
  80%), disables the charger, and re-enables it at
  `charge_control_start_threshold` (default 75%).  Cells spend their
  idle life at a noticeably lower voltage, where calendar aging is
  dramatically reduced.  The trade-off is about 20% less runtime during
  an outage (~1.3 h on a full X1206); the benefit is that the cells
  retain meaningfully more of their original capacity after several
  years.  Best for a **frequently cycled build** (e.g. a portable
  unit), where cycle aging dominates and trimming the top of the charge
  greatly extends cell life — or, on a UPS, only when the pack is
  oversized relative to your worst outage or deferring the eventual
  replacement matters more than runtime.  See *Choosing a profile:
  runtime vs. longevity* below — on a standby UPS, slower aging does
  *not* automatically mean more runtime years later, because `Long Life`
  also starts every outage at a lower charge.

Enable and disable conservation mode from the command line:

```bash
# Enable conservation mode (charges to 80%, resumes at 75%)
echo "Long Life" | sudo tee /sys/class/power_supply/x120x-charger/charge_type

# Disable conservation mode (charges to 100%)
echo "Fast" | sudo tee /sys/class/power_supply/x120x-charger/charge_type

# Check current mode
cat /sys/class/power_supply/x120x-charger/charge_type

# Adjust thresholds (example: stop at 85%, resume at 70%)
echo 70 | sudo tee /sys/class/power_supply/x120x-charger/charge_control_start_threshold
echo 85 | sudo tee /sys/class/power_supply/x120x-charger/charge_control_end_threshold
```

> **Note:** `charge_control_start_threshold` and
> `charge_control_end_threshold` always report the **Long Life** band
> (default 75 / 80), regardless of the active mode — the standard sysfs
> interface has no way to express the Fast band.  In `Fast` mode those
> two values are inert: charging follows the fixed 100% / 95% band
> described above.  So seeing `75` / `80` there while in `Fast` is
> expected, not a misconfiguration.

The default thresholds (75% / 80%) match the recommendation of TLP, the
widely-used Linux power management tool, and are a commonly accepted
balance between battery longevity and available backup capacity.

The default thresholds can also be changed permanently via module
parameters in `/etc/modprobe.d/x120x.conf`:

```
options x120x battery_mah=20000 conservation_start=75 conservation_end=80
```

### Choosing a profile: runtime vs. longevity

The full discussion — calendar vs. cycle aging, why `Fast` wins for a
standby UPS and `Long Life` for a frequently cycled build, and the
measured standby sawtooth — has moved to
[docs/battery-profiles.md](docs/battery-profiles.md).

### Dead battery detection

#### How lithium-ion cells die

Lithium-ion cells have a safe operating voltage range of approximately
3.0–4.2 V per cell.  When a cell discharges below ~3.0 V the chemistry
becomes unstable: copper current collectors begin to dissolve into the
electrolyte and redeposit as dendrites on the anode.  This is
irreversible — the cell permanently loses capacity and internal
resistance rises sharply.  In severe cases the cell will no longer
accept charge at all.

This is the most common cause of the "battery charged to 100% but
powers off immediately when unplugged" reports on the Geekworm wiki.
The user ran the battery flat, plugged the charger back in, but the
cells had already been destroyed by deep discharge and cannot recover.

#### How the driver prevents this

The driver reports `capacity_level=Critical` when SoC drops below 5%
**and the system is on battery** (`ac_online=0`).  When mains power is
present, even at 0% SoC, `capacity_level` is never reported as Critical
— the battery is charging and shutting down would cause a livelock on
recovery from a deep discharge event.  UPower then fires
`warning-level: action` when SoC reaches its `PercentageAction`
threshold, which the installer sets to **2%** — well above the 3.20 V
floor.  This causes `systemd-logind` to initiate a clean OS shutdown.
The install script configures the whole chain automatically:
`HandleLowBattery=poweroff` via a drop-in under
`/etc/systemd/logind.conf.d/`, and `UsePercentageForPolicy=true`,
`PercentageAction=2` and `CriticalPowerAction=PowerOff` in
`UPower.conf`.

With the driver installed, the shutdown sequence on a prolonged outage
is:

```
grid power lost
    ↓
system runs on battery
    ↓
SoC drops to 5% → capacity_level=Critical → UPower warning-level: low
    ↓
SoC drops to 2% → UPower warning-level: action → logind: systemctl poweroff
    ↓
clean OS shutdown
    ↓
UPS cuts power to Pi — cells preserved well above 3.0 V
    ↓
grid restored → Pi boots automatically
```

Without the driver, there is no automatic shutdown.  The Pi runs until
the UPS hardware cuts power at its own low-voltage threshold, which may
be at or below the cell damage threshold.

#### Detection of already-destroyed cells

If cells have already been deep-discharged and destroyed, the driver
detects this automatically.  When the system is on grid power, the
cell voltage remains below 3.10 V for 10 minutes with no meaningful
voltage rise (less than 10 mV/h), and SoC is at or below 2%, the
battery health is reported as `Dead`:

```bash
cat /sys/class/power_supply/x120x-battery/health
# Dead
```

UPower surfaces this as `health: dead` and desktop environments will
display a battery warning.  A kernel log entry is also emitted:

```
x120x 1-0036: battery appears dead: 3050 mV on grid for 600 s with <10 mV/h rise
```

The health flag clears automatically if the condition resolves — for
example after replacing the cells.

### Charge mode persistence

The charge mode (`Fast` or `Long Life`) is persisted across reboots
automatically.  The installer installs a udev rule that fires whenever
`charge_type` is written and updates `conservation_mode_default` in
`/etc/modprobe.d/x120x.conf`.  On next boot the driver reads this
parameter and starts in the last-used mode.

The persistence files installed are:

- `/usr/local/lib/x120x-persist-mode.sh` — shell script called by udev
- `/etc/udev/rules.d/90-x120x-persist.rules` — udev rule

No action is required from the user — write `Long Life` once and it
will remain across reboots until explicitly changed back to `Fast`.

### GNOME and KDE

The conservation mode interface integrates natively with desktop
environments via UPower:

- **GNOME 48+** — "Preserve battery health" toggle in Settings → Power
- **KDE Plasma** — charge threshold controls in Power Management

When the toggle is enabled, UPower writes `Long Life` to `charge_type`
automatically.  The full chain — desktop toggle → UPower → sysfs →
driver → GPIO16 → hardware — works without any custom userspace code.

TLP and any other tool that writes to the standard
`charge_control_start_threshold` and `charge_control_end_threshold`
sysfs files will also work automatically.

### systemd-logind shutdown

On headless systems, `systemd-logind` initiates a clean shutdown when
UPower's `PercentageAction` threshold is reached, which the installer
sets to 2% SoC.
The driver reports `capacity_level=Critical` at 5% SoC, which triggers
UPower's low battery warning.  The actual shutdown fires at 2% when
UPower escalates to `warning-level: action`.

The install script enables this automatically by writing a drop-in
file, `/etc/systemd/logind.conf.d/90-x120x.conf`:

```ini
[Login]
HandleLowBattery=poweroff
```

A drop-in is used instead of editing `/etc/systemd/logind.conf` so the
packaged file stays pristine — dpkg never sees it as modified, so a
systemd upgrade never raises a conffile prompt over it — and every
line in the drop-in belongs to the driver.  (Systems installed by
older versions carry a marker-wrapped block in `logind.conf` instead;
reinstalling migrates it to the drop-in automatically.)

To disable the behaviour, override it from a later drop-in (e.g.
`/etc/systemd/logind.conf.d/99-local.conf`):

```ini
[Login]
HandleLowBattery=ignore
```

The installer also configures `/etc/UPower/UPower.conf`:

- `UsePercentageForPolicy=true` — act on battery percentage; a UPS HAT
  reports no time-to-empty estimate for UPower to use.
- `PercentageAction=2` — fire the PowerOff action at 2% SoC.  Debian/
  RPi-OS ship `PercentageAction=0`, which would only act at 0% — no
  margin above the 3.20 V floor; the installer overrides it to 2%.
- `CriticalPowerAction=PowerOff` — the default `HybridSleep` requires
  swap space and hangs indefinitely on a Raspberry Pi.
- `NoPollBatteries=true` — the driver sends UPower a notification on
  every meaningful state change and on a 30-second heartbeat.  UPower
  polling the kernel independently on its own timer causes race
  conditions that produce spurious `0%/unknown` entries in the history
  files and corrupt the gnome-power-statistics rate and charge graphs.
  Disabling polling eliminates these artefacts.

## Hardware interface

### X120x series (GPIO assignments)

All X120x boards share an identical GPIO interface:

| Signal       | GPIO  | Direction | Description                              |
|--------------|-------|-----------|------------------------------------------|
| I²C SDA      | GPIO2 | in/out    | MAX17043 fuel gauge data                 |
| I²C SCL      | GPIO3 | out       | MAX17043 fuel gauge clock                |
| AC present   | GPIO6 | input     | High = mains OK, low = on battery        |
| Charge ctrl  | GPIO16| output    | Low = charging enabled, high = disabled  |

### X728 / X729 / X708 (GPIO assignments, experimental)

These boards share GPIO2/3 (I²C) and GPIO6 (AC detect) with the X120x
series, but add a power-off GPIO and differ in charge control:

| Signal       | X728 V2.x / X729 | X728 V1.x / X708 | Description                         |
|--------------|------------------|------------------|-------------------------------------|
| I²C SDA/SCL  | GPIO2 / GPIO3    | GPIO2 / GPIO3    | MAX17043 fuel gauge                 |
| AC present   | GPIO6            | GPIO6            | High = mains OK, low = on battery   |
| Power-off    | GPIO26           | GPIO13           | Pulse high ~3 s to cut UPS power    |
| Charge ctrl  | GPIO16 (V2.5 only) | —              | Low = enabled, high = disabled      |
| Fan speed    | —                | GPIO16 (X708)    | High = fast, low = slow (not used by driver) |

The power-off GPIO must be pulsed by the driver after OS shutdown to
tell the UPS to cut power — without it the UPS stays on indefinitely.
On X120x boards this is handled by `POWER_OFF_ON_HALT=1` in the Pi 5
bootloader EEPROM instead.

### GPIO6 pull-up

The X120x boards drive GPIO6 high when mains power is present and
actively pull it low on power loss.  Without a software pull-up, GPIO6
can float low at boot before the UPS hardware has finished
initialising — causing the driver to falsely report `ac_online=0` even
when the charger is connected.  This is particularly likely when the
PSU is overloaded at boot (e.g. simultaneously charging the UPS battery
and powering other USB devices), which can cause the input voltage to
sag and delay or prevent GPIO6 assertion.

The installer adds `gpio=6=pu` to `config.txt` to apply a software
pull-up.  This ensures GPIO6 reads high by default until the hardware
actively drives it low, eliminating false AC-lost readings at boot.

If you installed the driver manually, add this line to
`/boot/firmware/config.txt` (or `/boot/config.txt` on older systems):

```
gpio=6=pu
```

### Deep discharge recovery

After a genuine deep discharge event the MAX17043 fuel gauge may report
0% SoC on the next boot.  The driver handles this correctly:

- 0% SoC is treated as a valid reading, not implausible — a quick-start
  command (which resets the fuel gauge's SoC estimation) is not issued,
  avoiding a reset at the worst possible moment.
- The charger (GPIO16) is forced on at probe and whenever SoC is at or
  below the resume threshold; between the resume and stop thresholds it
  holds its previous state.  The battery therefore starts charging
  immediately on every boot regardless of saved state.
- `capacity_level=Critical` is never reported when mains power is
  present, preventing UPower from triggering a shutdown loop while the
  battery is recovering.
- The `gpio=6=pu` pull-up ensures AC is detected correctly even if the
  PSU voltage sagged during the outage.

Without these fixes, a deep discharge followed by a power restoration
can result in a livelock: the Pi boots, UPower immediately fires a
critical battery shutdown, the Pi reboots, and the cycle repeats until
the battery is exhausted.

### MAX17043 register layout

**Note on register layout:** The MAX17043 registers on these boards are
mapped differently from the datasheet.  VCELL is at register `0x02`
and SOC is at `0x04`, as confirmed by SupTronics' published software.
This driver follows the observed hardware behaviour.

The fuel gauge default I²C address is `0x36`.  The driver probes
`0x36, 0x55, 0x32, 0x62` in order to cover all known board revisions.

## Required bootloader settings (Raspberry Pi 5)

On a Raspberry Pi 5, two bootloader EEPROM settings are required for the
driver's core behaviour.  `install.sh` configures them automatically —
it is idempotent and only stages a change when a value is missing or
wrong — so most users never touch this.  Pass `--skip-eeprom` to opt out
and manage them yourself.

- `POWER_OFF_ON_HALT=1` — the Pi fully depowers the SoC when Linux
  halts, so the UPS can cut and restore power to restart it cleanly when
  mains returns.  Without it the Pi stays partially powered after
  shutdown and the UPS cannot restart it.  Caveat: this also disables
  RTC-alarm and power-button wake from a halted state — intended for a
  UPS install, where the UPS performs the power cycling.
- `PSU_MAX_CURRENT=5000` — tells the Pi its supply can deliver 5 A,
  removing firmware current-limiting and suppressing spurious low-power
  warnings when drawing high current through the UPS board.  Caveat:
  this assumes a genuinely 5 A-capable supply.

`rpi-eeprom-config --apply` only *stages* the update on the boot
partition; the bootloader flashes it early during the next boot, so it
lands with the same reboot as the driver install — no separate reboot is
needed.

To configure them manually (or in a scripted setup), the non-interactive
one-liner keeps every other setting and sets just these two:

```bash
conf=$(mktemp)
sudo rpi-eeprom-config > "$conf"
sed -i -e '/^POWER_OFF_ON_HALT=/d' -e '/^PSU_MAX_CURRENT=/d' "$conf"
printf 'POWER_OFF_ON_HALT=1\nPSU_MAX_CURRENT=5000\n' >> "$conf"
sudo rpi-eeprom-config --apply "$conf"
rm -f "$conf"
```

Or edit interactively (prefix `EDITOR=nano` or `EDITOR=vim` if you like):

```bash
sudo rpi-eeprom-config -e
```

Either way, reboot afterwards for the bootloader to flash the update.

## Installation

### Quick install (recommended)

Clone the repository and run the install script:

```bash
git clone https://github.com/mor-lock/x120x-dkms.git
cd x120x-dkms
sudo bash install.sh
```

The script handles everything — including the Pi 5 bootloader settings —
and tells you what it is doing at each step.  Reboot when it finishes.

#### Install script options

Optional arguments configure the driver at install time:

| Option | Default | Description |
|---|---|---|
| `--battery-mah N` | `1000` | Total pack capacity in mAh. Multiply per-cell capacity by number of cells. |
| `--charge-mode MODE` | `fast` | Initial charge mode: `fast` or `longlife`. Persisted across reboots. See Getting started for guidance on which to choose. |
| `--board VARIANT` | `x120x` | Board variant. Only `x120x` is currently installable — other variants are refused until per-board overlays ship. See [Experimental board support](#experimental-board-support). |
| `--skip-eeprom` | _(off)_ | Do not modify Pi 5 bootloader EEPROM settings (`POWER_OFF_ON_HALT`, `PSU_MAX_CURRENT`); configure them manually — see Required bootloader settings. |

Examples:

```bash
# X1206 with four 5000 mAh 21700 cells
sudo bash install.sh --battery-mah 20000

# X1205 with two 5000 mAh 21700 cells
sudo bash install.sh --battery-mah 10000

# Portable build cycled most days — Long Life to extend cell lifespan
sudo bash install.sh --battery-mah 20000 --charge-mode longlife

# Show available options
sudo bash install.sh --help
```

If omitted the default (1000 mAh) is used and can be changed
later by editing `/etc/modprobe.d/x120x.conf` and rebooting.

---

### Updating

To update, fetch the new version and re-run the installer:

```bash
git pull   # or: download and extract the new release archive
sudo bash install.sh
sudo reboot
```

Settings from the previous install — pack capacity, charge mode, and
board variant — are kept automatically; flags are only needed to
change something.

---

### Uninstallation

To remove the driver and all changes made by the installer:

```bash
sudo bash uninstall.sh
sudo reboot
```

The uninstall script removes:

- The DKMS kernel module (all installed kernel versions)
- The DKMS source tree from `/usr/src/`
- The device tree overlay from `/boot/firmware/overlays/`
- The `dtoverlay=x120x` and `gpio=6=pu` lines from `config.txt`
- `/etc/modprobe.d/x120x.conf`
- The charge mode persistence script and udev rule
- The logind drop-in `/etc/systemd/logind.conf.d/90-x120x.conf` (and
  the `logind.conf.d` directory itself, if empty afterwards)
- The marker-wrapped block that the installer added to
  `/etc/UPower/UPower.conf` (delimited by
  `# >>> x120x-dkms: upower-pi-tweaks (do not edit) >>>` ...
  `# <<< x120x-dkms: upower-pi-tweaks <<<`)
- On systems installed before the drop-in existed: the marker-wrapped
  `logind-low-battery` block in `/etc/systemd/logind.conf`, and any
  bare lines left over from even older (pre-marker) installer versions

The following are intentionally left unchanged:

- The `dkms` and `linux-headers-$(uname -r)` packages — removing them
  could break other DKMS modules on the system.
- Bootloader EEPROM settings (`POWER_OFF_ON_HALT`, `PSU_MAX_CURRENT`) —
  set by the installer on a Pi 5, but system-level and possibly relied
  upon by other software.  To revert them, run `sudo rpi-eeprom-config
  -e` and remove the relevant lines manually.
- Lines outside the installer's marker block in `logind.conf` and
  `UPower.conf`.  In particular, previously commented-out keys (such
  as a deliberate `#HandleLowBattery=ignore`) are **never**
  uncommented — the installer has no way to tell whether a comment
  was its own or yours, and silently reactivating a setting you had
  intentionally disabled would be surprising.  If you had pre-existing
  values in those files, review them manually after uninstall.

---

### Manual installation (step by step)

The full numbered walkthrough — every command install.sh runs, for
those who prefer to run each step themselves — has moved to
[docs/manual-install.md](docs/manual-install.md).

### Without device tree (I²C only, no GPIO)

If you cannot or do not want to use the device tree overlay, the driver
can be loaded manually.  I²C readings (capacity and voltage) will work
but `ac_online` will always read 0 because GPIO6 cannot be claimed
without the overlay on kernel 6.12+.

```bash
sudo modprobe x120x
```

To load automatically at boot without the overlay, add `x120x` to
`/etc/modules`.

### Testing

The installer and uninstaller logic is covered by a small shell test
suite under `tests/`.  The tests sed-extract the individual functions,
mock the external commands (`rpi-eeprom-config`, the device-tree model,
sysfs paths), and assert on the resulting files and logs — nothing
touches the real system, so they run unprivileged:

```bash
bash tests/test-install.sh        # bootloader EEPROM staging
bash tests/test-ini-blocks.sh     # logind/UPower block round-trip, config.txt
bash tests/test-args.sh           # argument parsing
bash tests/test-persist.sh        # charge-mode persistence script
bash tests/test-collect-debug.sh  # diagnostics collector
```

Or run the whole suite at once:

```bash
make test
```

CI runs all of them, plus `bash -n` and `shellcheck -S warning` on the
scripts, a module compile-check (`KCFLAGS=-Werror`), and a device-tree
overlay compile, on every push and pull request.

## Verifying operation

(These are the same checks as [Step 10 of the manual installation](docs/manual-install.md#step-13--verify),
collected here for quick reference.)

```bash
# Kernel log
dmesg | grep x120x

# sysfs directly
cat /sys/class/power_supply/x120x-battery/capacity
cat /sys/class/power_supply/x120x-battery/voltage_now
cat /sys/class/power_supply/x120x-battery/capacity_level
cat /sys/class/power_supply/x120x-ac/online
cat /sys/class/power_supply/x120x-charger/charge_type

# Via UPower
upower -i /org/freedesktop/UPower/devices/battery_x120x_battery

# Test conservation mode toggle
echo "Long Life" | sudo tee /sys/class/power_supply/x120x-charger/charge_type
echo "Fast"      | sudo tee /sys/class/power_supply/x120x-charger/charge_type
```

## Module parameters

| Parameter           | Default               | Description                          |
|---------------------|-----------------------|--------------------------------------|
| `i2c_bus`           | `1`                   | I²C bus number                       |
| `i2c_addrs`         | `0x36,0x55,0x32,0x62` | Fuel gauge addresses to probe        |
| `gpio_ac`           | `6`                   | BCM GPIO for AC-present              |
| `gpio_charge_ctrl`  | `16`                  | BCM GPIO for charge control          |
| `battery_mah`       | `1000`                | Total pack capacity in mAh           |
| `conservation_start`        | `75`  | SoC % at which charging resumes in Long Life mode |
| `conservation_end`          | `80`  | SoC % at which charging stops in Long Life mode   |
| `conservation_mode_default` | `0`   | Start in Long Life mode (`1`) or Fast mode (`0`). Updated automatically on every `charge_type` sysfs write and persisted to `modprobe.d` by a udev rule. |
| `board`                     | `x120x` | Board variant: `x120x`, `x728v2`, `x728v1`, `x708`, `x729`. Set by installer. Variants other than `x120x` are experimental. |

The install script writes these to `/etc/modprobe.d/x120x.conf`.  To
change them after installation, edit that file and reboot:

```
# /etc/modprobe.d/x120x.conf
options x120x battery_mah=20000
```

Set `battery_mah` to your total pack capacity — number of cells
multiplied by per-cell capacity.  For example, an X1206 with four
5000 mAh cells: `battery_mah=20000`.

## Repository layout

```text
x120x-dkms/
├── README.md
├── CHANGELOG.md              — release history, newest first
├── LICENSE
├── SECURITY.md               — vulnerability reporting policy
├── CONTRIBUTING.md           — build, test, and PR guidelines
├── RELEASING.md              — on-hardware release checklist
├── docs/
│   ├── battery-profiles.md   — Fast vs. Long Life, with measured data
│   ├── incidents.md          — the field incident write-ups
│   ├── manual-install.md     — step-by-step install without install.sh
│   ├── migration.md          — replacing direct-GPIO scripts with sysfs
│   └── images/               — README screenshots
│       ├── battery-tray.png       — panel battery icon
│       ├── upower-info.png        — upower -i output
│       └── power-statistics.png   — GNOME Power Statistics window
├── .gitignore                — build-artifact ignore rules
├── Makefile                  — DKMS build entry point
├── dkms.conf                 — DKMS package definition
├── install.sh                — installer (see Installation)
├── uninstall.sh              — uninstaller (see Uninstallation)
├── lib/
│   └── common.sh             — helpers shared by install.sh/uninstall.sh
├── x120x-overlay.dts         — device tree overlay source
├── suptronics,x120x.yaml     — DT binding schema (upstreaming)
├── src/
│   ├── x120x.c               — the kernel driver
│   └── Kbuild
├── tests/                    — shell test suite (see Testing)
│   ├── test-install.sh
│   ├── test-ini-blocks.sh
│   ├── test-args.sh
│   ├── test-persist.sh
│   ├── test-restore-overlay.sh
│   ├── test-collect-debug.sh
│   ├── test-check-links.sh
│   └── test-check-versions.sh
├── tools/
│   ├── collect-debug.sh      — one-shot diagnostics paste (see Troubleshooting)
│   ├── check-links.sh        — markdown link checker (run by CI)
│   ├── check-versions.sh     — version-string consistency check (run by CI)
│   └── check-layout-tree.sh  — this tree is complete (run by CI)
└── .github/
    ├── dependabot.yml        — weekly GitHub Actions updates
    ├── workflows/ci.yml      — CI: shell, module build, overlay
    └── ISSUE_TEMPLATE/
        ├── bug_report.yml
        ├── hardware_report.yml
        └── config.yml
```

## Migrating from GPIO scripts

The guide to replacing direct-GPIO polling scripts with the driver's
sysfs interface has moved to [docs/migration.md](docs/migration.md).

## Companion daemon

This driver exposes raw hardware values.  For applications requiring
sophisticated battery protection — layered shutdown logic, deep-discharge
detection, voltage oscillation analysis, or event logging — a userspace
daemon can read directly from the sysfs nodes above and implement
whatever safety policy is needed.

## Upstreaming

This driver follows the conventions of
`drivers/power/supply/max17040_battery.c` in the mainline kernel.
Upstreaming is a future goal once the driver has proven itself in
production use.

## Contributing

Bug reports, hardware reports for the experimental boards, and patches
are all welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for how to
build, run the test suite (`make test`, unprivileged), and what CI
checks a pull request must pass.

## Real-world incidents that shaped this driver

The field incidents behind this driver's design — deep discharge and
cell destruction, an undetected grid return, a uevent storm, and a
driver wiped by an Ubuntu package update — are written up in full in
[docs/incidents.md](docs/incidents.md).

## Changelog

The full release history has moved to [CHANGELOG.md](CHANGELOG.md).

## Copyright

Copyright (C) 2026 Edvard Fielding <mor-lock@users.noreply.github.com>

## Disclaimer

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.

IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES, OR OTHER LIABILITY — INCLUDING BUT NOT LIMITED TO LOSS OF
DATA, HARDWARE DAMAGE, FINANCIAL LOSS, OR CONSEQUENTIAL DAMAGES OF ANY
KIND — WHETHER IN AN ACTION OF CONTRACT, TORT, OR OTHERWISE, ARISING
FROM, OUT OF, OR IN CONNECTION WITH THIS SOFTWARE OR THE USE OR MISUSE
THEREOF.

This driver interacts directly with battery hardware.  Incorrect
operation, misconfiguration, or use on unsupported hardware may result in
improper charging behaviour, failure to shut down before battery
exhaustion, or hardware damage.  You are solely responsible for
validating correct operation on your specific hardware before relying on
this driver for any purpose.

**USE AT YOUR OWN RISK.**

This project is an independent personal contribution, developed in my
own time on my own hardware.  It is not affiliated with or endorsed by
SupTronics, Geekworm, or my employer.

## License

GPL-2.0-or-later.  Every source file carries an SPDX
`GPL-2.0-or-later` header; the full GPL-2.0 text is in
[LICENSE](LICENSE).
