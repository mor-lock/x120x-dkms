# Migrating from GPIO scripts

Part of [x120x-dkms](../README.md).

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
  configured thresholds (default 78%/80%).  Equivalent to what
  GPIO16 scripts were trying to achieve, but implemented correctly
  in the kernel with mutex protection.
- **Charge mode** is selectable and persistent via sysfs:

```bash
# Enable Long Life mode (stop at 80%, resume at 78%)
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
below 5% SoC, and UPower's `PercentageAction` (set to 2% SoC by the
installer) then causes systemd-logind to initiate a clean shutdown
automatically — no script required.  This works identically on headless
and desktop installations.
