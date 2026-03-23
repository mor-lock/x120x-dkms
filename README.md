# x120x — Suptronics X120x UPS HAT kernel driver

A DKMS kernel driver for the Suptronics X120x UPS HAT series
(X1200, X1201, X1202, X1203, X1205, X1206) on Raspberry Pi.

Provides native Linux power supply integration equivalent to a laptop
battery — no vendor scripts, no custom daemons, no polling loops.

## What it provides

After loading, three devices appear under `/sys/class/power_supply/`:

```
/sys/class/power_supply/x120x-battery/
    status          Charging | Discharging | Not charging | Full | Unknown
    present         1 if battery detected
    voltage_now     cell voltage in µV
    capacity        0-100 %
    capacity_level  Critical | Low | Normal | Full | Unknown
    technology      Li-ion
    scope           System

/sys/class/power_supply/x120x-ac/
    online          1 = mains present, 0 = on battery

/sys/class/power_supply/x120x-charger/
    online          1 = mains present
    status          Charging | Not charging | Discharging
    charge_type     Fast | Long life  (writeable)
```

### UPower integration

UPower reads these devices automatically:

```bash
upower -e
upower -i /org/freedesktop/UPower/devices/battery_x120x_battery
```

### GNOME and KDE

The charger device exposes `charge_type` as a writeable property using
the `Long life` value for conservation mode.  This integrates natively
with:

- **GNOME 48+** — "Preserve battery health" toggle in Settings → Power
- **KDE Plasma** — charge threshold controls in System Settings → Power Management

When conservation mode is enabled, UPower calls
`EnableChargeThreshold` over D-Bus, which writes `Long life` to
`charge_type`, which drives GPIO16 high on the X1206 board, inhibiting
charging.  The entire chain — from GNOME toggle to hardware GPIO — works
without any custom userspace code.

### systemd-logind shutdown

On headless systems without a desktop environment, `systemd-logind` can
initiate a clean shutdown when `capacity_level` reaches `Critical`
(voltage ≤ 3.20 V on battery).  Enable in `/etc/systemd/logind.conf`:

```ini
HandleLowBattery=poweroff
```

## Hardware

| Signal | GPIO | Direction | Description |
|--------|------|-----------|-------------|
| I²C SDA | GPIO2 | in/out | MAX17043 data |
| I²C SCL | GPIO3 | out | MAX17043 clock |
| AC present | GPIO6 | input | High = mains OK, low = on battery |
| Charge ctrl | GPIO16 | output | Low = charging enabled (default), high = disabled |

The MAX17043/MAX17044 fuel gauge reports state-of-charge (0–100 %) and
cell voltage.  Default I²C address is `0x36`; the driver probes
`0x36, 0x55, 0x32, 0x62` in order.

## Required bootloader setting (Raspberry Pi 5)

For the UPS to reliably restart the Pi after a shutdown, the Pi must
fully remove power from the SoC when Linux halts:

```bash
sudo rpi-eeprom-config -e
```

Add:

```
POWER_OFF_ON_HALT=1
```

Save and reboot. Without this, the Pi remains partially powered after
shutdown and the UPS cannot restart it when mains power returns.

## Installation

### Prerequisites

```bash
sudo apt install dkms raspberrypi-kernel-headers
```

### Install via DKMS

```bash
sudo cp -r . /usr/src/x120x-0.1.0
sudo dkms add x120x/0.1.0
sudo dkms build x120x/0.1.0
sudo dkms install x120x/0.1.0
```

### Device tree overlay (recommended)

Compile and install:

```bash
dtc -@ -I dts -O dtb -o x120x.dtbo x120x-overlay.dts
sudo cp x120x.dtbo /boot/firmware/overlays/   # Pi 5
# or: sudo cp x120x.dtbo /boot/overlays/      # Pi 4 and earlier
```

Add to `/boot/firmware/config.txt` (Pi 5) or `/boot/config.txt`:

```
dtoverlay=x120x
```

Reboot.

### Without device tree (module parameters)

```bash
sudo modprobe x120x i2c_bus=1 gpio_ac=6 gpio_charge_ctrl=16
```

To make permanent, create `/etc/modprobe.d/x120x.conf`:

```
options x120x i2c_bus=1 gpio_ac=6 gpio_charge_ctrl=16
```

And add `x120x` to `/etc/modules`.

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
echo "Long life" | sudo tee /sys/class/power_supply/x120x-charger/charge_type
echo "Fast"      | sudo tee /sys/class/power_supply/x120x-charger/charge_type
```

## Module parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `i2c_bus` | `1` | I²C bus number |
| `i2c_addrs` | `0x36,0x55,0x32,0x62` | Fuel gauge addresses to probe |
| `gpio_ac` | `6` | BCM GPIO for AC-present |
| `gpio_charge_ctrl` | `16` | BCM GPIO for charge control |

## Companion daemon

This driver exposes raw hardware values.  For applications requiring
sophisticated battery protection — layered shutdown logic, deep-discharge
detection, voltage oscillation analysis, event logging, or MQTT/Unix
socket status publishing — a userspace daemon can read directly from
the sysfs nodes above and implement whatever safety policy is needed.

## Upstreaming

This driver follows the conventions of
`drivers/power/supply/max17040_battery.c` in the mainline kernel.
Upstreaming is a future goal once the driver has proven itself in
production use.

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

This driver interacts directly with battery hardware. Incorrect
operation, misconfiguration, or use on unsupported hardware may result in
improper charging behaviour, failure to shut down before battery
exhaustion, or hardware damage. You are solely responsible for validating
correct operation on your specific hardware before relying on this driver
for any purpose.

**USE AT YOUR OWN RISK.**

## License

GPL v2
