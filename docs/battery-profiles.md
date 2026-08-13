# Choosing a profile: runtime vs. longevity

Part of [x120x-dkms](../README.md).

**Quick answer — pick by how often the battery actually gets used:**

| Your setup | Profile |
|---|---|
| **Standby UPS** — runs on mains, discharges only during occasional outages | **`Fast`** (default) |
| **Frequently cycled (e.g. portable)** — runs off the battery away from mains and recharges when docked, cycling most days | **`Long Life`** |

Most x120x installs are the first case, so `Fast` is the default — and
on a standby UPS, limiting charge to 80% mostly just throws away backup
time.  But if the pack is cycled hard, `Long Life` can double or triple
its lifespan.  The rest of this section explains why the right answer
hinges entirely on cycling, and what each profile costs.

#### Two ways a cell wears out

Lithium-ion cells age by two largely independent mechanisms:

- **Calendar aging** — damage from *time spent* sitting at a high state
  of charge, regardless of use.  Runs faster the closer to full (and
  the warmer) the cell sits.
- **Cycle aging** — damage *per charge/discharge cycle*, concentrated at
  the voltage extremes, especially the top of the charge.

Which one dominates is set entirely by **how often you cycle**.  A
standby UPS cycles a few times a year, so calendar aging dominates.  A
portable build, run off the pack away from mains and recharged when
docked, may cycle daily, so cycle aging dominates.  Each profile targets
one of these — which is why the right choice flips depending on your use.

#### Standby UPS — why `Fast` wins

On a standby UPS, calendar aging dominates, and the usual "more capacity
now vs. longer life later" framing is incomplete.  It overlooks one
fact: in `Long Life` mode the battery does not only age more slowly — it
also **starts every outage a full 20 percentage points lower**.  Slower
aging has to first overcome that head start before it yields any extra
*usable runtime*, and for a UPS sized close to its actual backup needs,
it often never does within the life of the device.

The model below estimates **usable runtime at the start of an outage**,
as a function of years in service, for both profiles.  Usable runtime
is the energy available between the resting state of charge and the
2% shutdown floor — where the driver actually powers off — scaled by the
capacity the cells have retained to that point.

