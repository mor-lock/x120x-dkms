# Upstreaming

**Status: a long-term goal, deliberately not pursued yet.** The driver is
written and maintained to mainline-kernel quality so that upstreaming
remains open, but the restructuring that a mainline submission would
require is not, today, a good trade for the people running this driver.
This document explains what is kept upstream-ready, what upstreaming
would actually cost, and the condition under which the decision would be
revisited. It is a deliberate engineering position, not an admission that
the code isn't ready.

## What is kept upstream-ready

The out-of-tree status is not an excuse for out-of-tree quality. The
driver is held to the standard a mainline reviewer would apply, and CI
enforces it:

- **`checkpatch.pl --no-tree` is clean with no ignores** — `src/x120x.c`
  passes the mainline style check outright (see the `checkpatch` job in
  `.github/workflows/ci.yml`).
- **`W=1` and sparse (`C=1`) are clean**, kernel-doc on every function.
- The **device-tree binding** (`suptronics,x120x.yaml`) validates against
  the dtschema meta-schema.
- **SPDX headers**, `GPL-2.0-or-later`, and correct `MODULE_*` /
  `MODULE_DEVICE_TABLE` metadata throughout.
- Idiomatic `power_supply` property tables, `devm_`-managed lifetime, and
  standard sysfs ABIs — charge thresholds via
  `POWER_SUPPLY_PROP_CHARGE_CONTROL_{START,END}_THRESHOLD` and Long Life
  via `POWER_SUPPLY_CHARGE_TYPE_LONGLIFE`, the same interfaces laptop
  vendor drivers use for conservation mode.

Keeping this bar means a future upstream effort starts from clean code,
not a rewrite for style.

## What upstreaming would actually require

Mainline would not accept this as a single driver. The kernel models
these functions by physical device, and a submission would have to be
decomposed into three layers:

1. **Fuel gauge → a patch to `drivers/power/supply/max17040_battery.c`.**
   The MAX17043 on these boards is already supported in mainline; the
   only real difference is the register-layout quirk (VCELL at `0x02`,
   SOC at `0x04`). That belongs in the existing driver as a new chip
   variant, not in a new driver. Long Life mode cannot live here — a fuel
   gauge only measures; it has no charge-control capability.

2. **AC-detect + charge control → a new, small charger driver.** This is
   the genuinely new part, and it is upstreamable: Long Life mode
   **survives unchanged**, because the charge-threshold and
   `CHARGE_TYPE_LONGLIFE` interfaces are exactly how mainline expects a
   charger to expose conservation mode. The power-off GPIO pulse (which
   physically cuts power *after* Linux has decided to halt) registers as
   a standard `sys_off_handler`.

3. **Shutdown decision → userspace.** The one function mainline would push
   back on is the driver initiating shutdown itself.

So the common assumption — "upstreaming means ripping out Long Life mode"
— is wrong twice over. Long Life stays (it is standard ABI), and the only
thing that actually has to go is the kernel-side `orderly_poweroff()` on
the undervoltage floor.

## The cost: the undervoltage poweroff backstop

The distinction that decides this is narrow but decisive:

- The **power-off GPIO handler** acts *after* the shutdown decision is
  made — mainline-acceptable, and it would be kept.
- The **`orderly_poweroff()` call on the voltage floor** *makes* the
  shutdown decision in the kernel — this is shutdown policy, which
  mainline declines in favour of asserting
  `POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL` and letting userspace act.

That kernel-side poweroff is not a stylistic nicety. It is the safety
mechanism the driver exists for. This hardware has **no hardware
undervoltage protection** — left alone it drains the cells until the
boost converter's UVLO trips, well past the point of Li-ion damage. The
mainline-preferred userspace path (UPower → logind
`HandleLowBattery=poweroff`) is **provably unreliable on common targets**:
`HandleLowBattery` does not exist before systemd 255 (Debian 12 ships
252), and UPower is frequently D-Bus-inactive on a headless box. On the
author's own hardware this chain silently did nothing and nearly
destroyed a cell pack — twice (see
[Incident 1](incidents.md#incident-1--deep-discharge-and-cell-destruction-2026-03-05)
and
[Incident 2](incidents.md#incident-2--grid-return-undetected-recovery-livelock-2026-03-29)).
The driver still asserts `CAPACITY_LEVEL=CRITICAL`, so a healthy
userspace chain acts first; the kernel poweroff is the backstop for when
it doesn't.

Upstreaming would ask the driver to hand that responsibility back to the
exact path already shown to fail on the systems that need protecting
most. That is a regression in the one property that matters for a UPS:
*not destroying the battery it is meant to protect.*

## The decision

For now, an excellent, well-maintained out-of-tree DKMS driver is the
**better product for its users** than the mainline restructure would be.
Out-of-tree keeps the standard sysfs ABIs *and* the undervoltage
backstop; upstreaming would trade the backstop away. Mainline's objection
is a design principle ("no shutdown policy in the kernel"); the driver's
requirement is physical ("the cells must not reach UVLO"). Where those
conflict, the hardware wins, and out-of-tree is where the hardware is
allowed to win.

This is not "we couldn't mainline it." The code is kept mainline-ready
precisely so the door stays open. It is that the mainline design
constraints are, for this specific device, the wrong ones.

**When this would be revisited:** if the userspace low-battery shutdown
chain becomes reliable on the supported targets (e.g. a systemd baseline
of ≥ 255 across the field, with a dependably-running UPower), the
backstop's value drops and the cost of the decomposition falls with it.
A cheap first step at that point would be an RFC to the linux-pm list
asking the one question that sets the whole shape — extend
`max17040_battery.c`, or a new driver? — before committing to any rework.
