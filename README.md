# x120x-dkms — SupTronics UPS HAT kernel driver

A DKMS kernel driver for SupTronics UPS HAT boards on Raspberry Pi,
distributed by Geekworm.  Covers the X120x series and experimentally
the X728, X729, and X708.

Provides native Linux power supply integration equivalent to a laptop
battery — battery icon in the taskbar, accurate state of charge,
clean undervoltage shutdown, and selectable Long Life battery
preservation mode.  No custom scripts, no daemons, no polling
loops.

## Getting started

If you just want to get up and running quickly, here is everything you
need in one place.

### 1. Configure the bootloader (Raspberry Pi 5 only)

Pi 4 and Pi 3 users can skip this step.

For reliable UPS operation — clean shutdown and automatic restart when
mains power returns — two bootloader settings are needed:

```bash
sudo rpi-eeprom-config -e
```

Add these lines, save, and reboot:

```
POWER_OFF_ON_HALT=1
PSU_MAX_CURRENT=5000
```

`POWER_OFF_ON_HALT=1` ensures the Pi fully powers off when Linux halts
so the UPS can restart it automatically when mains returns.
`PSU_MAX_CURRENT=5000` suppresses spurious low-power warnings.

### 2. Install the driver

Two charge modes are available — choose one before installing:

- **Fast** (default) — charges to 100%, then disables the charger
  and leaves the battery floating.  Charging resumes when the state
  of charge (SoC) drops to 95% due to self-discharge.  Best when
  maximum backup capacity is the priority.
- **Long Life** — charges to 80%, then disables the charger and leaves
  the battery floating.  Charging resumes when SoC drops to 75% due
  to self-discharge.  Best when the system is permanently on mains
  and full capacity is rarely needed — keeping cells below full charge
  significantly extends their lifespan.

Replace `<your_capacity>` with your total pack capacity in mAh —
multiply per-cell capacity by number of cells.  The mAh rating is
printed on the battery cell itself.  Common values:

| Hardware | Cells | Example capacity |
|---|---|---|
| X1200, X1201 | 2× 18650 | `--battery-mah 6000` |
| X1202 | 4× 18650 | `--battery-mah 12000` |
| X1205 | 2× 21700 | `--battery-mah 10000` |
| X1206 | 4× 21700 | `--battery-mah 20000` |

```bash
git clone https://github.com/mor-lock/x120x-dkms.git
cd x120x-dkms

# Permanently on mains — preserve battery longevity
sudo bash install.sh --battery-mah <your_capacity> --charge-mode longlife

# Maximum backup capacity
sudo bash install.sh --battery-mah <your_capacity> --charge-mode fast

sudo reboot
```

The charge mode is persisted across reboots automatically.  It can
also be changed at any time without reinstalling:

```bash
echo "Long Life" | sudo tee /sys/class/power_supply/x120x-charger/charge_type
echo "Fast"      | sudo tee /sys/class/power_supply/x120x-charger/charge_type
```

### 3. Monitor battery state

After rebooting, the battery appears as a standard Linux power supply.
The easiest way to see full details is `gnome-power-statistics`:

```bash
sudo apt install gnome-power-manager
gnome-power-statistics
```

This shows live battery percentage, voltage, energy, charge rate,
and history graphs — all read directly from the driver via UPower.
No configuration needed.

For a quick command-line view:

```bash
upower -i /org/freedesktop/UPower/devices/battery_x120x_battery
```

That is all that is needed for a fully working installation.  The
rest of this document covers the driver interface, hardware details,
and advanced configuration in depth.

---

## Supported hardware

All models share an identical software interface and are fully supported
by this driver:

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

To install for an X728 V2.x board:

```bash
sudo bash install.sh --battery-mah 6000 --board x728v2
sudo reboot
```

Available board variants: `x120x` (default), `x728v2`, `x728v1`, `x708`, `x729`.

**Important notes for experimental boards:**

- Long Life mode is only available on boards with charge control (X120x and
  X728 V2.5). On all other boards `charge_type` is read-only and always
  returns `Fast`.
- The power-off GPIO pulse is registered via `pm_power_off` and fires after
  `systemctl poweroff`. The DT overlay must provide the `power-off-gpios`
  property for this to work — without it a warning is logged and the UPS
  will not cut power automatically after shutdown.
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
  test pins rather than the 40-pin header.  No I2C fuel gauge or GPIO
  interface accessible from the Pi.  Software shutdown not supported.
- **X735** — power management and PWM fan controller, not a UPS.  Has
  no battery fuel gauge and no I2C interface.  Nothing for this driver
  to interface with.
- **X-UPS1** — a universal stackable UPS with 12V/5V dual output and
  no Raspberry Pi GPIO integration.  No I2C fuel gauge interface.

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
    capacity              0-100 %
    capacity_level        Critical (<5%) | Low (<10%) | Normal | Full (≥95%) | Unknown
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
    charge_control_start_threshold  SoC % to resume charging in Long life mode (writeable, default 75)
    charge_control_end_threshold    SoC % to stop charging in Long life mode (writeable, default 80)
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
mechanism for always-on systems.  Conservation mode addresses this by
holding the battery at a lower resting state of charge, where calendar
aging is significantly slower.

The driver supports two charge modes, selectable via `charge_type`:

- **`Fast`** (default) — charges to 100%, disables the charger, and
  re-enables it at 95% to replace self-discharge.  The 5% hysteresis
  band prevents the charger from micro-cycling against the full-charge
  cutoff.  Cells rest at or near full voltage, so calendar aging
  continues at its normal rate.  Best when the priority is maximum
  backup capacity at the moment an outage begins.
- **`Long Life`** — charges to `charge_control_end_threshold` (default
  80%), disables the charger, and re-enables it at
  `charge_control_start_threshold` (default 75%).  Cells spend their
  idle life at a noticeably lower voltage, where calendar aging is
  dramatically reduced.  The trade-off is 20% less runtime during an
  outage; the benefit is that the cells retain meaningfully more of
  their original capacity after several years.  Best when the pack is
  oversized relative to your worst realistic outage, or when deferring
  the eventual cell replacement matters more than per-outage runtime.
  See **Choosing a profile** below — slower aging does *not*
  automatically mean more runtime years later, because `Long Life`
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

The default thresholds (75% / 80%) match the recommendation of TLP, the
widely-used Linux power management tool, and are a commonly accepted
balance between battery longevity and available backup capacity.

The default thresholds can also be changed permanently via module
parameters in `/etc/modprobe.d/x120x.conf`:

```
options x120x battery_mah=20000 conservation_start=75 conservation_end=80
```

### Choosing a profile: runtime vs. longevity

The choice between `Fast` and `Long Life` is usually framed as "more
backup capacity now" versus "longer battery lifespan later."  That
framing is incomplete.  It overlooks one fact: in `Long Life` mode the
battery does not only age more slowly — it also **starts every outage
15–20 percentage points lower**.  Slower aging has to first overcome
that head start before it yields any extra *usable runtime*, and for a
UPS that is sized close to its actual backup needs, it often never does
within the life of the device.

The model below estimates **usable runtime at the start of an outage**,
as a function of years in service, for both profiles.  Usable runtime
is the energy available between the resting state of charge and the
10% safe-shutdown floor, scaled by the capacity the cells have retained
to that point.

