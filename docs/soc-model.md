# State-of-charge model

The default SoC source (`--soc-source voltage`) is a **current-sensorless
recursive voltage observer**: it reconstructs state of charge *and* battery
power from the cell terminal voltage alone (plus the grid/charger GPIOs),
because the boards ship a MAX17043-*style* ModelGauge clone whose SOC
register craters near empty — it reads 0 % while ~40 % of usable charge
remains (see [Why not just use the fuel gauge?](#why-not-just-use-the-fuel-gauge--recovering-stranded-capacity)).
The observer runs its own model; the gauge is used only for a single
top-of-charge anchor.

![One clean drain and recharge cycle: cell voltage, the voltage-observer
SoC — near-linear under the constant ~9 W drain — against the sagging,
cratering fuel gauge, and battery power](images/soc-cycle.png)

*One clean cycle: 30 min of float at 100 %, then an outage draining the
pack under ~9 W of load to the floor, then a recharge.
**Middle panel** — the shipped voltage-observer SoC (blue) vs the raw fuel
gauge (orange). The raw gauge reads **6 % while the pack is really ~29 %**;
it doesn't actually reach 6 % until **≈ 1.5 h later**, when the grid was
restored — that 1.5 h (≈ 15 Wh at this ~9 W load) is runtime a gauge-trusting
system would throw away. Both 6 % crossings are in this cycle, so the gap is
directly observed, not extrapolated.
**Bottom** — battery power `P = I·V`, which the observer produces for free
(green = the charger's measured power into the pack, `c3 − Pi`, for
reference; the small gap to the cell-side observer power is DC-DC loss).*

Pass `--soc-source gauge` to bypass the model and expose the raw gauge
register directly (unchanged legacy behaviour).

```
        ┌────────────────────────── closed loop ──────────────────────────┐
        ▼                                                                  │
 terminal V ─► V̄ (EMA) ─► I = (OCV(SoC) − V̄)/R ─► P = I·V̄ ─► ∫ = energy ─┴─► SoC (%)
                             ▲                                    ▲
        OCV branch (charge/discharge) by charge dir       gauge=100 %, 1 h on grid
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

This is a **closed loop**, and that is what makes it robust. Concretely: if
the estimate drifts a few percent *high*, then `OCV(SoC_est)` sits above the
true rested OCV, so `(OCV − V̄)` — and thus the inferred current — is
*over*-estimated, which drains the estimate back down toward truth; a *low*
estimate over-reads V relative to its OCV and corrects the same way. The
loop has no way to run away, so it is drift-free, needs **no separate IR
term** (a load that sags V below OCV simply makes `(OCV − V̄)` larger, which
*is* the discharge current that caused the sag), and a rough starting seed
converges on its own (typically within ~2 h, always from the safe
under-read side). And because SoC is defined on *energy* rather than
remaining charge, the scale is linear under load and `power_now` reads true
watts — see [Linear SoC (the energy basis)](#linear-soc-the-energy-basis).

**Energy scale.** `E_full = battery_mah × 3.6 V × X120X_USABLE_PERMILLE` —
the usable window from 100 % (4.20 V) down to 0 % (3.20 V), ~64.8 Wh for
the 4×P50B pack. The rated design energy (to 2.5 V) is reported separately
as `ENERGY_FULL_DESIGN`.

## Linear SoC (the energy basis)

SoC is defined as `energy_now / E_full` — a fraction of *energy*, not of the
remaining coulombs (charge). That single choice is what makes the percentage
behave the way people expect, and it has consequences worth stating plainly.

**Constant power draws a straight line.** Under a steady load SoC falls at a
constant %/hour, because energy leaves at a constant rate. A coulomb-based SoC
does not: even when *perfectly* estimated it bends downward late in a discharge,
because holding constant watts as the cell voltage sags means pulling *more*
amps, so charge drains faster than energy does. The straight blue observer
trace in the cycle figure above — against the sagging, cratering gauge — is that
linearity made visible.

**Half the number is half the runtime.** At steady load "50 % = half the runtime
remaining" is exactly true, so a naive time-to-empty extrapolation — UPower's, or
a human glancing at the number — is trustworthy rather than optimistic near
empty. The same energy scale is also why `power_now = E_full × dSoC/dt` reads
true watts: the time-derivative of an energy fraction, scaled by the energy, *is*
power.

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
(1 h) **on grid**, the observer energy is hard-anchored to `E_full`. This
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

**Reported power is *net*, not gross.** `P` is the power at the battery
terminal — the current actually entering or leaving the cell. Because the Pi
draws from the same node the charger feeds, during a charge the terminal lifts
only by the *net* current (charge minus the Pi's concurrent draw), and that net
is what the observer reports. That is the right quantity — SoC integrates net
current, so a heavy load never corrupts the state — but it does mean the charge
reading *understates the charger's gross output* by whatever the Pi is drawing,
and no voltage-only observer can do better: "15 W in − 11 W load" and "4 W in −
no load" produce an identical terminal voltage. Recovering gross charge power
would need a current sensor on the charge path, which this hardware lacks. (On
the bench, under a hard and varying Pi load, the reported net tracked an
independent charger-side measurement — `c3 − Pi` in the cycle figure above — to
within 0.1 W across the whole charge, confirming the net itself is accurate.)

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

## Why not just use the fuel gauge? — recovering stranded capacity

On a real deep discharge the raw MAX17043 clone reads far too low near empty
and then flatlines at 0 %. In the cycle above it reads **6 % while the pack is
really ~29 %**, and does not actually reach 6 % until **~1.5 h later** (when
the grid was restored) — so ~1.5 h of runtime is recovered simply by not
trusting the gauge at 6 %. It bottoms out reading 0 % while ~23 % still
remains. Both of those are directly observed in that one cycle.

How far the observer stretches the pack before its *own* safety shutdown is
load-dependent, and no everyday outage has yet taken the observer all the way
to its 2 % cutoff (they end on grid return at ~5–6 %) — so that figure is an
estimate, not a routine measurement. Replaying the observer over a March
run-to-destruction (~7 W) puts a number on it: the observer's 2 % shutdown
fires **~2.4 h after** the gauge's would (≈3.1 h to the 3.00 V hard-critical
floor), recovering the ~28 % the gauge writes off — proportionally more at a
lighter idle load. The gauge collapses because it is a *dynamic* model that
mis-tracks the discharge; the rested-OCV curve does not. So the gauge is
trusted for exactly one thing — its one reliable assertion, **full**, as the
top-of-charge anchor — and never for the absolute near empty, where it is
worst and where the shutdown decision is actually made.

### Is deeper discharge actually safe for the cells?

The converse worry — that the gauge's early 0 % was *protecting* the pack —
does not hold. The recovered runtime lives between roughly **3.5 V and 3.20 V
rested**, where the gauge was stranding usable capacity well above any
protective threshold, not guarding the cells; the observer just spends the
headroom the gauge left on the table.

Our 0 % is defined at **3.20 V rested**, a deliberately conservative floor. The
cells are rated for discharge to 2.5 V, and the sustained-damage onset measured
on *this exact hardware* is ~3.0 V (see
[Incident 1](incidents.md#incident-1--deep-discharge-and-cell-destruction-2026-03-05))
— so the cutoff keeps ~200 mV of margin above it, and the SoC-independent
**3.00 V hard-critical** backstop stands between the 3.20 V cutoff and the point
where damage actually accrues. Voltage, not the SoC estimate, is what stops the
discharge.

Deeper discharge does cost cycle life, but only on the outage cycles
themselves — rare on a standby UPS, whose dominant wear is *calendar* aging at
high SoC (see [Battery profiles](battery-profiles.md#two-ways-a-cell-wears-out)).
The protection budget is spent at the *top* of the range; the bottom is visited
too rarely for the extra depth to matter.

## Tuning constants (in `src/x120x.c`)

| Constant | Meaning |
|---|---|
| `X120X_USABLE_PERMILLE` | usable-energy fraction of rated → `E_full` (SoC / energy scale) |
| `X120X_R_UOHM` | pack DC resistance in the observer (`I = (OCV − V)/R`) |
| `X120X_CHG_FULL_DEBOUNCE_MS` | gauge-held-100 % time before the 100 % pin / 100 % charge-off (100 % targets only) |
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
