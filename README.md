# x120x-dkms — SupTronics X120x UPS HAT kernel driver

A DKMS kernel driver for the SupTronics X120x UPS HAT series on
Raspberry Pi.  The UPS boards are designed by SupTronics and distributed
by Geekworm.

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

**Architecture note:** The driver has been developed and tested on
Raspberry Pi OS 64-bit (`aarch64`).  The X1209 also supports Pi 4B,
Pi 3B+, and Pi 3B, which can run 32-bit Raspberry Pi OS (`armhf`).
The driver contains no architecture-specific code and should build and
run correctly on `armhf` — the DKMS build system will compile for
whatever kernel is running — but this has not been tested.  Reports
from `armhf` users are welcome.

### Not supported by this driver

- **X728** — uses a different power management IC with different GPIO
  assignments and an integrated RTC.  Requires its own driver.
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
    model_name            X120x
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

The driver reports `capacity_level=Critical` when SoC drops below 5%,
which triggers UPower's `warning-level: low` notification.  UPower then
fires `warning-level: action` when SoC reaches its `PercentageAction`
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

The installer also sets `CriticalPowerAction=PowerOff` in
`/etc/UPower/UPower.conf`.  The default value `HybridSleep` requires
swap space and will hang indefinitely on a Raspberry Pi rather than
shutting down cleanly.

## Hardware interface

All X120x boards use identical GPIO assignments:

| Signal       | GPIO  | Direction | Description                              |
|--------------|-------|-----------|------------------------------------------|
| I²C SDA      | GPIO2 | in/out    | MAX17043 fuel gauge data                 |
| I²C SCL      | GPIO3 | out       | MAX17043 fuel gauge clock                |
| AC present   | GPIO6 | input     | High = mains OK, low = on battery        |
| Charge ctrl  | GPIO16| output    | Low = charging enabled, high = disabled  |

**Note on register layout:** The MAX17043 registers on SupTronics X120x
boards are mapped differently from the datasheet.  VCELL is at register
`0x02` and SOC is at `0x04`, as confirmed by SupTronics' published
software.  This driver follows the observed hardware behaviour.

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

Two optional arguments let you configure the battery parameters at
install time rather than editing `/etc/modprobe.d/x120x.conf` by hand:

| Option | Default | Description |
|---|---|---|
| `--battery-mah N` | `1000` | Total pack capacity in mAh. Multiply per-cell capacity by number of cells. |
| `--charge-mode MODE` | `fast` | Initial charge mode: `fast` or `longlife`. Persisted across reboots. See Getting started for guidance on which to choose. |

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
sudo cp -r . /usr/src/x120x-0.1.0
```

#### Step 3 — Build and install the kernel module

```bash
sudo dkms add x120x/0.1.0
sudo dkms build x120x/0.1.0
sudo dkms install x120x/0.1.0
```

You will see compiler output scroll past — this is normal.  The build
takes about a minute on a Raspberry Pi 5.  It should end with
`DKMS: install completed`.

Verify the module is installed:

```bash
dkms status
```

You should see `x120x/0.1.0, <kernel-version>, aarch64: installed`.

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

Many X120x users run Python scripts that access GPIO6 and GPIO16
directly to monitor AC state and control charging.  Once the kernel
driver is loaded, it claims exclusive ownership of both GPIOs through
the kernel descriptor API.  Any userspace script directly accessing
these pins will fail or conflict with the driver.

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
clean shutdown automatically — no script required.

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

## Changelog

### v0.1.0 — Initial release

- Native Linux kernel driver for the full SupTronics X120x UPS HAT
  series (X1200–X1209)
- Registers three `power_supply` devices: `x120x-battery`,
  `x120x-ac`, `x120x-charger`
- Full UPower integration — battery icon, percentage, voltage, energy,
  charge rate, time-to-empty/full, battery health
- Dead battery detection — reports `health=Dead` when cells are stuck
  below 3.10 V on grid for 10 minutes with no charging progress
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