> **Assumptions (read these before trusting the numbers).**  These are
> a *model*, not measurements of your hardware, and the ranking is only
> as good as the assumptions behind it:
>
> - **Load:** ~5 W continuous (a Raspberry Pi 5 with NVMe at idle).
>   Runtime scales inversely with load — double the load, halve every
>   number.
> - **Pack:** an X1206 with 4× 21700 cells, fresh full→empty ≈ 7 h.
>   Runtime scales linearly with pack capacity; a smaller pack shifts
>   every row down proportionally but does **not** change the ranking.
> - **Shutdown floor:** 2% SoC — where the driver's UPower
>   `PercentageAction` fires the clean OS shutdown.
> - **Starting charge:** the X1206 has a small standby drain (~13 mW of
>   board/gauge quiescent draw on the battery rail, measured — see
>   [Measured: the standby sawtooth](#measured-the-standby-sawtooth)), so
>   a full pack slowly loses charge and tops back up — a shallow
>   sawtooth: `Fast` between 100% and 95% (recharging every ~9.5 days),
>   `Long Life` between 80% and 78% (a tighter band, so it tops up about
>   every ~5 days; at rest, a Fast-held pack relaxes to ~4.18 V).  The
>   table uses the top of each band; since the pack spends its time
>   evenly across the band, a typical outage starts ~mid-band (~2.5
>   points / ~0.15 h lower).
> - **Calendar aging:** assumed **3%/yr capacity loss at full charge
>   (~100% SoC)** and **2%/yr at 80% SoC**, at a moderate ~25 °C.  These
>   are illustrative midpoints from general Li-ion NMC literature,
>   **not** measured for any specific cell.  Real rates vary widely with
>   cell quality and, especially, **temperature** — calendar aging
>   roughly doubles per
>   +10 °C, so a pack running warm (e.g. in the Pi's exhaust) ages far
>   faster than this and *both* columns shrink.
> - **Cycle aging is small but not quite zero** — that standby sawtooth
>   is ~1.6–1.9 full-equivalent cycles per year, measured (see
>   [Measured: the standby sawtooth](#measured-the-standby-sawtooth)).
>   It adds marginally more wear under `Fast` (cycling at 95–100%, the
>   harshest region) than under `Long Life` (75–80%) — though both
>   profiles cycle about equally *often* — and it is dwarfed by the
>   calendar-aging difference above, so calendar aging still dominates.
>   A genuinely
>   cycled build (e.g. portable) is a different regime — see *Frequently
>   cycled builds* below.
> - Runtime is treated as proportional to the state-of-charge span,
>   ignoring the nonlinear "knee" near the bottom of the discharge curve.

**These rates assume moderate-quality NMC, not any particular cell.**
Cell quality shifts the result but rarely the ranking.  The "more
runtime" comparison is driven by *geometry* — `Long Life` gives up ~20%
of its usable span up front and must claw it back through slower aging —
so what matters is how fast the cells age relative to that handicap:

- **Premium cells** (e.g. Molicel) age slowly in absolute terms, so the
  year-0 runtime gap — which is pure starting charge, not aging —
  persists for longer.  `Fast` wins *more* decisively and the crossover
  pushes well past year 30.
- **Budget or hot-running cells** age fast, eroding both columns
  quickly and pulling the crossover in.  With genuinely poor cells (or
  a pack baking in the Pi's exhaust), `Long Life` can edge ahead in the
  early teens.

What would change the model itself is **chemistry, not brand**: the
numbers bake in NMC at 4.2 V.  LiFePO₄ cells have far flatter calendar
aging and much weaker sensitivity to storage charge, which shrinks the
`Long Life` benefit toward nothing and makes `Fast` win harder still.

Under the moderate-NMC assumptions (base case: 3%/yr vs 2%/yr):

| Years in service | `Fast` (~100%) | `Long Life` (hold 80%) | More runtime |
|---|---|---|---|
| 0  | 6.2 h | 4.9 h | `Fast` (+1.3 h) |
| 5  | 5.3 h | 4.5 h | `Fast` (+0.9 h) |
| 10 | 4.6 h | 4.0 h | `Fast` (+0.5 h) |
| 15 | 3.9 h | 3.6 h | `Fast` (+0.3 h) |
| 20 | 3.4 h | 3.3 h | `Fast` (+0.1 h) |
| 25 | 2.9 h | 3.0 h | `Long Life` (+0.1 h) |

(Runtime to the driver's 2% shutdown.  The year-0 Fast figure of 6.2 h
is the *measured* value from the [Incident 1](incidents.md#incident-1--deep-discharge-and-cell-destruction-2026-03-05) full discharge, not just a
model output; the later years scale it by assumed capacity retention.)

The counter-intuitive result: **`Fast` delivers more usable runtime
than `Long Life` for more than two decades.**  The lower starting charge
in `Long Life` costs ~1.3 h of runtime up front (~20% of the usable
span), and the slower aging does not repay that until the curves cross
at around year 22 — by which point the pack is well past a routine
replacement anyway.  The crossover is sensitive to the aging gap: if
`Long Life` ages much more slowly than assumed (e.g. 1.5%/yr) it pulls
in toward year 15; if the benefit is smaller (2.5%/yr) `Fast` wins past
year 40.  In none of these cases does `Long Life` win at year 10.

What `Long Life` *does* buy is **capacity retention**, not runtime: at
year 10 the 80%-held pack retains ~82% of its original capacity versus
~74% for the full-charge (`Fast`) pack.  That defers the eventual
*replacement*; it does not give you a longer outage on any given day.

So on a UPS the two profiles trade off cleanly: `Fast` wins on
**runtime** for the realistic life of the device, while `Long Life` only
wins on **capacity retention** — worthwhile if the pack is oversized
relative to your worst outage, or if postponing the eventual (cheap)
replacement matters more than per-outage runtime.  For most standby
installs that is not a good trade, which is why `Fast` is the default.

#### Measured: the standby sawtooth

The model above leans on an assumption — that the standby sawtooth is
worth only a few full-equivalent cycles a year.  That assumption has now
been measured, on the maintainer's own X1206 (4× 21700,
`--battery-mah 20000`) in `Fast` mode, from a minute-resolution log:

| Quantity | Measured |
|---|---|
| Sawtooth band | 100% → 95.9% (≈4.1 points by voltage) |
| Time between recharges | **9.5 days** |
| Recharges per year | ~38 |
| **Full-equivalent cycles per year** | **~1.6–1.9** |
| Time-weighted mean state of charge | 98.7% |
| Standby drain, averaged over a full cycle | ~0.43%/day (≈13 mW at the pack) |

At that rate a pack that spends its entire life on this UPS accumulates
roughly **40–50 full-equivalent cycles in 25 years** — under 10% of the
several-hundred-cycle rating of any reasonable NMC cell.  On a standby
UPS, cycle aging is not merely "dominated by" calendar aging; it is
close to irrelevant.

**This does not favour either profile.**  `Long Life` cycles a narrower
band (80% → 78%), so it tops up more often than `Fast` — every ~5 days
rather than ~11 by nominal-band arithmetic (`Fast`'s measured sawtooth is
9.5 days) — but each top-up is shallower (2% vs 5%), and the same
quiescent drain moves the same charge per year either way: still ~1.6
full-equivalent cycles.  So cycle aging stays essentially equal; the
profiles differ almost entirely in **mean state of charge** (98.7% vs
~79%), which is a calendar-aging term, not a cycle-count one.  The `Fast` sawtooth does sit in the harsher 95–100%
region, so each of its cycles costs a little more — but there are so few
of them that the difference stays in the noise.  The measurement
therefore *supports* the ranking above rather than changing it.

> **Caveats — this is one data point, not a characterisation.**  One
> pack, one board, two complete cycles observed.  State of charge here is
> derived from resting voltage (3.3 V = 0%, 4.2 V = 100% — the logging
> daemon's linear map, distinct from the driver's own 3.20 V OCV scale),
> not coulomb-counted, and that mapping is compressed at the top of the
> curve: the pack *appears* to hold 100% for ~2.5 days after each
> recharge and then fall at ~0.58%/day, but that shape is an artifact of
> the voltage map, not a real pause.  The robust figure is the **9.5-day
> recharge period**; every rate derived from it inherits the mapping's
> error.  An earlier estimate of ~0.7%/day for this same pack came from
> timing only the visible decline, which excludes that flat top and so
> overstates the drain — the full-cycle average is ~0.43%/day.

#### Frequently cycled builds (e.g. portable) — why `Long Life` wins

The exception is a Pi that genuinely cycles the pack often — most
realistically a **portable build**, where the unit runs off the battery
away from mains and is recharged whenever it is docked.  A pack cycled
most days is a different regime entirely: cycle aging now dominates, and
it is heavily concentrated at the **top of the charge**.  Taking an NMC
cell all the way to 4.2 V means:

- the cathode is fully delithiated and under maximum lattice strain, so
  it micro-cracks a little more each cycle;
- electrolyte oxidation accelerates sharply above ~4.0–4.1 V, growing
  resistive films on the cathode; and
- the graphite anode is fully lithiated, raising the risk of lithium
  plating (permanent capacity loss), worst when charging fast or cold.

`Long Life` stops before that zone, and because cycle life is strongly
nonlinear in the charge window, trimming the top buys a lot:

| Charge ceiling | Approx. cycle life (to 80% capacity) |
|---|---|
| 4.2 V (100%) | baseline (1×) |
| 4.1 V (~90%) | ~1.5–2× |
| 4.0 V (~80%, `Long Life` default) | **~2–3×** |
| 3.9 V (~70%) | ~3–4× |

(Approximate NMC figures; exact numbers vary by cell.)  So a pack that
cycles daily can last **two to three times as many cycles** before it
wears out — a benefit that lands immediately and compounds on every
cycle, not the decades-away payoff of the calendar case.  This is the
same reason laptops, phones and EVs cap charging at 80% by default: they
are battery-cycling devices, not standby reserves.

The trade is still real — `Long Life` gives up ~20% of per-charge
runtime — but here you are paying it to roughly triple the pack's
lifespan, rather than (as on a standby UPS) getting almost nothing back.
And because the charge mode can be switched at runtime, a portable user
can flip to `Fast` before a long outing when full capacity is needed,
then back to `Long Life` for everyday use.

#### Both profiles already avoid the worst stressor

Whichever you choose, **both** profiles disable the charger once the
pack reaches its ceiling rather than holding it on a continuous float —
so both avoid the single worst calendar-aging stressor (sitting pinned
at 4.2 V indefinitely).  The only difference between them is the resting
state of charge.  If you are unsure, you almost certainly have a standby
UPS: leave it on the default `Fast`, keep the cells cool, never let them
deep-discharge (see [Incident 1](incidents.md#incident-1--deep-discharge-and-cell-destruction-2026-03-05)), and a replacement — if ever needed —
is cheap and infrequent.

#### Measured runtime, and what 80% actually costs a UPS

Back to the standby-UPS numbers: the model's year-0 row is not just
theory — it matches a real full-depth discharge logged during the
[Incident 1](incidents.md#incident-1--deep-discharge-and-cell-destruction-2026-03-05) outage (before the undervoltage shutdown existed, so the
pack drained all the way down).  On an X1206 (4× 21700) at ~5 W idle
load, from a full start:

| Milestone | Time on battery |
|---|---|
| Down to 50% | ~4.2 h |
| 10% — driver flags `capacity_level=Low` | ~5.9 h |
| **2% — clean OS shutdown fires** | **~6.2 h** |
| 0% — fully empty (no shutdown was in place) | ~7.0 h |

([Incident 1](incidents.md#incident-1--deep-discharge-and-cell-destruction-2026-03-05)'s total 10.3 h on battery includes ~3.3 h spent below
fuel-gauge 0%, after the 7.0 h in the table above ends.)

Note the curve: the first half drains slowly on the flat part of the
discharge (~4 h to 50%), then collapses — the bottom half is gone in
about two hours.  The driver's clean shutdown fires at 2% SoC (~6.2 h
here); the 10% `Low`-battery warning lands at ~5.9 h, only ~18 minutes
earlier, because the bottom drains so fast.  The lesson: most of the
runtime is in the top half, so starting lower hurts disproportionately.

Because `Long Life` begins every outage at 80% instead of ~100%, it
enters that drain a full 20 points down and reaches the 2% shutdown well
over an hour sooner — **~4.9 h instead of ~6.2 h**, roughly 1.3 h less
ride-through, immediately, on every outage.  And as the table above
shows, that lost time is not repaid by slower aging until ~year 22.  So
on a pack sized close to its job (here ~6 h against typical 2–5 h
outages), limiting to 80% sheds backup time you are actually using, with
no practical payback — which is exactly why `Fast` is the default.

#### Does Long Life give more outage runtime over time?

It is sometimes assumed that over years of always-on operation Long Life
ends up giving *more* outage runtime — the idea being that its cells age
less, so the retained capacity offsets the lower starting charge.  For
the realistic life of a UPS this is **not** true.  Long Life also starts
every outage a full 20 points lower, and that head start is not repaid by
slower aging until well over two decades in; until then a 100%-held
(`Fast`) battery delivers more usable runtime despite aging faster (see
the year-by-year model above).  On a standby UPS, Long Life's real
benefit is **deferred cell replacement, not better outage protection** —
which is why `Fast` is the default.
