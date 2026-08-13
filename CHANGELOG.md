# Changelog

Release history of [x120x-dkms](README.md), newest first.

### Unreleased — v0.5.x voltage-observer series

A single **unreleased** development series; the last tagged release is v0.4.8.
The v0.5.x SoC work was developed 2026-08-06…08-11 while the version string
still read 0.4.8. **v0.5.0** and **v0.5.1** were exploratory dead ends —
voltage-derivative (dSoC/dt) power estimation, then a voltage↔gauge fusion
blend — neither stamped in `dkms.conf`, both superseded once the recursive
observer emerged as the far cleaner idea at **v0.5.2** (its dead code was
removed in v0.5.4). The first version stamp is therefore 0.5.2. Broken out
below by version bump, newest first; at tag time these reconcile into one
release section per the versioning convention.

#### v0.5.11 — hwmon: observer OCV channel + voltage safety limits

**Kernel driver**
- New hwmon **`in1`** channel (label `cell_ocv`): the voltage model's
  rested open-circuit-voltage estimate at the current SoC, exposed as a
  second standard `in` channel. `in0 − in1` is therefore the live IR drop.
  Only the observer produces this, so `in1` is present under the default
  `--soc-source voltage` and hidden under `soc_source=gauge`. `sensors`,
  node_exporter (`node_hwmon_in_volts{sensor="in1"}`), collectd, etc. pick
  it up with no configuration.
- `in0` now carries standard voltage safety limits: **`in0_lcrit`** (3.00 V,
  the on-battery critical floor), **`in0_min`** (3.20 V, design minimum /
  0 % SoC), and **`in0_lcrit_alarm`**, which reads `1` once the pack has
  held at the critical floor long enough to force `CAPACITY_LEVEL=CRITICAL`
  — so `sensors` renders `ALARM` exactly when the shutdown chain arms.
- The observer's OCV(SoC) estimate is now cached (`ocv_model_uv`) and the
  hwmon `power1` note corrected: under the voltage model `power1` is the
  observer's net `P = I·V`, not the gauge-mode dSoC/dt derivation.
- No ABI change to existing channels; `in0_input`, `curr1`, `power1`,
  `energy1` are untouched.

**Documentation**
- README hwmon section and the `src/x120x.c` interface comment document the
  new `in1` channel, the `in0` limits/alarm, and the node_exporter series.

#### v0.5.10 — Exact fractional charge band

**Kernel driver**
- Charge band thresholds now compared at **1/256-% (fractional)** rather than
  the floored integer capacity. The rising cut edge (`≥ end`) already landed on
  the true threshold, but the falling resume (`≤ start`) fired ~1% high
  (`floor(78.9)=78`, so a `78` resume triggered at true ~79) — a Long Life
  78/80 band, left to the driver, actually floated 79–80. Both edges now hit
  the true SoC, giving an exact `[start, end]` band. Applies to the Long Life
  band (fractional observer `soc256`) and the Fast float band (the raw gauge
  at 1/256-%, now tracked internally). No change to the rising cut. (Same floor
  off-by-one that made the drain-to-5% test stop at true ~6%.)

**Documentation**
- README: the "native Linux integration" summary paragraph moved from under the
  intro to just below the v0.5.x runtime graph, directly above the battery-tray
  screenshot (placement only, text unchanged).

#### v0.5.9 — Long Life default 78/80

