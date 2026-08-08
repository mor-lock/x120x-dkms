# State-of-charge model

The default SoC source (`--soc-source voltage`) does **not** trust the
on-board fuel gauge's SOC register directly. The boards ship a
MAX17043-*style* ModelGauge clone that is smooth but badly miscalibrated
near empty — it reads 0% while ~40% of usable charge remains (see the
field trace below). Instead the driver runs its own model and *fuses* the
gauge in only where the gauge is actually good.

There are four stages, all fed from one input: the cell terminal voltage
(the gauge's SOC register is used only as a shape reference).

```
 terminal V ─► OCV curve ─► IR correction ─► fusion ─► capacity (%)
                              ▲                 ▲
                power estimate ┘   raw gauge SOC ┘ ─► raw_capacity (%)
```

## 1. OCV curve (energy-true)

A single open-circuit-voltage → SoC table (`x120x_ocv[]`), calibrated so
that **SoC is linear in usable energy**: under a constant load the reading
falls at a constant %/hour. Anchors are 0% at 3.20 V (terminal, under
nominal drain) and 100% at 4.20 V. The table was built by fitting the
current pack's discharge shape and extending the bottom with a real deep
discharge, then normalised to the 3.20 V cutoff.

Energy scale: `ENERGY_FULL` is the *usable* window (rated ×
`X120X_USABLE_PERMILLE`, ~68 Wh for the 4×P50B pack), so `power_now =
E_full × dSoC/dt` reads true watts and SoC is linear in energy.

## 2. IR (load-sag) correction

The terminal voltage sags under load, so a bare lookup would read low
while discharging and jump at charge↔discharge transitions. The driver
estimates pack power from the GPIO regime (drain/charge/float) plus a
windowed `dSoC/dt`, converts it to an IR drop (`I·R`, `R =
X120X_R_UOHM`), and looks the curve up at the *compensated* OCV
(`V − I·R`). The power used for IR is slowly smoothed and independent of
the reported SoC, so there is no power→IR→SoC feedback loop. A fixed
nominal IR offset is used in the rate lookup so the estimate stays correct
on the steep top of the curve.

## 3. Sensor fusion (the key stage)

The IR-corrected voltage SoC (`v_soc`) is **accurate in absolute terms**
but *load-loose* where the curve is shallow (high SoC): a brief load
change moves the voltage a lot, so `v_soc` bounces on transients. The raw
MAX17043 ModelGauge (`g_soc`) is the opposite — a **smooth, load-immune
shape** with a **wrong absolute** that craters near empty.

So the driver takes the best of each, weighted by SoC:

- **High SoC** — report the gauge's shape, `g_soc + offset`, where
  `offset` is a slow EMA of `(v_soc − g_soc)` (τ ≈ 8.5 min). This keeps
  the gauge's smoothness but *our* calibrated absolute.
- **Low SoC** — report `v_soc` directly. The gauge has cratered here, and
  the voltage curve is tight (load-*insensitive*) and most accurate —
  which is exactly the region where the shutdown decision is made.
- The weight ramps linearly over `[X120X_FUSE_W_LO, X120X_FUSE_W_HI]`
  (40–55%) of `v_soc`, so the gauge is fully faded out before it can drag
  the critical low-SoC reading.

Because the gauge never bounces, the fusion removes the high-SoC
transient bounce *at the source* — there is no output slew, rate-limit or
final smoothing stage.

## 4. `raw_capacity` (diagnostics)

The raw ModelGauge SoC is exposed as a **non-standard** sysfs attribute
on the battery power_supply device:

```
/sys/class/power_supply/x120x-battery/raw_capacity   # 0-100 %, raw gauge
/sys/class/power_supply/x120x-battery/capacity       # 0-100 %, fused
```

It is added via `power_supply_config.attr_grp` (the power_supply core
manages its lifetime) and does not affect the standard property set or the
hwmon interface. Logging both alongside each other is the intended way to
validate the fusion on hardware.

## Why not just use the fuel gauge?

On a real deep discharge, the raw MAX17043 collapses to 0% at ~3.45 V
while the pack still delivers ~40% of its usable charge, then flatlines.
Driving shutdown off it would throw away a large fraction of runtime. The
voltage model tracks the true, linear discharge all the way to the 3.20 V
cutoff; the gauge is used only for its transient-rejection *shape* in the
high-SoC region, never for the absolute near empty.

## Tuning constants (in `src/x120x.c`)

| Constant | Meaning |
|---|---|
| `X120X_USABLE_PERMILLE` | usable-energy fraction of rated (SoC/energy scale) |
| `X120X_R_UOHM` | pack DC resistance for IR compensation |
| `X120X_FUSE_W_LO` / `_W_HI` | SoC band over which the gauge weight ramps 0→1 |
| `X120X_FUSE_OFF_SHIFT` | time constant of the voltage↔gauge offset EMA |
| `X120X_POWER_WINDOW_US` | window for the `dSoC/dt` power estimate |