> **Assumptions (read these before trusting the numbers).**  These are
> a *model*, not measurements of your hardware, and the ranking is only
> as good as the assumptions behind it:
>
> - **Load:** ~8–9 W continuous (a Raspberry Pi 5 with NVMe).  Runtime
>   scales inversely with load — double the load, halve every number.
> - **Pack:** an X1206 with 4× 21700 cells, fresh full→empty ≈ 7 h.
>   Runtime scales linearly with pack capacity; a smaller pack shifts
>   every row down proportionally but does **not** change the ranking.
> - **Shutdown floor:** 10% SoC (the driver's clean-shutdown trigger).
> - **Resting charge:** `Fast` rests at ~95% true SoC (observed 4.186 V
>   on a healthy NMC pack with the charger disabled); `Long Life` holds
>   80%.
> - **Calendar aging:** assumed **3%/yr capacity loss at ~95% SoC** and
>   **2%/yr at 80% SoC**, at a moderate ~25 °C.  These are illustrative
>   midpoints from general Li-ion NMC literature, **not** measured for
>   any specific cell.  Real rates vary widely with cell quality and,
>   especially, **temperature** — calendar aging roughly doubles per
>   +10 °C, so a pack running warm (e.g. in the Pi's exhaust) ages far
>   faster than this and *both* columns shrink.
> - **Cycle aging is neglected** — a UPS sees very few cycles, so
>   calendar aging dominates.  This assumption fails if your grid drops
>   often enough to cycle the pack regularly.
> - Runtime is treated as proportional to the state-of-charge span,
>   ignoring the nonlinear "knee" near the bottom of the discharge curve.

**These rates assume moderate-quality NMC, not any particular cell.**
Cell quality shifts the result but rarely the ranking.  The "more
runtime" comparison is driven by *geometry* — `Long Life` gives up ~21%
of its usable span up front and must claw it back through slower aging —
so what matters is how fast the cells age relative to that handicap:

- **Premium cells** (e.g. Molicel) age slowly in absolute terms, so the
  year-0 runtime gap — which is pure starting charge, not aging —
  persists for longer.  `Fast` wins *more* decisively and the crossover
  pushes well past year 20.
- **Budget or hot-running cells** age fast, eroding both columns
  quickly and pulling the crossover in.  With genuinely poor cells (or
  a pack baking in the Pi's exhaust), `Long Life` can edge ahead as
  early as year 10.

What would change the model itself is **chemistry, not brand**: the
numbers bake in NMC at 4.2 V.  LiFePO₄ cells have far flatter calendar
aging and much weaker sensitivity to storage charge, which shrinks the
`Long Life` benefit toward nothing and makes `Fast` win harder still.

Under the moderate-NMC assumptions (base case: 3%/yr vs 2%/yr):

| Years in service | `Fast` (rest ~95%) | `Long Life` (hold 80%) | More runtime |
|---|---|---|---|
| 0  | 5.9 h | 4.9 h | `Fast` (+1.0 h) |
| 5  | 5.1 h | 4.4 h | `Fast` (+0.7 h) |
| 10 | 4.4 h | 4.0 h | `Fast` (+0.4 h) |
| 15 | 3.8 h | 3.6 h | `Fast` (+0.1 h) |
| 20 | 3.2 h | 3.3 h | `Long Life` (+0.1 h) |

The counter-intuitive result: **`Fast` delivers more usable runtime
than `Long Life` for roughly the first two decades.**  The lower
starting charge in `Long Life` costs ~21% of the usable span up front,
and the slower aging does not repay that until the curves cross at
around year 20 — by which point the pack is well past a routine
replacement anyway.  The crossover is sensitive to the aging gap: if
`Long Life` ages much more slowly than assumed (e.g. 1.5%/yr) the
crossover pulls in toward year 15; if the benefit is smaller (2.5%/yr)
`Fast` wins past year 20.  In none of these cases does `Long Life` win
at year 10.

What `Long Life` *does* buy is **capacity retention**, not runtime: at
year 10 the 80%-held pack retains ~82% of its original capacity versus
~74% for the 95%-held pack.  That defers the eventual *replacement*; it
does not give you a longer outage on any given day.

So the two profiles optimise different things:

- **`Fast` (default)** — choose when **outage ride-through is the
  priority** and the pack is sized close to your needs.  Wins on
  runtime for the realistic life of the device.
- **`Long Life`** — choose when the pack is **oversized** relative to
  your worst realistic outage (you have runtime to spare, so giving
  some back costs nothing useful), when **postponing cell replacement**
  matters more than per-outage runtime, or when the cells are
  **expensive or awkward to replace**.

Note that **both** profiles already disable the charger once the pack
reaches its ceiling, rather than holding it on a continuous float — so
both avoid the single worst calendar-aging stressor (sitting pinned at
4.2 V indefinitely).  The remaining difference between them is only the
resting state of charge.  If you are unsure, the default `Fast` is the
right call for a capacity-constrained UPS: keep the cells cool, never
let them deep-discharge (see *Incident 1*), and a replacement — if ever
needed — is cheap and infrequent.

#### Measured runtime, and what 80% actually costs

The model's year-0 row is not just theory — it matches a real
full-depth discharge captured in the power database (the *Incident 1*
outage, before the undervoltage shutdown existed, so the pack drained
all the way down).  On an X1206 (4× 21700) at ~5 W idle load, measured
from a full start:

| Milestone | Time on battery |
|---|---|
| Down to 50% | ~4.2 h |
| **10% — clean auto-shutdown** | **~5.9 h** |
| 0% — fully empty | ~7.0 h |

Note the curve: the first half drains slowly on the flat part of the
discharge (~4 h to 50%), then collapses — the back half is gone in
under two hours.  This is why the 10% shutdown floor matters, and why
starting lower hurts disproportionately.

Because `Long Life` begins every outage at 80% instead of ~95%, it
enters that drain ~15 points down and reaches the shutdown floor at
roughly **4.8 h instead of 5.9 h** — about **one hour less ride-through,
immediately, on every outage**.  And as the table above shows, that lost
hour is not repaid by slower aging until ~year 20.  So on a pack sized
close to its job (here ~6 h against typical 2–5 h outages), limiting to
80% sheds backup time you are actually using, with no practical payback
— which is exactly why `Fast` is the default.

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
threshold (default 2%), which causes `systemd-logind` to initiate a
clean OS shutdown — well before the cells reach a dangerous voltage.
The install script configures this automatically via
`HandleLowBattery=poweroff` in `logind.conf` and
`CriticalPowerAction=PowerOff` in `UPower.conf`.

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
detects this automatically.  When the system is on grid power and the
cell voltage remains below 3.10 V for 10 minutes with no meaningful
voltage rise (less than 10 mV/h), the battery health is reported as
`Dead`:

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

#### Further protection with Long Life mode

Long Life mode also provides an indirect safety benefit during outages.
Over months or years of always-on operation, cells held at 80% retain
more of their original capacity than cells held at 100% — they have
aged less.  When an outage eventually happens, a Long Life battery
starts from a lower state of charge but delivers closer to its rated
runtime from that point, while a 100%-held battery starts higher but
has lost more capacity over the same period.  For systems where the
UPS is rarely called on, this capacity retention compounds over the
lifespan of the cells.

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
UPower's `PercentageAction` threshold is reached (default 2% SoC).
The driver reports `capacity_level=Critical` at 5% SoC, which triggers
UPower's low battery warning.  The actual shutdown fires at 2% when
UPower escalates to `warning-level: action`.

The install script enables this automatically by setting the following
in `/etc/systemd/logind.conf`:

```ini
HandleLowBattery=poweroff
```

To disable it, change the line to:

```ini
HandleLowBattery=ignore
```

The installer also configures `/etc/UPower/UPower.conf` with two
settings:

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
can float low at boot before the X1206 hardware has finished
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
- The charger (GPIO16) is forced low at probe and defaults to enabled
  whenever SoC is below the stop threshold — the battery starts charging
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

Two bootloader settings are recommended for reliable UPS operation:

```bash
sudo rpi-eeprom-config -e
```

Add:

```
POWER_OFF_ON_HALT=1
PSU_MAX_CURRENT=5000
```

- `POWER_OFF_ON_HALT=1` — ensures the Pi fully cuts power to the SoC
  when Linux halts, so the UPS can restart it cleanly when mains power
  returns.  Without this the Pi remains partially powered after shutdown
  and cannot be restarted by the UPS.
- `PSU_MAX_CURRENT=5000` — tells the Pi that its power supply can
  deliver 5 A, suppressing spurious low-power warnings when drawing
  high current through the UPS board.

Save and reboot.

## Installation

### Quick install (recommended)

Clone the repository and run the install script:

```bash
git clone https://github.com/mor-lock/x120x-dkms.git
cd x120x-dkms
sudo bash install.sh
```

The script handles everything and tells you what it is doing at each
step.  Reboot when it finishes.

#### Install script options

Optional arguments configure the driver at install time:

| Option | Default | Description |
|---|---|---|
| `--battery-mah N` | `1000` | Total pack capacity in mAh. Multiply per-cell capacity by number of cells. |
| `--charge-mode MODE` | `fast` | Initial charge mode: `fast` or `longlife`. Persisted across reboots. See Getting started for guidance on which to choose. |
| `--board VARIANT` | `x120x` | Board variant. See Experimental board support for details. Variants other than `x120x` are untested. |

Examples:

```bash
# X1206 with four 5000 mAh 21700 cells
sudo bash install.sh --battery-mah 20000

# X1205 with two 5000 mAh 21700 cells
sudo bash install.sh --battery-mah 10000

# X1206 always-on server — Long Life mode
sudo bash install.sh --battery-mah 20000 --charge-mode longlife


# Show available options
sudo bash install.sh --help
```

If omitted the default (1000 mAh) is used and can be changed
later by editing `/etc/modprobe.d/x120x.conf` and rebooting.

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
- The marker-wrapped block that the installer added to
  `/etc/systemd/logind.conf` (delimited by
  `# >>> x120x-dkms: logind-low-battery (do not edit) >>>` ...
  `# <<< x120x-dkms: logind-low-battery <<<`)
- The marker-wrapped block that the installer added to
  `/etc/UPower/UPower.conf` (delimited by
  `# >>> x120x-dkms: upower-pi-tweaks (do not edit) >>>` ...
  `# <<< x120x-dkms: upower-pi-tweaks <<<`)
- Any bare lines left over from older (pre-marker) installer versions

The following are intentionally left unchanged:

- The `dkms` and `linux-headers-$(uname -r)` packages — removing them
  could break other DKMS modules on the system.
- Bootloader EEPROM settings (`POWER_OFF_ON_HALT`, `PSU_MAX_CURRENT`) —
  these are system-level settings that may have been configured
  independently.  To revert them, run `sudo rpi-eeprom-config -e` and
  remove the relevant lines manually.
- Lines outside the installer's marker block in `logind.conf` and
  `UPower.conf`.  In particular, previously commented-out keys (such
  as a deliberate `#HandleLowBattery=ignore`) are **never**
  uncommented — the installer has no way to tell whether a comment
  was its own or yours, and silently reactivating a setting you had
  intentionally disabled would be surprising.  If you had pre-existing
  values in those files, review them manually after uninstall.

---

### Manual installation (step by step)

If you prefer to understand each step or the install script is not
suitable for your setup, follow these instructions.

#### Step 1 — Install dependencies

```bash
sudo apt update
sudo apt install dkms linux-headers-$(uname -r)
```

`dkms` manages the kernel module and rebuilds it automatically after
kernel updates.  `linux-headers-$(uname -r)` provides headers that
match the currently running kernel exactly, which is what DKMS needs
to compile the module.

> **Note:** older Raspberry Pi OS releases used a single metapackage
> `raspberrypi-kernel-headers`.  On Bookworm and later this metapackage
> may pull headers for a different kernel than the one you booted with,
> which causes DKMS builds to fail with `kernel headers ... cannot be
> found`.  Use the kernel-specific package shown above to avoid that.

#### Step 2 — Copy source to the DKMS tree

DKMS expects the source under `/usr/src/<name>-<version>/`:

```bash
sudo cp -r . /usr/src/x120x-0.4.3
```

#### Step 3 — Build and install the kernel module

```bash
sudo dkms add x120x/0.4.3
sudo dkms build x120x/0.4.3
sudo dkms install x120x/0.4.3
```

You will see compiler output scroll past — this is normal.  The build
takes about a minute on a Raspberry Pi 5.  It should end with
`DKMS: install completed`.

Verify the module is installed:

```bash
dkms status
```

You should see `x120x/0.4.3, <kernel-version>, aarch64: installed`.

#### Step 4 — Compile the device tree overlay

The overlay tells the kernel how the board is wired (I²C address,
GPIO assignments) so the driver can claim the hardware correctly.

```bash
dtc -@ -I dts -O dtb -o x120x.dtbo x120x-overlay.dts
```

#### Step 5 — Install the overlay

```bash
# Raspberry Pi 5 (Raspberry Pi OS Bookworm):
sudo cp x120x.dtbo /boot/firmware/overlays/

# Raspberry Pi 4 and earlier:
sudo cp x120x.dtbo /boot/overlays/
```

#### Step 6 — Enable the overlay at boot

Open the boot configuration file:

```bash
# Raspberry Pi 5:
sudo nano /boot/firmware/config.txt

# Raspberry Pi 4 and earlier:
sudo nano /boot/config.txt
```

Add these lines at the end of the file:

```
[all]
dtoverlay=x120x
```

The `[all]` section header ensures the overlay is applied on all Pi
models.  Without it, any `[cm4]` or `[cm5]` conditional blocks earlier
in the file will prevent the overlay from loading on a Pi 5.

Save and exit (`Ctrl+O`, `Enter`, `Ctrl+X` in nano).

#### Step 7 — Configure the bootloader (Raspberry Pi 5 only)

Two bootloader settings are recommended for reliable UPS operation.
Both are set in the same file:

```bash
sudo rpi-eeprom-config -e
```

Add these lines:

```
POWER_OFF_ON_HALT=1
PSU_MAX_CURRENT=5000
```

- `POWER_OFF_ON_HALT=1` — ensures the Pi fully cuts power to the SoC
  when Linux halts, so the UPS can restart it cleanly when mains power
  returns.  Without this the Pi remains partially powered after shutdown
  and cannot be restarted by the UPS.
- `PSU_MAX_CURRENT=5000` — tells the Pi that its power supply can
  deliver 5 A, suppressing spurious low-power warnings when drawing
  high current through the UPS board.

Save and exit.

#### Step 8 — Configure low-battery shutdown

The driver reports `capacity_level=Critical` when SoC drops below 5%.
UPower escalates to `warning-level: action` at 2% SoC (its default
`PercentageAction` threshold), which triggers a clean OS shutdown via
logind.  To enable this, add the following to `/etc/systemd/logind.conf`:

```bash
sudo nano /etc/systemd/logind.conf
```

Add or update:

```ini
HandleLowBattery=poweroff
```

To disable this behaviour at any time, change the value to `ignore`.

The install script does this automatically.

#### Step 9 — Reboot

```bash
sudo reboot
```

#### Step 10 — Verify

After the reboot, check that everything is working:

```bash
# Confirm the overlay loaded and the driver initialised
dmesg | grep x120x

# Check the three power_supply devices exist
ls /sys/class/power_supply/

# Read live values
cat /sys/class/power_supply/x120x-battery/capacity
cat /sys/class/power_supply/x120x-battery/voltage_now
cat /sys/class/power_supply/x120x-ac/online

# Full UPower view
upower -i /org/freedesktop/UPower/devices/battery_x120x_battery
```

Expected output from `dmesg | grep x120x`:

```
x120x: loading out-of-tree module taints kernel.
x120x 1-0036: MAX1704x at 0x36 version 0x000
x120x 1-0036: x120x UPS ready (battery=x120x-battery ac=x120x-ac charger=x120x-charger)
```

The "taints kernel" message is normal for any out-of-tree module.

`voltage_now` is reported in µV — divide by 1,000,000 for volts.
A healthy fully charged cell reads approximately 4,150,000 (4.15 V).

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

## Verifying operation

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

## Migrating from GPIO scripts

Many users of these boards run Python scripts that access GPIO6 and
GPIO16 directly to monitor AC state and control charging.  Once the
kernel driver is loaded, it claims exclusive ownership of these GPIOs
through the kernel descriptor API.  Any userspace script directly
accessing these pins will fail or conflict with the driver.

### GPIO6 — AC present (replace with sysfs)

Scripts that read GPIO6 to detect grid loss can be replaced with a
simple sysfs read:

```bash
# Old approach — direct GPIO access (will fail with driver loaded)
# pinctrl get 6
# gpio_value = open("/sys/class/gpio/gpio6/value").read()

# New approach — read from driver via sysfs
cat /sys/class/power_supply/x120x-ac/online
# 1 = mains present, 0 = on battery
```

In Python:

```python
def ac_online():
    with open('/sys/class/power_supply/x120x-ac/online') as f:
        return f.read().strip() == '1'
```

UPower also publishes AC state over D-Bus if your application
already uses UPower.

### GPIO16 — Charge control (managed by driver)

GPIO16 is reserved by the driver and cannot be accessed from
userspace while the driver is loaded.  This is intentional — the
driver manages it safely with proper locking and hysteresis.

In practice there should be little need to control GPIO16 directly:

- **Fast mode** — the driver automatically stops charging at 100%
  and floats the battery, resuming at 95%.  No script needed to
  prevent micro-cycling.
- **Long Life mode** — the driver manages hysteresis between the
  configured thresholds (default 75%/80%).  Equivalent to what
  GPIO16 scripts were trying to achieve, but implemented correctly
  in the kernel with mutex protection.
- **Charge mode** is selectable and persistent via sysfs:

```bash
# Enable Long Life mode (stop at 80%, resume at 75%)
echo "Long Life" | sudo tee /sys/class/power_supply/x120x-charger/charge_type

# Adjust thresholds
echo 90 | sudo tee /sys/class/power_supply/x120x-charger/charge_control_end_threshold
echo 85 | sudo tee /sys/class/power_supply/x120x-charger/charge_control_start_threshold
```

### Battery status (replace with sysfs or UPower)

Scripts that read the MAX17043 fuel gauge over I²C directly will
continue to work — the driver does not prevent I²C reads from
userspace.  However, reading from sysfs is simpler and requires no
I²C library:

```bash
cat /sys/class/power_supply/x120x-battery/capacity      # 0-100 %
cat /sys/class/power_supply/x120x-battery/voltage_now   # µV
cat /sys/class/power_supply/x120x-battery/status        # Charging | Discharging | ...
```

### Shutdown on power loss

Scripts that poll AC state and call `shutdown` when power is lost
can be removed entirely.  The driver reports `capacity_level=Critical`
at 2% SoC (via UPower PercentageAction), which causes systemd-logind to initiate a
clean shutdown automatically — no script required.  This works
identically on headless and desktop installations.

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

## Real-world incidents that shaped this driver

This driver was developed on hardware running unattended, always-on.
Two real power incidents exposed failure modes that no lab test would
have found — and drove significant hardening of the driver.

A companion daemon running on the same system reads the driver's sysfs
nodes continuously, logs every reading to a SQLite database, and
implements layered shutdown logic on top of what the driver exposes.
All power data cited in the incidents below — SoC, voltage, AC state,
charge state, shutdown events, and PSU power draw — comes from that
database.  The driver surfaces the raw hardware values; the daemon
records and acts on them.

---

### Incident 1 — Deep discharge and cell destruction (2026-03-05)

#### What happened

A grid outage began at 17:20 UTC on 2026-03-05.  At the time, there
was no software undervoltage shutdown in place — it had been assumed
that the UPS hardware would cut power before the cells could be
damaged.  That assumption was wrong: the X120x UPS hardware has no
automatic undervoltage cutoff.  It simply powers the Pi until the
cells are physically unable to sustain the load.

The system ran on battery for 10.3 hours with nothing to stop it.
The fuel gauge saturated at 0% SoC when the cell voltage reached
3.25V — from that point on, voltage was the only reliable signal.
By 02:39 UTC the voltage had fallen below 3.0V, the point at which
irreversible electrochemical damage begins in lithium-ion cells.  The
Pi ran until 03:38 UTC when the supply rail collapsed at 2.54V.

When grid power returned at 08:58 UTC, the battery had been destroyed.
The cells could no longer hold a charge above ~2.99V despite being on
grid for 26+ hours.  Post-mortem analysis of the voltage data confirmed
a characteristic oscillation signature — rapid ±20 mV swings at the
fuel gauge output, a known pattern when the MAX17043 is alternating
reads across cell groups that can no longer hold voltage.

#### What the data revealed

Analysis of the power database from the incident produced several
findings that informed the driver design:

- The fuel gauge saturates at 0% SoC while voltage is still 3.25V —
  well above the damage threshold.  Once SoC hits 0%, **voltage is the
  only reliable indicator** of remaining capacity.
- Cell damage begins at approximately 3.0V, confirmed by the onset of
  voltage oscillation in the data.  Sixty-three oscillations of >15mV
  within intervals of <30 seconds were recorded in the first 100
  sub-3.0V readings — a distinct signature not seen during healthy
  discharge.
- A destroyed battery on grid shows a characteristic plateau: voltage
  rises only ~165mV over 26 hours (surface charge only), never enters
  a `CHARGING` state, and settles around 2.99V.  Healthy cells charge
  from 2.8V to 4.1V within 2–3 hours.
- The gap between the 10% SoC shutdown trigger and the 3.20V voltage
  trigger is approximately 14 minutes.  If the SoC-based trigger fails,
  the voltage backstop is the last line of defence before cell damage.

#### What was added to the driver

The core lesson was that the X120x hardware provides **no undervoltage
protection** — software must supply it entirely.  This shaped several
additions to the driver:

**Dead battery detection** — when the system is on grid and the cell
voltage remains below 3.10V for 10 minutes with no meaningful rise
(less than 10mV/h), the driver reports `health=Dead` via the
`x120x-battery/health` sysfs node.  UPower surfaces this as
`health: dead` and desktop environments display a battery warning.
A kernel log entry is also emitted.  This allows the operator to
identify destroyed cells and replace them before relying on the UPS
for protection again.

**Capacity level reporting** — the driver reports `capacity_level`
accurately throughout the discharge curve, giving UPower and logind
the information needed to trigger a clean shutdown via the standard
`HandleLowBattery=poweroff` path before the cells reach a dangerous
voltage.  Without this, UPower has no basis on which to act.

The incident made clear that the X120x hardware provides no
undervoltage protection whatsoever — software must supply it entirely.
The install script configures the complete shutdown chain automatically:
the driver reports `capacity_level=Critical` at low SoC, UPower
escalates this to `warning-level: action`, and logind calls
`systemctl poweroff` — all without any additional daemon or script.
No extra userspace software is required beyond what the installer
sets up.

---

### Incident 2 — Grid return undetected, recovery livelock (2026-03-29)

#### What happened

A grid outage began at **10:26:50 UTC on 2026-03-29** with the battery
at 82% SoC / 4.04 V.  The system ran on battery normally, discharging
at the expected rate.

Grid was restored approximately 1 hour after the outage began
(confirmed by the uptime of a desktop machine on the same circuit), but
the X1206 never detected the return — `ac_online` remained `0` for the
remainder of the discharge.  Because `powerd.py` saw no grid, charging
never resumed.  The system continued draining as if the outage was
still in progress.

The companion daemon's shutdown mechanism worked correctly:
`shutdown_armed` fired at **14:29:28 UTC** at 10.0% SoC / 3.59 V, and
`shutdown_initiated` followed 15 seconds later exactly as designed.
At that point the grid had already been back for approximately 3 hours,
and the cells should have been charging throughout that window.  They
were not, because the board was silently failing to assert GPIO6.

When the system rebooted after shutdown, `ac_online` was still `0`
despite the charger being connected.  The
system entered a livelock: it booted, UPower immediately read
`capacity_level=Critical` on the near-empty battery, logind called
`systemctl poweroff`, the UPS cut and then restored power, and the
cycle repeated.  This drained the cells further on every cycle.

The livelock ran across three dates — 2026-03-11 (2 cycles from the
initial recovery attempt), 2026-03-30 (11 cycles), and 2026-04-02 (5
cycles, the last confirmed shutdown voltage 3.15 V) — for a total of
**21 forced shutdowns** before the board was replaced.  The database
records no `ac_online=1` after the original outage, because the board
was never able to drive GPIO6 high again.

#### Root cause analysis

**X1206 hardware failure — GPIO6 output stage.**  Forensic analysis of
the power database confirms that `ac_online` never returned `true`
after the 10:26:50 UTC grid loss, with the grid independently confirmed
as restored roughly an hour later.  The GPIO6 output stage on the board
had failed silently during normal operation: not at boot, not under
load stress, but mid-session while the system was running.  This is
a harder failure mode than a boot-time marginal-PSU scenario — the
board stopped driving its own AC-present signal while everything else
appeared functional.

The v0.3.0 driver fixes (GPIO6 pull-up, `capacity_level=Critical`
suppressed on AC, always-on charger at probe) mitigated the livelock
mechanism by protecting against a floating GPIO6 at boot.  They could
not compensate for a board whose output stage had permanently failed.
Board replacement was the correct and necessary remedy.

With `ac_online=0` and the battery at near-zero SoC, the livelock
chain on every boot was:

1. UPower read `capacity_level=Critical` and fired
   `warning-level: action` immediately — before the driver had finished
   probing.
2. logind received the action and called `systemctl poweroff`.
3. The UPS cut power, then restored it (auto-restart on halt).
4. The cycle repeated.

#### What was added to the driver

**`gpio=6=pu` pull-up in `config.txt`** — the installer now adds a
software pull-up on GPIO6.  The UPS hardware actively drives GPIO6 low
on power loss and high when AC is present.  The pull-up ensures the pin
reads high (AC present) by default during boot, before the hardware has
finished asserting the signal.  This protects against GPIO6 floating
low during the boot window; it cannot compensate for a board whose
output stage has failed entirely.

**`capacity_level=Critical` only reported on battery** — the driver
previously reported `capacity_level=Critical` whenever SoC dropped
below 5%, regardless of AC state.  On a nearly-dead battery with AC
present, this caused UPower to trigger a shutdown loop during recovery.
The driver now only reports `Critical` when `ac_online=0` — when mains
is present, even at 0% SoC, the battery is charging and shutting down
would cause exactly the livelock described above.

**Charger always enabled at probe** — the driver explicitly drives
GPIO16 low (charger enabled) at probe time, regardless of any
previously saved state.  A battery that has been deeply discharged
starts charging immediately on every boot.

**Charger default changed to always-on** — the charge hysteresis logic
previously only re-enabled the charger when SoC dropped below the
resume threshold.  The start threshold has been removed: the charger is
now enabled whenever SoC is below the stop threshold, defaulting to on
in all uncertain or low-SoC states.

**0% SoC no longer treated as implausible** — the driver previously
issued a MAX17043 quick-start command when the initial SoC reading was
0%, treating it as a fuel gauge convergence failure.  After deep
discharge the battery is genuinely at 0% — issuing a quick-start
resets the fuel gauge's SoC model at the worst possible moment.  The
plausibility floor has been lowered to 0%.

#### Resolution — X1206 board replacement (2026-04-07)

The faulty board was replaced with a new X1206 on 2026-04-07.  The
existing Molicel INR-21700-P50B cells (4 × 5000 mAh, 20 Ah pack) were
reinstalled — deeply depleted by the livelock cycles but undamaged,
as the repeated shutdowns had kept the voltage above the cell damage
threshold throughout.
The power supply was also replaced with a multi-port GaN charger
(Anker Prime 160 W) giving the Pi and the mobile router
independent ports with separate overcurrent protection, eliminating
any shared-PSU load concern at boot.

The new board's first reading, at **18:20:53 UTC on 2026-04-07**, showed
`ac_online=1` immediately — GPIO6 asserting correctly from the first
moment — with `soc_pct=0.01%` and `bat_v=3.34 V`.  The v0.3.0/v0.4.0
recovery path worked exactly as designed: `capacity_level=Critical` was
suppressed because `ac_online=1`, UPower did not trigger a shutdown, and
the charger was on from the first probe.  Zero livelock cycles occurred.

At 20:36:20 UTC, a brief `grid_change: true → null → true` transition
lasting ~0.5 s was recorded — this corresponds to the v0.4.0 driver
module being reloaded during installation.  Charging continued without
interruption.

The cells charged from 0.01% / 3.34 V to 100% / 4.19 V in
approximately **6.7 hours**, consistent with the X1206's 3 A charge
ceiling (~15 W) applied to a 20 Ah pack.  PSU draw measured via the
driver's hwmon interface held steady at **~16.7 W** throughout the bulk
charge phase (battery charging plus Pi idle consumption), dropping to
**~5.9 W** once the cells reached full charge and the charger switched
to float.

| Milestone | Time (UTC, 2026-04-07/08) | SoC | Cell voltage |
|---|---|---|---|
| First valid reading | 18:20:53 | 0.01% | 3.34 V |
| Charging begins | 18:33:24 | 0.32% | 3.48 V |
| 10% | 20:16:52 | 10.0% | 3.74 V |
| 50% | 22:02:38 | 50.0% | 3.94 V |
| 80% | 23:21:27 | 80.1% | 4.09 V |
| Full (~100%) | 01:02:05+1 | 99.6% | 4.22 V |

The healthy charge profile — smooth SoC rise, voltage climbing steadily
from 3.34 V to 4.22 V, no oscillation, no plateau — confirmed that the
new cells were undamaged.

#### Operational lesson

**X1206 GPIO6 output stage failure is silent and undetectable in
software.**  The board continued to appear functional in every other
respect: the fuel gauge was readable over I²C, and the system ran
normally on battery.  Only the AC-present signal was
wrong, and only the power database — recording `ac_online=0` throughout
a period when grid was independently confirmed as restored — revealed
the failure.

If `ac_online` remains `0` after a grid outage despite the charger
LED indicating input power, and the pattern persists across multiple
reboots with the v0.3.0+ driver and `gpio=6=pu` in place, the board
itself should be suspected and replaced.  The driver cannot work around
a permanently failed GPIO6 output stage.

### Incident 3 — uevent storm from uninitialised stack variable (2026-05-20)

#### What happened

The system fan on the host Pi 5 had been audibly revving for an extended
period.  CPU temperature was a benign **65.9 °C** and the SoC was not
thermally throttled (`vcgencmd get_throttled` reported `0x0`), but the
fan's PWM cooling device was sitting at `cur_state=2/4` continuously,
indicating sustained cooling demand driven by compute load rather than
silicon heat.

`uptime` reported a load average of **7.01** on a 4-core Pi 5 — fully
saturated.  `ps` showed three processes consuming the bulk of the CPU:

```
    345 91.7 systemd-udevd
 818657 87.1 (udev-worker)
 818612 83.6 (udev-worker)
```

`udevadm monitor --kernel` revealed a continuous flood of `change`
uevents from `/sys/class/power_supply/x120x-charger`, advancing the
kernel `SEQNUM` counter by roughly **820 events per second**.  Over the
21 h uptime preceding the diagnosis, the system had emitted approximately
**62 million** uevents on this single device — every one of them carrying
identical property values, and every one of them woken up udev to scan
the rules database and re-evaluate the same hook chain.

#### Root cause analysis

**Uninitialised `chrg_changed` stack variable in `x120x_poll_work`.**

The poll work function declared three booleans on entry:

```c
bool bat_changed, ac_changed, chrg_changed;
```

In the I²C error paths all three were set to `false` before the
`goto notify` jump.  In the happy path `bat_changed` and `ac_changed`
were assigned unconditionally from the new vs. cached comparisons, but
`chrg_changed` was only assigned to `true` inside the conservation-mode
hysteresis block when `want_inhibit != chip->charger_inhibited` — i.e.
only when GPIO16 actually needed to flip.  In the steady state this
branch is rarely taken, so the variable was read at the notify site with
whatever garbage the stack happened to contain.

The compiler-generated stack frame produced a truthy value on most
invocations, causing `power_supply_changed(chip->charger)` to fire every
poll.  This kicked off a tight feedback loop via the `supplied_to`
notification chain:

1. `power_supply_changed(charger)` schedules `power_supply_changed_work`
2. The kernel walks supplicants — the battery is supplied by the charger
3. The battery's `external_power_changed` callback fires
4. That callback calls `mod_delayed_work(system_wq, &chip->work, 0)`,
   kicking `x120x_poll_work` to run immediately
5. The poll reads I²C, finds no real state change, but reads the
   uninitialised `chrg_changed` as truthy and fires
   `power_supply_changed(charger)` again
6. Goto 1

`bpftrace`-confirmed rates during the incident:

| Function                       | Calls / second |
|---|---|
| `x120x_poll_work`              | ~405 (vs. the intended 2 Hz) |
| `power_supply_changed_work`    | ~412 |
| `power_supply_changed(charger)`| ~423 |
| kernel `uevent_seqnum` growth  | ~820 |

The poll loop was running **200× faster than designed**, each iteration
re-triggering the loop on a stack-resident phantom.

The bug was latent from v0.4.1, where the polling work function was
restructured to take snapshots of `conservation_mode` and `capacity_pct`
under the chip mutex (see that release's changelog).  The refactor
introduced the unconditional read of `chrg_changed` at the notify site
without ensuring the variable was initialised on every path leading
there.  GCC's `-Wmaybe-uninitialized` does not fire on this case because
the variable *is* assigned on the failing path (via the
`if (want_inhibit != chip->charger_inhibited)` branch), just not on
every path.

#### What was added to the driver

**Default-initialise `bat_changed`, `ac_changed`, and `chrg_changed`
at declaration.**  All three booleans now default to `false`, so the
notify site reads `true` only when an explicit assignment marked a real
state change.  Defensive initialisation of all three (not just the
one that bit us) prevents the same class of bug from reappearing the
next time a path is added to the function.

#### Operational lesson

**Sustained fan noise without a hot SoC means a software bug, not a
thermal one.**  At 65 °C the Pi 5's silicon is well inside its comfort
envelope; the fan curve responds to total CPU load, not just core
temperature.  If the fan is loud while `vcgencmd measure_temp` reports
something benign, the first place to look is `uptime` and the top of
`ps`.  In this case the load average pointed at udev within seconds —
and `udevadm monitor --kernel` exposed the storm in another two.

`/sys/kernel/uevent_seqnum` is an underused diagnostic.  Reading it
twice with a delay gives the kernel-wide uevent rate in a single
shell pipeline:

```bash
s1=$(cat /sys/kernel/uevent_seqnum); sleep 2; \
  s2=$(cat /sys/kernel/uevent_seqnum); echo $(( (s2-s1) / 2 ))/sec
```

A healthy idle system reports `0/sec`.  Anything higher than the low
tens, sustained, is a misbehaving driver.

#### Independent confirmation on a different board variant

A second user ([issue #2](https://github.com/mor-lock/x120x-dkms/issues/2))
hit the same bug independently on a **Geekworm X1209 + X1002 NVMe**
expansion board running v0.4.2, on the same day the author diagnosed
it on a X1206.  Their symptom was different — no audible fan, but the
attached Samsung 970 Evo NVMe was heatsoaking to **70–75 °C** at idle
versus a normal **~51 °C** on v0.4.1.  The mechanism is the same: the
udev rule `90-x120x-persist.rules` runs
`/usr/local/lib/x120x-persist-mode.sh` on every `change` event, and at
~820 events per second the constant `fork`+`exec` plus small writes
keep the NVMe controller pinned in its highest active power state and
the PCIe link out of L1 substates.  After upgrading to v0.4.3 their
NVMe settled back to **51–52 °C** within ~15 minutes and
`uevent_seqnum` delta reported `0/sec`.

The same reporter noted that their v0.4.1 idle NVMe temperature
(**58–61 °C**) was elevated above the clean v0.4.3 baseline
(**51–52 °C**) by ~7–10 °C.  This is consistent with the uninitialised
`chrg_changed` reading as *intermittently* truthy on v0.4.1's stack
frame layout — same bug, but a lower duty cycle than the
always-truthy pathology v0.4.2 happened to produce.  v0.4.3's explicit
`= false` initialiser makes the variable deterministically falsy on
every entry, so the baseline should now match a system that never had
the bug.

This second data point matters because it widens the symptom set
documented for this incident: the same bug can present as fan noise
on a host with mediocre case airflow and no NVMe, as silent NVMe heat
on a host with a stack expansion board, or — in principle — as
elevated power draw and slightly shortened battery runtime on any
host.  Future reports that don't match the original "loud fan" shape
should still trigger the same diagnostic (`uevent_seqnum` delta) as
the first step.

## Changelog

### v0.4.3 — uevent storm fix

**Kernel driver**
- Initialise `bat_changed`, `ac_changed`, and `chrg_changed` to `false`
  at declaration in `x120x_poll_work`.  Previously `chrg_changed` was
  declared without an initialiser and only assigned to `true` inside
  the conservation-mode hysteresis block; in the steady state (the
  common case) the variable was read uninitialised at the notify site
  and the compiler-generated stack value was truthy often enough to
  fire `power_supply_changed(chip->charger)` on most poll cycles.
  Combined with the `supplied_to` propagation chain back into the
  battery's `external_power_changed` callback (which immediately
  reschedules the poll work), this turned a 2 Hz poll into a ~400 Hz
  feedback loop and produced approximately 820 `change` uevents per
  second on `/sys/class/power_supply/x120x-charger`.  The flood
  saturated `systemd-udevd` and two worker processes at ~90% CPU each.
  See *Real-world incidents — Incident 3* for the full diagnosis.

### v0.4.2 — Security audit follow-ups

**Installer**
- `set -euo pipefail` so unset variables and pipeline failures abort
  the install rather than continuing silently.
- `--battery-mah` is now validated as a positive integer at parse
  time.  The value is interpolated directly into
  `/etc/modprobe.d/x120x.conf`, so any non-numeric, empty, negative,
  zero, or shell-injection-shaped input is rejected before anything
  is written.  Leading zeros are normalised away to avoid any octal
  interpretation downstream.
- The device tree overlay is now compiled into a root-owned
  `mktemp -d` (mode 700, cleaned up on `EXIT`) rather than the source
  directory, and copied from there into `/boot/firmware/overlays/`.
  If the source tree happens to live on a path an unprivileged user
  can write to, compiling in place opened a brief TOCTOU window
  between `dtc` finishing and `cp` running.  A private tmpdir closes
  it.
- `logind.conf` and `UPower.conf` edits are wrapped in marker blocks
  delimited by `# >>> x120x-dkms: <tag> (do not edit) >>>` /
  `# <<< x120x-dkms: <tag> <<<`.  The installer:
  - never comments out lines it did not write;
  - relies on the systemd / UPower INI rule that the **last** matching
    key in a section wins, so appending our block at the bottom
    overrides any earlier user setting without disturbing it;
  - creates the `[Login]` / `[UPower]` section header on a minimal
    config file before writing the block;
  - is idempotent — a second install replaces the existing block in
    place rather than appending a duplicate;
  - cleans up bare lines left behind by pre-v0.4.2 installers
    (via `clean_legacy_logind` / `clean_legacy_upower`) so an upgrade
    from an older install doesn't accumulate dead comments.

**Uninstaller**
- **Regression fix:** the uninstaller no longer uncomments any line.
  Previous versions ran
  `sed -i 's/^#HandleLowBattery=/HandleLowBattery=/'` and the two
  analogous lines for `UPower.conf`, intending to "restore" what the
  installer had commented out.  But the installer commented blindly
  without recording which lines were originally uncommented, so a
  user who had deliberately written e.g. `#HandleLowBattery=ignore`
  to disable that policy would silently have it reactivated on
  uninstall.  All three restoration steps are removed.
- Marker-wrapped blocks are removed by `remove_ini_block`; lines
  outside the markers are never touched.
- Legacy line-by-line cleanup is retained (factored into the same
  `clean_legacy_logind` / `clean_legacy_upower` helpers used by the
  installer) so users upgrading from older installer versions still
  get cleaned up correctly.
- `set -euo pipefail`.

**Kernel driver**
- `pm_power_off` legacy function pointer replaced with
  `devm_register_sys_off_handler(SYS_OFF_MODE_POWER_OFF_PREPARE, ...)`.
  Only applies to the experimental X728/X708/X729 board variants
  (X120x does not use this path).  Benefits:
  - PREPARE-mode handlers may sleep, so the 3-second power-off pulse
    now uses `msleep(3000)` instead of a `mdelay(3000)` busy-wait.
  - The sys-off API supports stacking; we no longer unconditionally
    clobber a power-off handler that another driver may have
    installed.
  - `devm` cleanup tears down the handler automatically on unbind, so
    `x120x_remove` is simpler and the static global
    `x120x_poweroff_chip` is gone.
- GPIO16 (charge-control) state is now cached in
  `chip->charger_inhibited`, and the entire read-modify-write of the
  cached flag plus the hardware GPIO is performed under `chip->lock`.
  This closes a race where `x120x_poll_work`'s hysteresis decision
  could in principle disagree with a concurrent `charge_type` write
  from sysfs, briefly inhibiting charging based on stale state for
  one poll tick.  The same change also removes three unlocked reads
  of the hardware GPIO from the sysfs `get_property` callbacks; they
  now read the cached value, which is by definition consistent with
  `conservation_mode` because both are written under the same lock.
- README: the manual-install instructions now use
  `linux-headers-$(uname -r)` (matching the running kernel) rather
  than `raspberrypi-kernel-headers`, which can pull stale headers on
  Bookworm and cause DKMS builds to fail.
- Boot-log noise eliminated: when the DT overlay is present the
  module init function no longer races against it.  Previously, after
  `i2c_add_driver` returned (with the DT-instantiated client at 0x36
  already bound), the manual probe loop would try to register a
  duplicate client at 0x36, get EBUSY (logged by the i2c subsystem),
  fall through to 0x55, succeed in creating a phantom client, and
  produce three scary lines in dmesg
  (`Failed to register i2c client x120x at 0x36 (-16)`,
  `1-0055: failed to read chip version: -121`, and
  `1-0055: probe with driver x120x failed with error -121`).
  `probe()` now sets a flag on success; `x120x_init` checks the flag
  immediately after `i2c_add_driver` returns and skips the manual
  fallback entirely if a DT binding already happened.  Cosmetic only
  — the driver was functional before — but stops people thinking
  their install is broken.

### v0.4.1 — Installer and uninstaller robustness, locking cleanup

**Uninstaller**
- Uninstaller now discovers every installed version of the driver via
  `dkms status` and removes them all, rather than relying on a single
  hardcoded version string.  Fixes a case where `uninstall.sh` left the
  kernel module installed if it had been built against a different
  version than the uninstaller expected.
- Orphaned `/usr/src/x120x-*` source trees are cleaned up even if DKMS
  no longer tracks them.

**Installer**
- `apt-get update` is now run before `apt-get install` so that a Pi
  with a stale package index does not fail to find `dkms` or the
  kernel headers package.
- Removed a duplicated Step 10 block that re-installed the charge-mode
  persistence script and udev rule a second time.  Functionally
  harmless (same content written to the same files), but cleaned up
  for clarity.

**Kernel driver**
- The polling work function now snapshots `conservation_mode` and
  `capacity_pct` under the chip mutex and uses the local copies in
  the subsequent hysteresis region, rather than reading the `chip`
  fields a second time outside the lock.  Fixes a minor correctness
  issue where the hysteresis decision could in principle race against
  a concurrent `charge_type` write from sysfs.

### v0.4.0 — hwmon interface, rate estimation fix

**hwmon device registration**
- Driver now registers a hwmon device (`x120x`) alongside the existing
  `power_supply` devices at probe time
- Exposes four channels via `/sys/class/hwmon/hwmonN/`:
  - `in0_input` — cell voltage in mV (direct hardware reading, label `cell_voltage`)
  - `curr1_input` — charge/discharge current in mA, signed (derived, label `battery_current`)
  - `power1_input` — charge/discharge power in µW, signed (derived, label `battery_power`)
  - `energy1_input` — stored energy in µJ (derived, label `battery_energy`)
- Sign convention: positive = charging, negative = discharging
- Compatible with `sensors` (lm-sensors), Prometheus `node_exporter`
  (`--collector.hwmon`, enabled by default), collectd, Grafana, and any
  other tool that reads the standard Linux hwmon sysfs interface
- node_exporter exposes `node_hwmon_in_volts`, `node_hwmon_curr_amps`,
  `node_hwmon_power_watt`, `node_hwmon_energy_joules` labelled `chip="x120x"`
  with no additional configuration
- hwmon registration failure is non-fatal — the `power_supply` interface
  remains the primary ABI and the driver continues normally if hwmon
  registration fails

**Rate estimation fix**
- Fixed a bug where `energy_rate_uw` (and therefore `POWER_SUPPLY_PROP_POWER_NOW`,
  and all hwmon power/current channels) was permanently zero
- Root cause: `chip->capacity_256` was overwritten with `new_256` before
  the rate estimator compared `new_256 != chip->capacity_256` — the
  comparison was always equal so no rate was ever computed
- Fix: snapshot `old_256 = chip->capacity_256` before the update
- UPower's displayed `energy-rate` was unaffected because UPower computes
  its own rate from consecutive `energy_now` polls independently of the
  driver; `power_now` and all hwmon derived channels were the affected paths
- Added spike rejection: when the SoC register is stuck for >90 s and
  then jumps multiple LSBs in a single tick, the resulting rate estimate
  would be a large transient spike (large ΔE ÷ clamped dt).  The driver
  now detects this condition (real dt > 90 s clamp window) and retains
  the previous rate estimate rather than emitting the spike

### v0.3.0 — Deep discharge recovery hardening, GPIO6 pull-up, graph fixes

**Deep discharge recovery hardening**
- `capacity_level=Critical` only reported when on battery
  (`ac_online=0`) — on AC the battery is charging; reporting Critical
  caused UPower to trigger an immediate shutdown livelock after a deep
  discharge event
- 0% SoC no longer treated as implausible — quick-start command not
  issued on a genuinely empty battery, avoiding a fuel gauge reset
  during recovery
- Charger (GPIO16) explicitly forced low at probe — charging starts
  immediately on every boot regardless of any previously latched state
- Charger default changed to always-on: the resume threshold is
  removed; the charger is enabled whenever SoC is below the stop
  threshold, defaulting to on in all uncertain or low-SoC states

**GPIO6 pull-up**
- `gpio=6=pu` added to `config.txt` by installer — prevents GPIO6
  floating low at boot before the X1206 hardware asserts the AC-present
  signal, eliminating false `ac_online=0` readings after a power outage
  or PSU overload at boot

**UPower history and graph fixes**
- `NoPollBatteries=true` set in `UPower.conf` by installer — eliminates
  spurious `0%/unknown` history entries caused by UPower polling the
  kernel independently of driver notifications
- Battery status during Fast mode float is `Discharging` rather than
  `Not charging` — UPower records `discharging` history entries during
  float so gnome-power-statistics graphs stay populated
- Self-discharge floor (−1 mW) reported as `power_now` when SoC is
  stable for >90 s — prevents graph gaps during float periods
- 30-second heartbeat `power_supply_changed()` notification — keeps
  UPower history recording active during extended stable float periods
- AC state change no longer resets the rate tracking window — rate
  computation is continuous across grid transitions, eliminating
  transition spikes in the rate graph

### v0.2.0 — Experimental board support, additional properties, dead battery detection

**Experimental board support**
- Experimental support for Geekworm X728 V2.x/V1.x, X708, X729 via
  `--board` parameter in `install.sh`
- `pm_power_off` hook pulses the power-off GPIO on these boards after
  OS shutdown so the UPS cuts power automatically

**Additional power_supply properties**
- `manufacturer`, `model_name`, `charge_now`, `charge_full`,
  `charge_empty`, `voltage_max_design`, `voltage_min_design` added
- `energy_now`, `energy_full`, `energy_empty` computed from SoC and
  pack capacity

**Dead battery detection**
- Driver reports `health=Dead` when cell voltage remains below 3.10 V
  on grid for ≥ 10 minutes with no meaningful voltage rise (<10 mV/h)
  and SoC ≤ 2% — detects cells destroyed by deep discharge
- Kernel log entry emitted on confirmation; clears automatically if
  voltage recovers

**Deep discharge recovery hardening**
- `capacity_level=Critical` only reported when on battery
  (`ac_online=0`) — on AC the battery is charging; reporting Critical
  caused UPower to trigger an immediate shutdown livelock after a deep
  discharge event
- 0% SoC no longer treated as implausible — quick-start command not
  issued on a genuinely empty battery, avoiding a fuel gauge reset
  during recovery
- Charger (GPIO16) explicitly forced low at probe — charging starts
  immediately on every boot regardless of any previously latched state
- Charger default changed to always-on: the resume threshold is
  removed; the charger is enabled whenever SoC is below the stop
  threshold, defaulting to on in all uncertain or low-SoC states

**GPIO6 pull-up**
- `gpio=6=pu` added to `config.txt` by installer — prevents GPIO6
  floating low at boot before the X1206 hardware asserts the AC-present
  signal, eliminating false `ac_online=0` readings after a power outage
  or PSU overload at boot

**UPower history and graph fixes**
- `NoPollBatteries=true` set in `UPower.conf` by installer — eliminates
  spurious `0%/unknown` history entries caused by UPower polling the
  kernel independently of driver notifications
- Battery status during Fast mode float is `Discharging` rather than
  `Not charging` — UPower records `discharging` history entries during
  float so gnome-power-statistics graphs stay populated
- Self-discharge floor (−1 mW) reported as `power_now` when SoC is
  stable for >90 s — prevents graph gaps during float periods
- 30-second heartbeat `power_supply_changed()` notification — keeps
  UPower history recording active during extended stable float periods
- AC state change no longer resets the rate tracking window — rate
  computation is continuous across grid transitions, eliminating
  transition spikes in the rate graph

**Migration guide**
- Added guide for users migrating from existing GPIO scripts

### v0.1.0 — Initial release

- Native Linux kernel driver for the full SupTronics X120x UPS HAT
  series (X1200–X1209)
- Registers three `power_supply` devices: `x120x-battery`,
  `x120x-ac`, `x120x-charger`
- Full UPower integration — battery icon, percentage, voltage, energy,
  charge rate, time-to-empty/full, battery health
- **Fast mode** — charges to 100%, then floats with 95% recharge
  threshold to prevent micro-cycling
- **Long Life mode** — configurable conservation hysteresis
  (default 75%/80%) to extend cell lifespan
- Charge mode persisted across reboots via udev rule
- Clean undervoltage shutdown via UPower/logind at 2% SoC
- DKMS packaging — survives kernel updates automatically
- Device tree overlay for GPIO descriptor API (kernel 6.12+)
- `install.sh` with `--battery-mah` and `--charge-mode` options

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
own time on my own hardware.  It is not affiliated with or endorsed by SupTronics, Geekworm, or my employer.

## License

GPL v2