**Kernel driver**
- Long Life default band tightened **75/80 → 78/80** (`conservation_start`
  75 → 78; end stays 80). Under the board's ~0.43%/day standby drain both bands
  see the same annual charge throughput (~1.6 EFC/yr), so cycle aging is
  unchanged; 78/80 just keeps the pack a touch nearer full (mean ~79% vs
  ~77.5%) for slightly more backup runtime, topping up ~every 5 days instead of
  ~11 (shallower, so no extra wear). Fast mode is unchanged (95/100). Wider
  bands (e.g. TLP's 75/80) remain one modprobe/sysfs write away. Updates the
  `module_param` default + desc + validator fallback.

**Documentation**
- Every doc that stated 75 updated: README (band description, sysfs listings,
  example, params table, and the TLP justification — reframed as the wider
  alternative rather than the default), `battery-profiles.md`, `migration.md`,
  and `install.sh --help`.

#### v0.5.8 — Charger-off settle seed on boot

**Kernel driver**
- Cold-boot/reload seed reworked to a **charger-off settle** (supersedes the
  v0.5.2 nominal-IR seed). On the first sample the driver holds the charger off
  for `X120X_SEED_SETTLE_MS` (5 min) and re-seeds SoC from the OCV curve each
  poll. On grid the held-off pack rests, so the terminal relaxes to true OCV
  and the seed converges to the real SoC — no IR guess, and a wrong seed can no
  longer trip the charger on. The old seed assumed charging whenever on grid
  and, on a rested-but-not-full pack, under-read badly (a rested 86% pack
  seeded ~72%: near the top the OCV curve is ~4 mV/%, so a small IR offset maps
  to a large SoC swing). During the settle reported power is 0 (no phantom
  re-anchor blip). Two shortcuts skip the settle: a full pack on a 100% target
  (gauge=100) pins to full; a near-empty pack (`V ≤ X120X_SEED_EMPTY_UV`,
  ~3.1 V) seeds 0% and charges at once. Removes the now-unused
  `X120X_SEED_CHG_UW`; off-grid boots still IR-correct via `X120X_SEED_DIS_UW`.

#### v0.5.7 — Gate the gauge=100 pin on a 100% target

**Kernel driver**
- The observer's gauge=100 pin (hard-anchor energy to `E_full`) now also
  requires a 100% charge target — Fast mode, or a Long Life band configured to
  100%. The gauge only reads 100 at a genuinely full pack, so in normal cycling
  this changes nothing; the gate is defensive — switching a *full* pack into a
  sub-100 Long Life band no longer pins the observer at full while the
  still-100 laggy gauge holds it there as the pack self-discharges toward the
  band. The cold-boot seed (a genuinely full pack at boot) is intentionally
  left ungated — full is full in any mode.

#### v0.5.6 — Scope the full-charge debounce to 100% targets

**Kernel driver**
- Full-charge debounce now applies **only to a 100% stop target**, and is
  lengthened 30 min → 1 h (`X120X_CHG_FULL_DEBOUNCE_MS`). At a 100% target —
  Fast mode, or a Long Life band configured to 100% — the gauge=100% assertion
  gates both charge-off and the observer 100% pin after 1 h of held-full, keyed
  on the raw gauge (the only reliable read at full, since the observer only
  approaches full asymptotically). Cost is ~50 min extra CV hold per top-off —
  negligible for a UPS that floats near full.
- Sub-100 Long Life bands now **cut the charger immediately** at
  `conservation_end`, keyed on the observer SoC, instead of waiting out the
  debounce. The debounce is a CV top-off tool for the 100% anchor; applying it
  to an 80% band let the charger run the full window past the target and
  overshoot the band top by ~5% (a 75–80 band effectively topped out ~85%, then
  had to self-discharge all the way back before the first recharge). A
  mid-range band has nothing to top off, so it stops on the spot. The Long-Life
  soak run surfaced this exactly.

#### v0.5.5 — Two-branch OCV tables + full documentation pass

**Fuel gauge / state of charge**
- Two-branch (charge/discharge) OCV tables replacing the single curve. NMC has
  a real OCV hysteresis — at a given SoC the rested voltage is higher just after
  charging than just after discharging (~115 mV low, → ~0 above 80%). The
  observer selects the charge-branch table while charging (`ac_online &&
  !charger_inhibited`) and the discharge branch on battery/at rest, so the
  estimate no longer front-loads on the charge leg (a single mean curve read the
  charge current ~half a hysteresis too high at low SoC). `energy_now` stays
  continuous across a branch flip, so SoC never steps — only the rate does.
  Both tables are energy-true rested OCV from an ECM characterization cycle,
  56 points each. Validated against an independent coulomb reference over 5 days
  of logged telemetry: RMS 3.75 % / max 9.8 % vs truth, beating the previous
  single-curve (4.73 % / 17.2 %) and the raw gauge (11.1 % / 27.4 %).
  NMC-calibrated; LFP unsupported.
- Usable-energy scale `X120X_USABLE_PERMILLE` 875 → 900 (e_full 63.0 → 64.8 Wh
  for the 4×P50B pack). The coulomb-measured usable window (4.20 → 3.20 V) came
  out at 63–65.4 Wh across metered cycles; 0.900 is the mid-range value. Only
  scales the transient power/rate — the OCV feedback self-anchors steady SoC —
  so it is not a precision knob.
- Voltage EMA rounds to nearest (+16 half-LSB) instead of truncating, so it
  settles on the true voltage from below (was biasing up to ~31 µV low).
  Sub-LSB against the ~1250 µV ADC step — a correctness tidy-up, no measurable
  accuracy change.

**Documentation**
- SoC docs rewritten for the v0.5.x observer model (`docs/soc-model.md`); the
  annotated recovered-capacity drain+recharge figure and the 2026-03-05
  deep-discharge/destruction figure added (`docs/incidents.md`); README intro
  banner announcing the voltage-observer SoC model up top with the runtime
  graph.
- SoC is *linear in energy* (`energy_now / E_full`, not remaining coulombs)
  surfaced as a first-class, user-visible property: under constant power it
  falls at a constant %/hour, so "50% ≈ half the runtime left" holds at steady
  load and UPower's time-to-empty stays trustworthy near empty; a coulomb-based
  SoC would bend downward instead.
- `soc-model.md` clarifies that reported power is **net, not gross** under load
  (the observer reports net battery power — charge minus Pi load — since the Pi
  shares the battery node), and addresses whether deeper discharge is safe for
  the cells.
- Documented the SoC-model calibration envelope: NMC/NCA at ~room temperature,
  no temperature sensor / no temperature compensation. Because SoC is
  voltage-anchored and shutdown is a fixed voltage floor, out-of-range
  temperature degrades the estimate *gracefully and never unsafely*.
- README: fixed the sysfs `charge_full`/`energy_full` descriptions (usable
  window vs `*_design` rated to 2.5 V), added the `soc_source` module-param and
  `--soc-source` install-option rows, reworded the board-table note to "not yet
  installable", and documented the per-change lockstep versioning convention.
- Version lockstep reconciled to 0.5.5 across `dkms.conf`, `install.sh`,
  `src/x120x.c`, and `docs/manual-install.md`.

#### v0.5.4 — Remove dead code; lengthen the debounce

**Kernel driver**
- Removed dead code superseded by the recursive observer: the entire unused
  v_soc↔gauge **fusion** subsystem (`fusion_off_256`, `fusion_primed`,
  `FUSE_W_LO/W_HI/OFF_SHIFT` and its doc block), the abandoned live-dSoC/dt
  power estimator (`ocv_slow_uv`, `rate_prev_soc256`, `rate_windows`,
  `learned_charge_uw`, `learned_drain_uw`, `prev_regime`, `power_primed`,
  `ir_power_uw`, `enum x120x_regime`, `POWER_WINDOW_US`, `OCV_SLOW_SHIFT`,
  `SEED_CHARGE/DRAIN/FLOAT_UW`, `IR_NOM_DRAIN/CHARGE_UV`), and the write-only
  `soc_offset`/`prev_charging`/`model_primed` fields. No functional change —
  none were read; the observer's `P = I·V` and gauge=100 pin are the live path.
- Full-charge debounce lengthened 10 min → 30 min: on the flat CV top the
  observer's charge current `(OCV−V)/R → 0` as SoC → 100 (τ ≈ 11 min), so
  waiting ~3τ lets the pack integrate to ~99.7% before the anchor, shrinking the
  100%-pin snap from ~1.6% to ~0.3%. (Later scoped to 100%-only and 1 h in
  v0.5.6.)

#### v0.5.3 — Hard terminal-voltage floor

**Kernel driver**
- Hard terminal-voltage floor (SoC-independent safety backstop): on battery, a
  raw cell voltage ≤ 3.00 V held for 20 s forces `CAPACITY_LEVEL=CRITICAL`
  regardless of the SoC estimate, driving the OS (UPower/logind) shutdown chain.
  The last-ditch defence if the observer ever reads high while the pack is
  genuinely empty — voltage is ground truth. The 20 s confirm (mirrors the
  dead-battery detector) rejects transient load-spike sags; gated on battery
  (never fires while charging).

#### v0.5.2 — Recursive voltage-observer SoC model

The current-sensorless observer the README announces as the v0.5.x model, and
where the design finally clicked: after the v0.5.0 voltage-derivative and v0.5.1
fusion experiments, the recursive energy-integrating observer emerged as a far
superior idea and replaced both. This is the first version-stamped 0.5.x
release.

**Fuel gauge / state of charge**
- Recursive energy-integrating voltage observer replaces the instantaneous
  IR-lookup + gauge-fusion SoC path: `SoC = energy_now/energy_full`,
  `I = (OCV(SoC)−V̄)/R`, `P = I·V̄`, `energy_now −= P·dt`. Self-anchoring,
  load-independent, with signed battery power for free.
- New `soc_source` module param (default `voltage`) derives SoC from cell
  voltage via an NMC open-circuit-voltage model instead of the fuel gauge's SOC
  register. The gauge on these boards is a MAX17043-style clone (version reports
  `0x000`, non-datasheet register map, boots reporting an impossible 102%) whose
  SoC register over-reads the discharge near full — a *dynamic*
  stuck-then-catch-up artifact. Validated against a coulomb-counted discharge,
  the model read 90% where the gauge read 86.6% and truth was ~90%. Two curves
  — charge (on grid) and discharge (on battery) — because charge current lifts
  terminal voltage ~100–150 mV above the discharge OCV; both empirical, measured
  on the 4×P50B pack across 216 days, with a decaying offset re-anchoring SoC at
  each charge↔discharge transition so it never jumps. `soc_source=gauge`
  restores the raw register behaviour. (The empirical curves were refined into
  the energy-true ECM two-branch tables in v0.5.5.)
- Cold-boot SoC seed IR-corrected by a nominal load (5 W drain, 10 W charge)
  instead of the load-depressed/charge-lifted terminal, so a boot mid-cycle
  starts near truth. (Replaced by the charger-off settle seed in v0.5.8.)
- Gauge=100 observer pin: while on grid, once the raw MAX17043 gauge has read
  100% for 10 min (or reads 100% at driver start), the observer's energy is
  hard-anchored to full. Handles the common float-restart and prevents slow
  integrator drift over long floats; gated on AC so it releases the instant the
  grid drops (the laggy gauge keeps reading 100 for minutes after a cut). The
  gauge is trusted only for this `==100` full-assertion; the discharge/steady
  path stays voltage-only. (Debounce later 30 min → 1 h and 100%-gated across
  v0.5.4/0.5.6/0.5.7.)
- Charge-off debounce raised 45 s → 10 min: Fast mode keeps charging for 10 min
  after the gauge first reads 100% (CV top-off) before cutting the charger, so
  "full" is genuine before the pin and termination fire.
- `POWER_NOW` is no longer output-smoothed for the voltage observer: it was
  passed through an α=1/1024 EMA (τ ≈ 8.5 min) that lagged real load steps by
  ~29 min. The observer's rate is already V-EMA-smoothed (τ ≈ 16 s) and the
  gauge supplies its own smoothness, so `POWER_NOW` is served directly again
  (the v0.4.8 behavior).
- Charge status reports **Not charging** whenever the charger is inhibited (both
  modes), and Fast-mode charge termination inhibits on the raw gauge, debounced.

**Installer**
- New `--soc-source voltage|gauge` flag (default `voltage`), resolved with the
  same flag > existing-conf > default precedence as the other options and
  written to `/etc/modprobe.d/x120x.conf`.
- Fixed a latent `set -euo pipefail` abort: reading a config that lacks any key
  (e.g. a pre-`board` conf on upgrade) aborted the installer because a no-match
  `grep` in the per-key extraction propagated failure. All four extractions now
  tolerate a missing key.
- Ubuntu for Raspberry Pi is now supported. `install.sh` detected the firmware
  partition at `/boot/firmware` but hardcoded the overlays subdirectory as
  `overlays/`, whereas Ubuntu's flash-kernel layout keeps the active kernel's
  overlays under `current/overlays/`. The installer now probes for
  `current/overlays` (a Debian/Ubuntu-only convention, never present on
  Raspberry Pi OS) and falls back to `overlays`, so the overlay lands where the
  bootloader reads it on both distributions. Reported by @AbbynatorNZ on
  Ubuntu 26.04 LTS / Pi 5 / X1201 V1.1 (#5).

**Documentation**
- README now shows the driver in use: screenshots of the battery icon in the
  panel tray, `upower -i` output, and the GNOME Power Statistics window
  (`docs/images/`, captured on the maintainer's Pi 5 / X1206).

### v0.4.8 — Update-safe install, uninstall --help, kernel floor, docs completion

**CI**
- New `check-versions` step: `tools/check-versions.sh` verifies the
  version string agrees between `dkms.conf`, `install.sh`,
  `src/x120x.c`, and every hardcoded ref in
  `docs/manual-install.md` — previously enforced only by the
  RELEASING.md checklist.  A missing version (refactored away) fails
  rather than silently passing; CHANGELOG/README mentions are
  historical records and deliberately out of scope.

**Installer**
- Re-running `install.sh` now preserves the existing configuration:
  each of `battery_mah`, `conservation_mode_default`, and `board` is
  resolved as CLI flag > value from the existing
  `/etc/modprobe.d/x120x.conf` > default, independently, with the
  effective source shown in the install output.  Previously the
  natural update command — `git pull && sudo bash install.sh` —
  silently reset pack capacity to 1000 mAh and a persisted Long Life
  mode back to Fast.  Invalid parsed values warn and fall back to the
  default for that setting only; a first install (no conf) is
  unchanged.
- `uninstall.sh` now parses arguments: `--help` / `-h` prints what the
  script removes and what it leaves alone (works without root), and an
  unknown option aborts before any uninstall step.  Previously every
  argument — including `--help` — was silently ignored and a full
  uninstall proceeded; the cautious user asking "what will this
  remove?" was the one who got bitten.
- `install.sh` pre-flight now checks the running kernel against the
  6.3 floor (numeric major.minor comparison, `X120X_UNAME_R`
  overridable for tests) and fails with the requirement and the fix —
  a fully-updated Raspberry Pi OS Bookworm or later — instead of a
  screenful of DKMS compiler errors on old images.  An unparseable
  version string warns and continues.

**Documentation**
- Getting started and the `ac_online` troubleshooting entry now state
  that the power supply connects to the UPS board's own power input,
  not the Pi's USB-C port — a charger feeding the Pi directly keeps
  the Pi running but never charges the battery or asserts AC
  detection.
- The manual-installation walkthrough now covers everything the
  installer does: new steps for the modprobe battery configuration
  (without it every capacity figure is computed against the 1000 mAh
  default), the `gpio=6=pu` pull-up in config.txt, the four UPower
  settings, and a charge-mode-persistence note; the `PercentageAction`
  reference no longer points at "what the installer sets".  New
  "Updating" section in the README: `git pull && sudo bash install.sh`
  now keeps previous settings (see the installer entry above).
- Second README split: the manual-installation walkthrough moved to
  [docs/manual-install.md](docs/manual-install.md) and the
  GPIO-scripts migration guide to [docs/migration.md](docs/migration.md)
  (verbatim, one-time-audience material; ~250 lines).  The README
  headings remain as pointer stubs so existing deep links keep
  resolving.

### v0.4.7 — Review fixes, logind drop-in, docs restructure, CI expansion

**Driver**
- `set_property` emits a synchronous `power_supply_changed()` on the
  charger after a `charge_type` write (outside `chip->lock`), so the udev
  charge-mode persistence runs on the write itself rather than waiting
  for the next poll/heartbeat cycle to emit the charger uevent.
- When the charge-control GPIO is absent from the device tree, the
  driver now demotes itself to a no-charge-control board: `charge_type`
  and both `charge_control_*_threshold` files become read-only and a
  `Long Life` write is rejected with `EOPNOTSUPP`.  Previously the write
  was accepted and read back as `Long Life` while the poll loop —
  which gates on the GPIO descriptor — silently enforced nothing,
  contradicting the probe warning that promised read-only behaviour.
  Threshold writes are likewise rejected on boards without charge
  control instead of being accepted and ignored.
- `conservation_start` / `conservation_end` module parameters are now
  validated at probe against the same rules as the sysfs store paths
  (start 0–99, end 1–100, start < end), falling back to the 75/80
  defaults with a warning.  Previously e.g. `conservation_end=200` in
  `/etc/modprobe.d` loaded silently and the stop threshold never
  triggered.  Mirrors the existing `battery_mah` probe-time clamp.
- Full kernel-doc coverage: every function now carries a validated
  kernel-doc comment (22 blocks, up from 3 — the struct and the two
  register helpers).  `scripts/kernel-doc -Wall -none` reports zero
  warnings; note that CI's `W=1` check only validates blocks that
  exist, so coverage itself is not CI-enforced.
- `MODULE_LICENSE` changed from `"GPL v2"` to the canonical `"GPL"`,
  deferring to the SPDX `GPL-2.0-or-later` headers as the authoritative
  license statement (the README's License section now says the same —
  the three previously disagreed).

**Installer**
- Low-battery shutdown is now configured through a drop-in file,
  `/etc/systemd/logind.conf.d/90-x120x.conf`, instead of appending a
  marker block to `/etc/systemd/logind.conf`.  The packaged file stays
  pristine — dpkg never sees it as modified, so a systemd upgrade
  never raises a conffile prompt over the driver's edit — and
  uninstall of the setting is a plain `rm`.  Reinstalling migrates a
  pre-drop-in system automatically (the old marker block and any
  pre-marker bare lines are removed from `logind.conf`).  UPower has
  no drop-in mechanism, so `UPower.conf` keeps the marker block.
- `uninstall.sh` now `systemctl try-restart`s systemd-logind after
  removing the drop-in (or, on pre-drop-in systems, the marker block
  from `logind.conf`), so the removed `HandleLowBattery` setting
  stops applying immediately instead of at the next reboot.
- The DKMS tree copy is now an explicit allowlist (`dkms.conf`,
  `Makefile`, `LICENSE`, `src/x120x.c`, `src/Kbuild`) instead of
  `cp -r` of the whole checkout, which dragged `.git` and the
  documentation into `/usr/src` on every install and could pick up
  stray build artifacts from a developer tree.
- `install_ini_block` no longer accumulates a leading blank line before
  its marker block on reinstall — a repeat install is now byte-identical.
- `--charge-mode` and `--board` with a missing value now die with a
  clean usage error instead of crashing with bash's unbound-variable
  message under `set -u` (`--battery-mah` already handled this; the
  test suite now covers all three).
- `--board` variants other than `x120x` are refused with an explanation:
  no per-board device tree overlay ships yet, so the power-off pulse
  those boards require after shutdown cannot work and the UPS would
  drain the pack indefinitely after `poweroff`.  Previously the
  installer proceeded and produced exactly that setup.  The README's
  [Experimental board support](README.md#experimental-board-support) section now
  documents the manual development path instead.

**Documentation**
- Getting started restructured for first-time users (newbie-first
  requirements, commands in execution order); charge-mode selection
  moved to [Battery conservation mode](README.md#battery-conservation-mode).
- Section cross-references converted from italics to in-page GitHub
  anchor links.
- Note that `charge_control_*_threshold` reports the Long Life band even
  in `Fast` mode (Fast uses a fixed 100% / 95% band the standard sysfs
  interface cannot express), so `75` / `80` there in Fast is expected.
- New [Measured: the standby sawtooth](docs/battery-profiles.md#measured-the-standby-sawtooth):
  the standby sawtooth is now measured rather than assumed — 9.5 days
  between recharges, ~38 recharges and ~1.6–1.9 full-equivalent cycles
  per year, ~0.43%/day drain (≈13 mW).  It confirms the profile ranking:
  cycle aging is close to irrelevant on a standby UPS, and both profiles
  cycle about equally often, so they differ almost entirely in *mean*
  state of charge (98.7% vs ~77.5%) — a calendar-aging term.  Corrects
  the earlier "roughly weekly" and "~20 mW" estimates, which timed only
  the visible decline and so excluded the flat top of the voltage curve.
- New "Tested hardware" matrix (one confirmed row) inviting reports via
  the hardware-report issue template.
- Requirements now state the minimum kernel (6.3+), surfaced by the
  `build-lts` CI job when 5.15 failed to compile.

**CI**
- Module compile-checks (`KCFLAGS=-Werror`, never loaded): a `build`
  matrix against generic and newest-HWE headers, a `build-lts` job
  against an older *supported* LTS kernel (Ubuntu 24.04, 6.8) in a
  container, and a `build-armhf` job that verifies the 32-bit cross
  toolchain (a real armhf build needs an armhf kernel tree, reported by
  users).  New `overlay` job compiles `x120x-overlay.dts` with `dtc`.
- New `static` job runs `make W=1` (kernel extra-warnings and kernel-doc,
  any warning fails) and `sparse` (`make C=1`) over the driver.
- New `checkpatch` job runs `checkpatch.pl --no-tree` (fetched pinned
  to a kernel tag from GitHub's mirror of torvalds/linux) over
  `src/x120x.c`, with three
  documented house-style ignores (`BLOCK_COMMENT_STYLE`,
  `SPLIT_STRING`, `DEEP_INDENTATION`) to drop before an upstream
  submission.  The three findings outside those classes were fixed:
  a `static ... = 0` initialiser and two missing blank lines after
  declarations — no behaviour change.
- New `binding` job validates `suptronics,x120x.yaml` against the
  dtschema meta-schema with `dt-doc-validate`.
- New `build-rpi` job cross-compiles the module (arm64,
  `bcm2712_defconfig`, `-Werror`) against a shallow clone of
  raspberrypi/linux `rpi-6.12.y` — the kernel Raspberry Pi OS
  actually ships — catching RPi-tree divergence the Ubuntu-header
  builds cannot see.
- Shell job now also verifies every README in-page anchor link resolves
  to a heading (GitHub slug rules), next to the repository-layout tree
  check.

**Project**
- Issue templates: a bug report mirroring the Troubleshooting checklist,
  and a hardware test report for experimental boards (X728 / X708 /
  X729) and armhf builds; blank issues stay enabled.  Dependabot keeps
  the pinned GitHub Actions fresh weekly.
- `SECURITY.md` (private vulnerability reporting, latest-release support,
  best-effort response) and `RELEASING.md` (the on-hardware release
  checklist codified from the v0.4.6 process).

**Tooling**
- `tools/collect-debug.sh` — a no-root one-shot diagnostics collector
  (Pi model, OS, kernel, `dkms status`, `dmesg`, power_supply devices,
  sysfs values, module params, `modprobe.d`) into one paste-ready block;
  runs cleanly whether or not the driver is loaded.  Referenced from
  Troubleshooting and the bug template, with a test covering the
  driver-present and driver-absent paths.

**Build**
- New `make test` target runs the whole shell suite (documented in the
  Testing section).

### v0.4.6 — Automatic Pi 5 bootloader configuration, troubleshooting guide

**Installer**
- `install.sh` now stages the two required Pi 5 bootloader EEPROM
  settings (`POWER_OFF_ON_HALT=1`, `PSU_MAX_CURRENT=5000`) itself, rather
  than only warning if they were missing.  The read-modify-apply is
  idempotent (no EEPROM write when both are already correct), rewrites a
  differing prior value with no special case, and leaves every other key
  untouched.  `--apply` only stages the update; the bootloader flashes it
  at the next boot, so it lands with the reboot the installer requests.
  A successful-but-empty config read is guarded so it can never stage a
  config that wipes the rest of the EEPROM.  `--skip-eeprom` opts out.
- Temp files are tracked in a single cleanup list removed by one EXIT
  trap, so nothing leaks if the script is killed mid-run.

**Documentation**
- Getting started restructured around the automatic bootloader setup:
  two steps (install, monitor) with a single reboot, a per-board
  install-command table for every supported board (a real capacity
  where the cells are fixed, `<your_capacity>` only for external-pack
  boards), and an OS-requirements note.
- New Troubleshooting section (symptom → check → fix) for the common
  novice failure modes.
- `## Required bootloader settings (Raspberry Pi 5)` is now the single
  canonical reference for the settings, what they do, their caveats, and
  the manual one-liner.
- Intro reframed as an independent community project — not affiliated
  with or endorsed by SupTronics/Geekworm, though linked from Geekworm's
  wiki pages.
- New Testing section documenting how to run the suite locally.

**Tests**
- New unprivileged shell test suite under `tests/` — functions
  sed-extracted, externals mocked, assertions on files and logs:
- `test-install.sh` — `configure_bootloader()` against a mocked
  `rpi-eeprom-config` and device-tree model path.
- `test-ini-blocks.sh` — the `install_ini_block`/`remove_ini_block`
  round-trip, the `clean_legacy_*` helpers, and the config.txt
  `[all]`-orphan perl.
- `test-args.sh` — `install.sh` argument parsing.
- `test-persist.sh` — the charge-mode persistence script.
- GitHub Actions runs `bash -n`, `shellcheck -S warning`, and every
  suite on each push and pull request.

**Security** (repo-audit hardening)
- Manual bootloader-config steps use `mktemp` instead of a fixed
  `/tmp/bootconf.txt`, closing a local TOCTOU on the root-consumed file.
- Installer Step 7 anchors its config.txt "already present" checks, so a
  commented-out or prefixed `dtoverlay=x120x` / `gpio=6=pu` line no
  longer suppresses the append.
- Installer runs `chmod -R go-w` on the copied DKMS source tree, so root
  builds from sources a non-root user cannot alter; the uninstaller
  narrows its orphan-cleanup glob to `x120x-[0-9]*`.
- Driver: `set_property` takes `chip->lock` around the charge-threshold
  writes and rejects a band-inverting value with `-EINVAL`; probe clamps
  `battery_mah` to `[1, 500000]` to avoid an integer overflow in the
  ENERGY_FULL property.
- Driver: the poll work item is now initialized before any power_supply
  is registered, closing a use-before-init window where a deferred
  `external_power_changed` event could call `mod_delayed_work()` on an
  uninitialized work item.
- Driver: the `conservation_start`, `conservation_end`, and
  `conservation_mode_default` module parameters are now `0444`
  (read-only at runtime).  Load-time `modprobe.d` configuration is
  unchanged; runtime threshold changes go through the validating,
  locking `charge_control_*_threshold` sysfs properties, and the charge
  mode follows `charge_type` writes.
- CI drops to `permissions: contents: read` and pins `actions/checkout`
  to a commit SHA.

**Note**
- The version was bumped to v0.4.6 for the installer and documentation
  work; the kernel module also carries the threshold-locking and
  `battery_mah`-clamp fixes listed under Security.

### v0.4.5 — Enforce the 2% low-battery shutdown threshold

**Installer**
- `install.sh` now sets `UsePercentageForPolicy=true` and
  `PercentageAction=2` in `UPower.conf`, in addition to
  `CriticalPowerAction=PowerOff`.  Previously the installer relied on the
  distribution default for `PercentageAction`; Debian/Raspberry Pi OS
  ship it as **0**, so UPower's PowerOff action only fired at 0% SoC
  (≈3.3 V, almost no margin above the 3.20 V floor) rather than the
  documented 2%.  UPower (GLib `GKeyFile`) honours the last value, so the
  appended `PercentageAction=2` overrides the distro default.
- Kernel module is unchanged from v0.4.4; version bumped to keep the
  package, DKMS, and module versions in lockstep.

### v0.4.4 — Charge hysteresis band restored

**Kernel driver**
- Charging now resumes at the lower threshold again, restoring a proper
  hysteresis band: **Fast** stops at 100% and resumes at 95%; **Long
  Life** stops at `conservation_end` (80%) and resumes at
  `conservation_start` (75%); the charger holds its state in between.
  The v0.3.0 "always-on" change had re-enabled the charger as soon as
  SoC fell below the *stop* threshold, so the pack was topped back up
  almost immediately and never rested below ~100% (Fast) or ~80% (Long
  Life) — defeating Long Life's longevity benefit and making the true
  self-discharge rate impossible to observe (the charger masked it).
- Deep-discharge safety is preserved: `charger_inhibited` still starts
  `false` (charging on) and any SoC at or below the resume threshold
  forces the charger on, so the charger is still always on at boot,
  after a deep discharge, and in any low/uncertain-SoC state — only an
  in-band reading holds the previous state.
- `conservation_start` is used by the hysteresis again (it had been
  exposed over sysfs but ignored).

### v0.4.3 — uevent storm fix

**Kernel driver**
- Initialise `bat_changed`, `ac_changed`, and `chrg_changed` to `false`
  at declaration in `x120x_poll_work`.  Previously `chrg_changed` was
  declared without an initialiser and only assigned to `true` inside
  the conservation-mode hysteresis block; in the steady state (the
  common case) the variable was read uninitialised at the notify site
  and the compiler-generated stack value was truthy often enough to
  fire `power_supply_changed(chip->charger)` on most poll cycles.
  Combined with the `supplied_to` propagation chain back into the
  battery's `external_power_changed` callback (which immediately
  reschedules the poll work), this turned a 2 Hz poll into a ~400 Hz
  feedback loop and produced approximately 820 `change` uevents per
  second on `/sys/class/power_supply/x120x-charger`.  The flood
  saturated `systemd-udevd` and two worker processes at ~90% CPU each.
  See [Real-world incidents — Incident 3](docs/incidents.md#incident-3--uevent-storm-from-uninitialised-stack-variable-2026-05-20) for the full diagnosis.

### v0.4.2 — Security audit follow-ups

**Installer**
- `set -euo pipefail` so unset variables and pipeline failures abort
  the install rather than continuing silently.
- `--battery-mah` is now validated as a positive integer at parse
  time.  The value is interpolated directly into
  `/etc/modprobe.d/x120x.conf`, so any non-numeric, empty, negative,
  zero, or shell-injection-shaped input is rejected before anything
  is written.  Leading zeros are normalised away to avoid any octal
  interpretation downstream.
- The device tree overlay is now compiled into a root-owned
  `mktemp -d` (mode 700, cleaned up on `EXIT`) rather than the source
  directory, and copied from there into `/boot/firmware/overlays/`.
  If the source tree happens to live on a path an unprivileged user
  can write to, compiling in place opened a brief TOCTOU window
  between `dtc` finishing and `cp` running.  A private tmpdir closes
  it.
- `logind.conf` and `UPower.conf` edits are wrapped in marker blocks
  delimited by `# >>> x120x-dkms: <tag> (do not edit) >>>` /
  `# <<< x120x-dkms: <tag> <<<`.  The installer:
  - never comments out lines it did not write;
  - relies on the systemd / UPower INI rule that the **last** matching
    key in a section wins, so appending our block at the bottom
    overrides any earlier user setting without disturbing it;
  - creates the `[Login]` / `[UPower]` section header on a minimal
    config file before writing the block;
  - is idempotent — a second install replaces the existing block in
    place rather than appending a duplicate;
  - cleans up bare lines left behind by pre-v0.4.2 installers
    (via `clean_legacy_logind` / `clean_legacy_upower`) so an upgrade
    from an older install doesn't accumulate dead comments.

**Uninstaller**
- **Regression fix:** the uninstaller no longer uncomments any line.
  Previous versions ran
  `sed -i 's/^#HandleLowBattery=/HandleLowBattery=/'` and the two
  analogous lines for `UPower.conf`, intending to "restore" what the
  installer had commented out.  But the installer commented blindly
  without recording which lines were originally uncommented, so a
  user who had deliberately written e.g. `#HandleLowBattery=ignore`
  to disable that policy would silently have it reactivated on
  uninstall.  All three restoration steps are removed.
- Marker-wrapped blocks are removed by `remove_ini_block`; lines
  outside the markers are never touched.
- Legacy line-by-line cleanup is retained (factored into the same
  `clean_legacy_logind` / `clean_legacy_upower` helpers used by the
  installer) so users upgrading from older installer versions still
  get cleaned up correctly.
- `set -euo pipefail`.

**Kernel driver**
- `pm_power_off` legacy function pointer replaced with
  `devm_register_sys_off_handler(SYS_OFF_MODE_POWER_OFF_PREPARE, ...)`.
  Only applies to the experimental X728/X708/X729 board variants
  (X120x does not use this path).  Benefits:
  - PREPARE-mode handlers may sleep, so the 3-second power-off pulse
    now uses `msleep(3000)` instead of a `mdelay(3000)` busy-wait.
  - The sys-off API supports stacking; we no longer unconditionally
    clobber a power-off handler that another driver may have
    installed.
  - `devm` cleanup tears down the handler automatically on unbind, so
    `x120x_remove` is simpler and the static global
    `x120x_poweroff_chip` is gone.
- GPIO16 (charge-control) state is now cached in
  `chip->charger_inhibited`, and the entire read-modify-write of the
  cached flag plus the hardware GPIO is performed under `chip->lock`.
  This closes a race where `x120x_poll_work`'s hysteresis decision
  could in principle disagree with a concurrent `charge_type` write
  from sysfs, briefly inhibiting charging based on stale state for
  one poll tick.  The same change also removes three unlocked reads
  of the hardware GPIO from the sysfs `get_property` callbacks; they
  now read the cached value, which is by definition consistent with
  `conservation_mode` because both are written under the same lock.
- README: the manual-install instructions now use
  `linux-headers-$(uname -r)` (matching the running kernel) rather
  than `raspberrypi-kernel-headers`, which can pull stale headers on
  Bookworm and cause DKMS builds to fail.
- Boot-log noise eliminated: when the DT overlay is present the
  module init function no longer races against it.  Previously, after
  `i2c_add_driver` returned (with the DT-instantiated client at 0x36
  already bound), the manual probe loop would try to register a
  duplicate client at 0x36, get EBUSY (logged by the i2c subsystem),
  fall through to 0x55, succeed in creating a phantom client, and
  produce three scary lines in dmesg
  (`Failed to register i2c client x120x at 0x36 (-16)`,
  `1-0055: failed to read chip version: -121`, and
  `1-0055: probe with driver x120x failed with error -121`).
  `probe()` now sets a flag on success; `x120x_init` checks the flag
  immediately after `i2c_add_driver` returns and skips the manual
  fallback entirely if a DT binding already happened.  Cosmetic only
  — the driver was functional before — but stops people thinking
  their install is broken.

### v0.4.1 — Installer and uninstaller robustness, locking cleanup

**Uninstaller**
- Uninstaller now discovers every installed version of the driver via
  `dkms status` and removes them all, rather than relying on a single
  hardcoded version string.  Fixes a case where `uninstall.sh` left the
  kernel module installed if it had been built against a different
  version than the uninstaller expected.
- Orphaned `/usr/src/x120x-*` source trees are cleaned up even if DKMS
  no longer tracks them.

**Installer**
- `apt-get update` is now run before `apt-get install` so that a Pi
  with a stale package index does not fail to find `dkms` or the
  kernel headers package.
- Removed a duplicated Step 10 block that re-installed the charge-mode
  persistence script and udev rule a second time.  Functionally
  harmless (same content written to the same files), but cleaned up
  for clarity.

**Kernel driver**
- The polling work function now snapshots `conservation_mode` and
  `capacity_pct` under the chip mutex and uses the local copies in
  the subsequent hysteresis region, rather than reading the `chip`
  fields a second time outside the lock.  Fixes a minor correctness
  issue where the hysteresis decision could in principle race against
  a concurrent `charge_type` write from sysfs.

### v0.4.0 — hwmon interface, rate estimation fix

**hwmon device registration**
- Driver now registers a hwmon device (`x120x`) alongside the existing
  `power_supply` devices at probe time
- Exposes four channels via `/sys/class/hwmon/hwmonN/`:
  - `in0_input` — cell voltage in mV (direct hardware reading, label `cell_voltage`)
  - `curr1_input` — charge/discharge current in mA, signed (derived, label `battery_current`)
  - `power1_input` — charge/discharge power in µW, signed (derived, label `battery_power`)
  - `energy1_input` — stored energy in µJ (derived, label `battery_energy`)
- Sign convention: positive = charging, negative = discharging
- Compatible with `sensors` (lm-sensors), Prometheus `node_exporter`
  (`--collector.hwmon`, enabled by default), collectd, Grafana, and any
  other tool that reads the standard Linux hwmon sysfs interface
- node_exporter exposes `node_hwmon_in_volts`, `node_hwmon_curr_amps`,
  `node_hwmon_power_watt`, `node_hwmon_energy_joules` labelled `chip="x120x"`
  with no additional configuration
- hwmon registration failure is non-fatal — the `power_supply` interface
  remains the primary ABI and the driver continues normally if hwmon
  registration fails

**Rate estimation fix**
- Fixed a bug where `energy_rate_uw` (and therefore `POWER_SUPPLY_PROP_POWER_NOW`,
  and all hwmon power/current channels) was permanently zero
- Root cause: `chip->capacity_256` was overwritten with `new_256` before
  the rate estimator compared `new_256 != chip->capacity_256` — the
  comparison was always equal so no rate was ever computed
- Fix: snapshot `old_256 = chip->capacity_256` before the update
- UPower's displayed `energy-rate` was unaffected because UPower computes
  its own rate from consecutive `energy_now` polls independently of the
  driver; `power_now` and all hwmon derived channels were the affected paths
- Added spike rejection: when the SoC register is stuck for >90 s and
  then jumps multiple LSBs in a single tick, the resulting rate estimate
  would be a large transient spike (large ΔE ÷ clamped dt).  The driver
  now detects this condition (real dt > 90 s clamp window) and retains
  the previous rate estimate rather than emitting the spike

### v0.3.0 — Deep discharge recovery hardening, GPIO6 pull-up, graph fixes

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

*[GPIO6 pull-up](README.md#gpio6-pull-up)*
- `gpio=6=pu` added to `config.txt` by installer — prevents GPIO6
  floating low at boot before the X1206 hardware asserts the AC-present
  signal, eliminating false `ac_online=0` readings after a power outage
  or PSU overload at boot

**UPower history and graph fixes**
- `NoPollBatteries=true` set in `UPower.conf` by installer — eliminates
  spurious `0%/unknown` history entries caused by UPower polling the
  kernel independently of driver notifications
- Battery status during Fast-mode float is reported as `Full` (on AC at
  ≥95% SoC); UPower history stays populated via the 30-second heartbeat
  below rather than by faking a discharge
- `power_now` reported as `0` when SoC is stable for >90 s — the driver
  cannot measure true self-discharge, so it reports zero rather than a
  fabricated floor
- 30-second heartbeat `power_supply_changed()` notification — keeps
  UPower history recording active during extended stable float periods
- AC state change no longer resets the rate tracking window — rate
  computation is continuous across grid transitions, eliminating
  transition spikes in the rate graph

### v0.2.0 — Experimental board support, additional properties, dead battery detection

*[Experimental board support](README.md#experimental-board-support)*
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
