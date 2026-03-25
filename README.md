# x120x-dkms — SupTronics X120x UPS HAT kernel driver

A DKMS kernel driver for the SupTronics X120x UPS HAT series on
Raspberry Pi.  The UPS boards are designed by SupTronics and distributed
by Geekworm.

Provides native Linux power supply integration equivalent to a laptop
battery — no vendor scripts, no custom daemons, no polling loops.

## Getting started

If you just want to get up and running quickly, here is everything you
need in one place.

### 1. Install the driver

```bash
git clone https://github.com/mor-lock/x120x-dkms.git
cd x120x-dkms
sudo bash install.sh --battery-mah <your_capacity>
sudo reboot
```

Replace `<your_capacity>` with your total pack capacity in mAh —
multiply per-cell capacity by number of cells.  Common values:

| Hardware | Cells | Example capacity |
|---|---|---|
| X1200, X1201 | 2× 18650 | `--battery-mah 6000` |
| X1202 | 4× 18650 | `--battery-mah 12000` |
| X1205 | 2× 21700 | `--battery-mah 10000` |
| X1206 | 4× 21700 | `--battery-mah 20000` |

### 2. Configure the bootloader (Raspberry Pi 5 only)

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

### 3. Choose a charge mode

Two charge modes are available:

- **Fast** (default) — charges to 100%.  Best for occasional backup
  use where you want maximum capacity available.
- **Long Life** — limits charging to 75–80% using hysteresis.  Best
  for always-on systems permanently plugged into mains power, where
  prolonged 100% charge would degrade the cells over time.

Set the mode at install time:

```bash
# Always-on server — preserve battery longevity
sudo bash install.sh --battery-mah 20000 --charge-mode longlife

# Occasional backup use — maximise available capacity
sudo bash install.sh --battery-mah 20000 --charge-mode fast
```

The mode is persisted across reboots automatically.  You can also
change it at any time without reinstalling:

```bash
# Switch to Long Life
echo "Long Life" | sudo tee /sys/class/power_supply/x120x-charger/charge_type

# Switch back to Fast
echo "Fast" | sudo tee /sys/class/power_supply/x120x-charger/charge_type
```

### 4. Monitor battery state

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
    status          Charging | Discharging | Not charging | Full | Unknown
    present         1 if battery detected
    voltage_now     cell voltage in µV
    capacity        0-100 %
    capacity_level  Critical (<5%) | Low (<10%) | Normal | Full (≥95%) | Unknown
    technology      Li-ion
    scope           System

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
  extended period without cycling.  Best for occasional backup use
  where maximum capacity is needed.
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
`capacity_level` reaches `Critical` (battery SoC below 5%).

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

The driver reports `capacity_level=Critical` when the cell voltage drops
to or below 3.20 V on battery.  To trigger a clean OS shutdown at that
point, add the following to `/etc/systemd/logind.conf`:

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

## License

GPL v2
