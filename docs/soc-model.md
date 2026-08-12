# State-of-charge model

The default SoC source (`--soc-source voltage`) is a **current-sensorless
recursive voltage observer**: it reconstructs state of charge *and* battery
power from the cell terminal voltage alone (plus the grid/charger GPIOs),
because the boards ship a MAX17043-*style* ModelGauge clone whose SOC
register craters near empty — it reads 0 % while ~40 % of usable charge
remains (see [Why not just use the fuel gauge?](#why-not-just-use-the-fuel-gauge)).
The observer runs its own model; the gauge is used only for a single
top-of-charge anchor.

Pass `--soc-source gauge` to bypass the model and expose the raw gauge
register directly (unchanged legacy behaviour).

```
        ┌────────────────────────── closed loop ──────────────────────────┐
        ▼                                                                  │
 terminal V ─► V̄ (EMA) ─► I = (OCV(SoC) − V̄)/R ─► P = I·V̄ ─► ∫ = energy ─┴─► SoC (%)
                             ▲                                    ▲
        OCV branch (charge/discharge) by charge dir       gauge=100 %, 30 min on grid
                                                            └► pin energy = full
```

## The observer

State is a running **energy integral**; SoC is that energy over the usable
full energy. Every poll (~0.5 s):

```
V̄     = EMA(V_cell, α = 1/32)                 # ~16 s smoothing on the lookup V
SoC    = energy_now / E_full
OCV    = OCV_table[branch](SoC)                # rested OCV at the estimated SoC
I      = (OCV − V̄) / R                         # >0 discharge, <0 charge  (R = X120X_R_UOHM)
P      = I · V̄                                 # true battery power, for free
energy_now −= P · dt                           # integrate; clamp to [0, E_full]
SoC%   = energy_now / E_full × 100
```

This is a **closed loop**: the OCV feedback pulls the estimate back onto
the curve, so it is drift-free and needs no separate IR term — a load that
sags V below OCV simply makes `(OCV − V̄)` larger, which *is* the discharge
current that caused the sag. Because `E_full` is the *usable* window
energy, `P = E_full × dSoC/dt` reads true watts and SoC is linear in energy
(constant power → constant %/hour).

**Energy scale.** `E_full = battery_mah × 3.6 V × X120X_USABLE_PERMILLE` —
the usable window from 100 % (4.20 V) down to 0 % (3.20 V), ~64.8 Wh for
the 4×P50B pack. The rated design energy (to 2.5 V) is reported separately
as `ENERGY_FULL_DESIGN`.

## Two OCV branches (hysteresis)

NMC cells have a real open-circuit-voltage **hysteresis**: at a given SoC
the rested voltage sits higher just after charging than just after
discharging (~115 mV at low SoC, shrinking to ~0 above 80 %). So there are
**two** rested-OCV tables — `x120x_ocv_discharge[]` and `x120x_ocv_charge[]`,
56 points each — and the observer selects the branch by charge direction:

```
charging = ac_online AND NOT charger_inhibited
OCV       = charging ? charge_table(SoC) : discharge_table(SoC)
```

The charge branch is used while charging, the discharge branch on battery
and at rest. Only the *lookup* switches; `energy_now` (the state) is
continuous across a flip, so **SoC never steps — only the rate does**. A
single mean curve would read the charge current ~half a hysteresis too high
at low SoC and front-load the charge-leg SoC; the split removes that.

Both tables are energy-true **rested** OCV (0 % = 3.20 V; 100 % = 4.20 V
discharge / 4.213 V charge), built from a characterisation cycle with
settled rest points on *both* branches, coulomb-anchored; the discharge
knee below ~9 % is a scaled deep-discharge shape, and the charge branch
below ~23 % is the discharge branch plus the measured hysteresis.

## Cold-boot seed

On the first sample the energy state is seeded from the OCV lookup at the
measured voltage, IR-corrected by a *nominal* load (`X120X_SEED_DIS_UW` on
battery, `X120X_SEED_CHG_UW` on grid) so a boot mid-cycle starts near truth
rather than the load-depressed or charge-lifted terminal. The seed only
sets the starting point — the closed loop self-anchors from there (within
~2 h even from a worst-case wrong seed, always in the safe under-read
direction), and the full-charge pin erases any residual exactly. If the
gauge already reads 100 % at boot, the state starts at full directly.

## Gauge=100 pin (the only use of the gauge)

The raw MAX17043 is reliable at exactly *one* point — **full** — because it
only craters *low*. So when it has held 100 % for `X120X_CHG_FULL_DEBOUNCE_MS`
(30 min) **on grid**, the observer energy is hard-anchored to `E_full`. This
kills slow integrator drift over long floats and gives a crisp 100 % despite
the CV-taper asymptote (as SoC → 100 the charge current `(OCV − V)/R → 0`, so
the integral only approaches full; the pin is the anchor). The pin **releases
the instant the grid drops**, so a real outage tracks the drain immediately
instead of freezing at full while the laggy gauge still reads 100. The gauge
is used for nothing else — the discharge and steady paths are voltage-only.

## Power for free

The observer already computes `P = I · V̄` every poll (clamped to
`[X120X_POWER_MIN_UW, X120X_POWER_MAX_UW]`), so `POWER_NOW` is served
directly (negative = discharging), already V-EMA-smoothed — there is no
extra output filter. In `--soc-source gauge` mode the power instead comes
from an event-driven `dSoC/dt` estimator on the gauge register.

## Safety floors (SoC-independent)

Shutdown is governed by **absolute cell voltage**, not the SoC estimate:
0 % is defined at 3.20 V, and a hard floor forces `CAPACITY_LEVEL=CRITICAL`
when V ≤ `X120X_VMIN_CRITICAL_UV` (3.00 V) held ~20 s on battery, regardless
of what the observer reads. This is the backstop if the estimate is ever
wrong-high — voltage is ground truth — and it is why the model is safe to
trust: **a bad SoC number cannot over-discharge a cell.**
`X120X_SOC_CRITICAL_PCT` / `X120X_SOC_LOW_PCT` set the reported CRITICAL /
LOW levels; the OS shutdown policy (e.g. UPower `PercentageAction`) is what
actually powers off.

## `raw_capacity` (diagnostics)

The raw ModelGauge SoC is exposed as a **non-standard** sysfs attribute on
the battery power_supply device:

```
/sys/class/power_supply/x120x-battery/raw_capacity   # 0-100 %, raw gauge
/sys/class/power_supply/x120x-battery/capacity       # 0-100 %, observer
```

It is added via `power_supply_config.attr_grp` (the power_supply core
manages its lifetime) and does not affect the standard property set or the
hwmon interface. Logging both alongside each other is the intended way to
validate the observer on hardware; the raw gauge also feeds the =100 top
anchor.

## Why not just use the fuel gauge?

On a real deep discharge, the raw MAX17043 collapses to 0 % at ~3.45 V while
the pack still delivers ~40 % of its usable charge, then flatlines. Driving
shutdown off it would throw away a large fraction of runtime. The voltage
observer tracks the true, linear discharge all the way to the 3.20 V cutoff;
the gauge is trusted only for its one reliable assertion — *full* — as the
top-of-charge anchor, never for the absolute near empty.

## Tuning constants (in `src/x120x.c`)

| Constant | Meaning |
|---|---|
| `X120X_USABLE_PERMILLE` | usable-energy fraction of rated → `E_full` (SoC / energy scale) |
| `X120X_R_UOHM` | pack DC resistance in the observer (`I = (OCV − V)/R`) |
| `X120X_CHG_FULL_DEBOUNCE_MS` | gauge-held-100 % time before the 100 % pin / charge-off |
| `X120X_SEED_DIS_UW` / `_CHG_UW` | nominal load for the cold-boot IR seed (battery / grid) |
| `X120X_POWER_MIN_UW` / `_MAX_UW` | physical clamp on the reported power |
| `X120X_VMIN_CRITICAL_UV` | hard voltage floor → CRITICAL (SoC-independent backstop) |
| `x120x_ocv_discharge[]` / `x120x_ocv_charge[]` | the two rested-OCV branch tables |

## Calibration envelope & temperature

The OCV curves are calibrated for **NMC/NCA 4.2 V Li-ion** cells (a
4×Molicel INR21700-P50B pack, 1S4P) at roughly **room temperature
(~15–30 °C)**. The board has **no ambient- or battery-temperature
sensor** — the MAX17043-style gauge reports voltage only, and the only
on-board thermal reading is the Pi's CPU die, which is load-dominated
and not ambient — so there is **no temperature compensation**.

This matters far less than "uncompensated" usually implies, because the
estimate is **voltage-anchored** and the shutdown backstop is a **fixed
voltage floor** (3.20 V = 0 %, 3.00 V critical) that is
temperature-independent:

- **Temperature can never cause harm.** Over-discharge is prevented by
  the absolute voltage floor, not the SoC number — a cell at any
  temperature that reaches the floor is genuinely low. A temperature
  error in the *estimate* has no path to damaging a cell.
- **The estimate self-compensates in the direction that matters.** Cold
  cells sag faster (less usable capacity, higher resistance); because SoC
  follows the measured voltage, the observer *sees* that sag and reports
  SoC dropping faster — reflecting the cold cell's reduced capacity and
  reaching 0 % when the pack actually hits the floor.
- **The residual is a bounded curve-shape offset, not a divergence.** The
  only thing temperature truly breaks is the room-temperature
  voltage→SoC *mapping*, so out of range the SoC% reads a little
  nonlinearly (drains unevenly) and the nominal runtime-Wh drifts. The
  worst case — extreme cold under load — is an *early* shutdown that
  strands a little capacity, the safe direction (and arguably correct: a
  cold cell shouldn't be pushed hard near empty).

So outside the calibrated range the SoC readout gets less linear and the
runtime-hours estimate drifts, but it degrades **gracefully and never
unsafely**. Closing it properly would require a battery thermistor
(hardware, out of scope). The model ships **room-temperature-calibrated,
graceful outside it.** Chemistry is NMC/NCA only — another NMC cell is
expected to transfer within a few percent (re-validate per pack for best
accuracy); LiFePO4 (3.2 V, flat plateau) is unsupported, and unchargeable
by this hardware anyway.
