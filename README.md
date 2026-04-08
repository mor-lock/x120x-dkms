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

### UPower integration

UPower reads these devices automatically:

```bash
upower -e
upower -i /org/freedesktop/UPower/devices/battery_x120x_battery
```

### Battery conservation mode

Lithium-ion cells degrade faster when kept at 100% charge for extended
periods.  If your Pi is permanently plugged in — as a server, NAS, or
always-on system — keeping the battery topped up continuously will
shorten its lifespan over time.  Conservation mode addresses this by
limiting the charge range to a healthier window.

The driver supports two charge modes, selectable via `charge_type`:

- **`Fast`** (default) — charges to 100%, then disables the charger
  and re-enables it at 95%.  This float-protection hysteresis prevents
  constant micro-cycling at full charge, which degrades cells even when
  the battery appears "full".  The battery holds its charge for an
  extended period without cycling.  Best when maximum backup
  capacity is the priority.
- **`Long Life`** — the driver manages GPIO16 using user-configurable
  hysteresis: charging stops when SoC reaches `charge_control_end_threshold`
  (default 80%) and resumes when SoC drops to
  `charge_control_start_threshold` (default 75%).  Best for always-on
  systems permanently plugged into mains power.

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

Long Life mode provides an additional layer of protection.  By keeping
cells below 80%, there is more usable capacity remaining when a power
outage begins.  A battery kept at 80% has significantly more runtime
before reaching the 2% shutdown threshold than one that starts the
outage at 100% and has been micro-cycling for months.  The cells also
age more slowly, maintaining their capacity over more charge cycles.

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

### Manual installation (step by step)

If you prefer to understand each step or the install script is not
suitable for your setup, follow these instructions.

#### Step 1 — Install dependencies

```bash
sudo apt update
sudo apt install dkms raspberrypi-kernel-headers
```

`dkms` manages the kernel module and rebuilds it automatically after
kernel updates.  `raspberrypi-kernel-headers` provides the kernel
headers needed to compile the module.

#### Step 2 — Copy source to the DKMS tree

DKMS expects the source under `/usr/src/<name>-<version>/`:

```bash
sudo cp -r . /usr/src/x120x-0.2.0
```

#### Step 3 — Build and install the kernel module

```bash
sudo dkms add x120x/0.2.0
sudo dkms build x120x/0.2.0
sudo dkms install x120x/0.2.0
```

You will see compiler output scroll past — this is normal.  The build
takes about a minute on a Raspberry Pi 5.  It should end with
`DKMS: install completed`.

Verify the module is installed:

```bash
dkms status
```

You should see `x120x/0.2.0, <kernel-version>, aarch64: installed`.

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

## Changelog

### v0.2.0 — Experimental board support, deep discharge recovery, graph fixes

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

## License

GPL v2

