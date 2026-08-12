# Real-world incidents that shaped this driver

Part of [x120x-dkms](../README.md).

This driver was developed on hardware running unattended, always-on.
Two real power incidents, plus two issues surfaced by users in the
field — a driver bug and a distribution-packaging interaction —
exposed failure modes that no lab test would have found, and drove
significant hardening of the driver and its installer.

A companion daemon running on the same system reads the driver's sysfs
nodes continuously, logs every reading to a SQLite database, and
implements layered shutdown logic on top of what the driver exposes.
All power data cited in the incidents below — SoC, voltage, AC state,
charge state, shutdown events, and PSU power draw — comes from that
database.  The driver surfaces the raw hardware values; the daemon
records and acts on them.

---

### Incident 1 — Deep discharge and cell destruction (2026-03-05)

#### What happened

A grid outage began at 17:20 UTC on 2026-03-05.  At the time, there
was no software undervoltage shutdown in place — it had been assumed
that the UPS hardware would cut power before the cells could be
damaged.  That assumption was wrong: the X120x UPS hardware has no
automatic undervoltage cutoff.  It simply powers the Pi until the
cells are physically unable to sustain the load.

The system ran on battery for 10.3 hours with nothing to stop it.
The fuel gauge saturated at 0% SoC when the cell voltage reached
3.25V — from that point on, voltage was the only reliable signal.
By 02:39 UTC the voltage had fallen below 3.0V, the point at which
irreversible electrochemical damage begins in lithium-ion cells.  The
Pi ran until 03:38 UTC when the supply rail collapsed at 2.54V.

When grid power returned at 08:58 UTC, the battery had been destroyed.
The cells could no longer hold a charge above ~2.99V despite being on
grid for 26+ hours.  Post-mortem analysis of the voltage data confirmed
a characteristic oscillation signature — rapid ±20 mV swings at the
fuel gauge output, a known pattern when the MAX17043 is alternating
reads across cell groups that can no longer hold voltage.

![The 2026-03-05 deep-discharge run to cell destruction: the fuel gauge floors
at 0 % while the true SoC continues into negative](images/destruction-run.png)

*The incident, reconstructed from the power database. **Top** — the raw fuel
gauge (orange) craters to ~2 % while the cell is still at 3.55 V (~29 % really
remaining) and then flatlines, giving no further information for the remaining
~4 hours; the voltage-observer model (blue, replayed on the recorded voltage)
instead tracks the true SoC straight through 0 % and into **negative** — the
pack was drained **far past empty**. **Middle** — cell voltage falling to
2.54 V, well past the 3.0 V damage threshold. **Bottom** — battery power
(`P = I·V`, recovered by the observer from voltage alone) held a steady ~7 W
until the cell could no longer sustain the load and collapsed (dashed tail,
where the fixed-resistance model breaks down). Nothing stopped the drain — no
undervoltage safeguard existed yet. The SoC below the 3.20 V usable floor is a
linear extrapolation of the OCV curve — continuing the near-empty slope down to
the ~2.5 V physical floor — so the negative value (~−13 % at the collapse) is an
estimate; the point is that the pack went well below 0 %.*

#### What the data revealed

Analysis of the power database from the incident produced several
findings that informed the driver design:

- The fuel gauge saturates at 0% SoC while voltage is still 3.25V —
  well above the damage threshold.  Once SoC hits 0%, **voltage is the
  only reliable indicator** of remaining capacity.
- Cell damage begins at approximately 3.0V, confirmed by the onset of
  voltage oscillation in the data.  Sixty-three oscillations of >15mV
  within intervals of <30 seconds were recorded in the first 100
  sub-3.0V readings — a distinct signature not seen during healthy
  discharge.
- A destroyed battery on grid shows a characteristic plateau: voltage
  rises only ~165mV over 26 hours (surface charge only), never enters
  a `CHARGING` state, and settles around 2.99V.  Healthy cells charge
  from 2.8V to 4.1V within 2–3 hours.
- The gap between the 10% SoC shutdown trigger and the 3.20V voltage
  trigger is approximately 14 minutes.  If the SoC-based trigger fails,
  the voltage backstop is the last line of defence before cell damage.

#### What was added to the driver

The core lesson was that the X120x hardware provides **no undervoltage
protection** — software must supply it entirely.  This shaped several
additions to the driver:

**Dead battery detection** — when the system is on grid and the cell
voltage remains below 3.10V for 10 minutes with no meaningful rise
(less than 10mV/h), the driver reports `health=Dead` via the
`x120x-battery/health` sysfs node.  UPower surfaces this as
`health: dead` and desktop environments display a battery warning.
A kernel log entry is also emitted.  This allows the operator to
identify destroyed cells and replace them before relying on the UPS
for protection again.

**Capacity level reporting** — the driver reports `capacity_level`
accurately throughout the discharge curve, giving UPower and logind
the information needed to trigger a clean shutdown via the standard
`HandleLowBattery=poweroff` path before the cells reach a dangerous
voltage.  Without this, UPower has no basis on which to act.

The incident made clear that the X120x hardware provides no
undervoltage protection whatsoever — software must supply it entirely.
The install script configures the complete shutdown chain automatically:
the driver reports `capacity_level=Critical` at low SoC, UPower
escalates this to `warning-level: action`, and logind calls
`systemctl poweroff` — all without any additional daemon or script.
No extra userspace software is required beyond what the installer
sets up.

---

### Incident 2 — Grid return undetected, recovery livelock (2026-03-29)

#### What happened

A grid outage began at **10:26:50 UTC on 2026-03-29** with the battery
at 82% SoC / 4.04 V.  The system ran on battery normally, discharging
at the expected rate.

Grid was restored approximately 1 hour after the outage began
(confirmed by the uptime of a desktop machine on the same circuit), but
the X1206 never detected the return — `ac_online` remained `0` for the
remainder of the discharge.  Because the companion daemon saw no grid,
charging never resumed.  The system continued draining as if the outage
was still in progress.

The companion daemon's shutdown mechanism worked correctly:
`shutdown_armed` fired at **14:29:28 UTC** at 10.0% SoC / 3.59 V, and
`shutdown_initiated` followed 15 seconds later exactly as designed.
At that point the grid had already been back for approximately 3 hours,
and the cells should have been charging throughout that window.  They
were not, because the board was silently failing to assert GPIO6.

When the system rebooted after shutdown, `ac_online` was still `0`
despite the charger being connected.  The
system entered a livelock: it booted, UPower immediately read
`capacity_level=Critical` on the near-empty battery, logind called
`systemctl poweroff`, the UPS cut and then restored power, and the
cycle repeated.  This drained the cells further on every cycle.

The livelock ran across three dates — 2026-03-29 (2 cycles from the
initial recovery attempt), 2026-03-30 (11 cycles), and 2026-04-02 (5
cycles, the last confirmed shutdown voltage 3.15 V) — for a total of
**18 forced shutdowns** before the board was replaced.  The database
records no `ac_online=1` after the original outage, because the board
was never able to drive GPIO6 high again.

#### Root cause analysis

**X1206 hardware failure — GPIO6 output stage.**  Forensic analysis of
the power database confirms that `ac_online` never returned `true`
after the 10:26:50 UTC grid loss, with the grid independently confirmed
as restored roughly an hour later.  The GPIO6 output stage on the board
had failed silently during normal operation: not at boot, not under
load stress, but mid-session while the system was running.  This is
a harder failure mode than a boot-time marginal-PSU scenario — the
board stopped driving its own AC-present signal while everything else
appeared functional.

The v0.3.0 driver fixes (GPIO6 pull-up, `capacity_level=Critical`
suppressed on AC, always-on charger at probe) mitigated the livelock
mechanism by protecting against a floating GPIO6 at boot.  They could
not compensate for a board whose output stage had permanently failed.
Board replacement was the correct and necessary remedy.

With `ac_online=0` and the battery at near-zero SoC, the livelock
chain on every boot was:

1. UPower read `capacity_level=Critical` and fired
   `warning-level: action` immediately — before the driver had finished
   probing.
2. logind received the action and called `systemctl poweroff`.
3. The UPS cut power, then restored it (auto-restart on halt).
4. The cycle repeated.

#### What was added to the driver

**`gpio=6=pu` pull-up in `config.txt`** — the installer now adds a
software pull-up on GPIO6.  The UPS hardware actively drives GPIO6 low
on power loss and high when AC is present.  The pull-up ensures the pin
reads high (AC present) by default during boot, before the hardware has
finished asserting the signal.  This protects against GPIO6 floating
low during the boot window; it cannot compensate for a board whose
output stage has failed entirely.

**`capacity_level=Critical` only reported on battery** — the driver
previously reported `capacity_level=Critical` whenever SoC dropped
below 5%, regardless of AC state.  On a nearly-dead battery with AC
present, this caused UPower to trigger a shutdown loop during recovery.
The driver now only reports `Critical` when `ac_online=0` — when mains
is present, even at 0% SoC, the battery is charging and shutting down
would cause exactly the livelock described above.

**Charger always enabled at probe** — the driver explicitly drives
GPIO16 low (charger enabled) at probe time, regardless of any
previously saved state.  A battery that has been deeply discharged
starts charging immediately on every boot.

**Charger default changed to always-on** — the charge hysteresis logic
previously only re-enabled the charger when SoC dropped below the
resume threshold.  The start threshold has been removed: the charger is
now enabled whenever SoC is below the stop threshold, defaulting to on
in all uncertain or low-SoC states.  (v0.4.4 later restored the
hysteresis band — the charger is still forced on at probe and at or
below the resume threshold, but in-band readings now hold state; see
the changelog.)

**0% SoC no longer treated as implausible** — the driver previously
issued a MAX17043 quick-start command when the initial SoC reading was
0%, treating it as a fuel gauge convergence failure.  After deep
discharge the battery is genuinely at 0% — issuing a quick-start
resets the fuel gauge's SoC model at the worst possible moment.  The
plausibility floor has been lowered to 0%.

#### Resolution — X1206 board replacement (2026-04-07)

The faulty board was replaced with a new X1206 on 2026-04-07.  The
existing Molicel INR-21700-P50B cells (4 × 5000 mAh, 20 Ah pack) were
reinstalled — deeply depleted by the livelock cycles but undamaged,
as the repeated shutdowns had kept the voltage above the cell damage
threshold throughout.
The power supply was also replaced with a multi-port GaN charger
(Anker Prime 160 W) giving the Pi and the mobile router
independent ports with separate overcurrent protection, eliminating
any shared-PSU load concern at boot.

The new board's first reading, at **18:20:53 UTC on 2026-04-07**, showed
`ac_online=1` immediately — GPIO6 asserting correctly from the first
moment — with `soc_pct=0.01%` and `bat_v=3.34 V`.  The v0.3.0/v0.4.0
recovery path worked exactly as designed: `capacity_level=Critical` was
suppressed because `ac_online=1`, UPower did not trigger a shutdown, and
the charger was on from the first probe.  Zero livelock cycles occurred.

At 20:36:20 UTC, a brief `grid_change: true → null → true` transition
lasting ~0.5 s was recorded — this corresponds to the v0.4.0 driver
module being reloaded during installation.  Charging continued without
interruption.

The cells charged from 0.01% / 3.34 V to 99.6% / 4.22 V in
approximately **6.7 hours**, consistent with the X1206's 3 A charge
ceiling (~15 W) applied to a 20 Ah pack.  PSU draw measured via the
driver's hwmon interface held steady at **~16.7 W** throughout the bulk
charge phase (battery charging plus Pi idle consumption), dropping to
**~5.9 W** once the cells reached full charge and the charger switched
to float.

| Milestone | Time (UTC, 2026-04-07/08) | SoC | Cell voltage |
|---|---|---|---|
| First valid reading | 18:20:53 | 0.01% | 3.34 V |
| Charging begins | 18:33:24 | 0.32% | 3.48 V |
| 10% | 20:16:52 | 10.0% | 3.74 V |
| 50% | 22:02:38 | 50.0% | 3.94 V |
| 80% | 23:21:27 | 80.1% | 4.09 V |
| Full (~100%) | 01:02:05+1 | 99.6% | 4.22 V |

The healthy charge profile — smooth SoC rise, voltage climbing steadily
from 3.34 V to 4.22 V, no oscillation, no plateau — confirmed that the
new cells were undamaged.

#### Operational lesson

**X1206 GPIO6 output stage failure is silent and undetectable in
software.**  The board continued to appear functional in every other
respect: the fuel gauge was readable over I²C, and the system ran
normally on battery.  Only the AC-present signal was
wrong, and only the power database — recording `ac_online=0` throughout
a period when grid was independently confirmed as restored — revealed
the failure.

If `ac_online` remains `0` after a grid outage despite the charger
LED indicating input power, and the pattern persists across multiple
reboots with the v0.3.0+ driver and `gpio=6=pu` in place, the board
itself should be suspected and replaced.  The driver cannot work around
a permanently failed GPIO6 output stage.

---

### Incident 3 — uevent storm from uninitialised stack variable (2026-05-20)

#### What happened

The system fan on the host Pi 5 had been audibly revving for an extended
period.  CPU temperature was a benign **65.9 °C** and the SoC was not
thermally throttled (`vcgencmd get_throttled` reported `0x0`), but the
fan's PWM cooling device was sitting at `cur_state=2/4` continuously,
indicating sustained cooling demand driven by compute load rather than
silicon heat.

`uptime` reported a load average of **7.01** on a 4-core Pi 5 — fully
saturated.  `ps` showed three processes consuming the bulk of the CPU:

```
    345 91.7 systemd-udevd
 818657 87.1 (udev-worker)
 818612 83.6 (udev-worker)
```

`udevadm monitor --kernel` revealed a continuous flood of `change`
uevents from `/sys/class/power_supply/x120x-charger`, advancing the
kernel `SEQNUM` counter by roughly **820 events per second**.  Over the
21 h uptime preceding the diagnosis, the system had emitted approximately
**62 million** uevents on this single device — every one of them carrying
identical property values, and every one of them woken up udev to scan
the rules database and re-evaluate the same hook chain.

#### Root cause analysis

**Uninitialised `chrg_changed` stack variable in `x120x_poll_work`.**

The poll work function declared three booleans on entry:

```c
bool bat_changed, ac_changed, chrg_changed;
```

In the I²C error paths all three were set to `false` before the
`goto notify` jump.  In the happy path `bat_changed` and `ac_changed`
were assigned unconditionally from the new vs. cached comparisons, but
`chrg_changed` was only assigned to `true` inside the conservation-mode
hysteresis block when `want_inhibit != chip->charger_inhibited` — i.e.
only when GPIO16 actually needed to flip.  In the steady state this
branch is rarely taken, so the variable was read at the notify site with
whatever garbage the stack happened to contain.

The compiler-generated stack frame produced a truthy value on most
invocations, causing `power_supply_changed(chip->charger)` to fire every
poll.  This kicked off a tight feedback loop via the `supplied_to`
notification chain:

1. `power_supply_changed(charger)` schedules `power_supply_changed_work`
2. The kernel walks supplicants — the battery is supplied by the charger
3. The battery's `external_power_changed` callback fires
4. That callback calls `mod_delayed_work(system_wq, &chip->work, 0)`,
   kicking `x120x_poll_work` to run immediately
5. The poll reads I²C, finds no real state change, but reads the
   uninitialised `chrg_changed` as truthy and fires
   `power_supply_changed(charger)` again
6. Goto 1

`bpftrace`-confirmed rates during the incident:

| Function                       | Calls / second |
|---|---|
| `x120x_poll_work`              | ~405 (vs. the intended 2 Hz) |
| `power_supply_changed_work`    | ~412 |
| `power_supply_changed(charger)`| ~423 |
| kernel `uevent_seqnum` growth  | ~820 |

The poll loop was running **200× faster than designed**, each iteration
re-triggering the loop on a stack-resident phantom.

The bug was latent from v0.4.1, where the polling work function was
restructured to take snapshots of `conservation_mode` and `capacity_pct`
under the chip mutex (see that release's changelog).  The refactor
introduced the unconditional read of `chrg_changed` at the notify site
without ensuring the variable was initialised on every path leading
there.  GCC's `-Wmaybe-uninitialized` does not fire on this case because
the variable *is* assigned on the failing path (via the
`if (want_inhibit != chip->charger_inhibited)` branch), just not on
every path.

#### What was added to the driver

**Default-initialise `bat_changed`, `ac_changed`, and `chrg_changed`
at declaration.**  All three booleans now default to `false`, so the
notify site reads `true` only when an explicit assignment marked a real
state change.  Defensive initialisation of all three (not just the
one that bit us) prevents the same class of bug from reappearing the
next time a path is added to the function.

#### Operational lesson

**Sustained fan noise without a hot SoC means a software bug, not a
thermal one.**  At 65 °C the Pi 5's silicon is well inside its comfort
envelope; the fan curve responds to total CPU load, not just core
temperature.  If the fan is loud while `vcgencmd measure_temp` reports
something benign, the first place to look is `uptime` and the top of
`ps`.  In this case the load average pointed at udev within seconds —
and `udevadm monitor --kernel` exposed the storm in another two.

`/sys/kernel/uevent_seqnum` is an underused diagnostic.  Reading it
twice with a delay gives the kernel-wide uevent rate in a single
shell pipeline:

```bash
s1=$(cat /sys/kernel/uevent_seqnum); sleep 2; \
  s2=$(cat /sys/kernel/uevent_seqnum); echo $(( (s2-s1) / 2 ))/sec
```

A healthy idle system reports `0/sec`.  Anything higher than the low
tens, sustained, is a misbehaving driver.

#### Independent confirmation on a different board variant

A second user ([issue #2](https://github.com/mor-lock/x120x-dkms/issues/2))
hit the same bug independently on a **Geekworm X1209 + X1002 NVMe**
expansion board running v0.4.2, on the same day the author diagnosed
it on an X1206.  Their symptom was different — no audible fan, but the
attached Samsung 970 Evo NVMe was heatsoaking to **70–75 °C** at idle
versus a normal **~51 °C** on v0.4.1.  The mechanism is the same: the
udev rule `90-x120x-persist.rules` runs
`/usr/local/lib/x120x-persist-mode.sh` on every `change` event, and at
~820 events per second the constant `fork`+`exec` plus small writes
keep the NVMe controller pinned in its highest active power state and
the PCIe link out of L1 substates.  After upgrading to v0.4.3 their
NVMe settled back to **51–52 °C** within ~15 minutes and
`uevent_seqnum` delta reported `0/sec`.

The same reporter noted that their v0.4.1 idle NVMe temperature
(**58–61 °C**) was elevated above the clean v0.4.3 baseline
(**51–52 °C**) by ~7–10 °C.  This is consistent with the uninitialised
`chrg_changed` reading as *intermittently* truthy on v0.4.1's stack
frame layout — same bug, but a lower duty cycle than the
always-truthy pathology v0.4.2 happened to produce.  v0.4.3's explicit
`= false` initialiser makes the variable deterministically falsy on
every entry, so the baseline should now match a system that never had
the bug.

This second data point matters because it widens the symptom set
documented for this incident: the same bug can present as fan noise
on a host with mediocre case airflow and no NVMe, as silent NVMe heat
on a host with a stack expansion board, or — in principle — as
elevated power draw and slightly shortened battery runtime on any
host.  Future reports that don't match the original "loud fan" shape
should still trigger the same diagnostic (`uevent_seqnum` delta) as
the first step.

---

### Incident 4 — Driver vanishes after an Ubuntu update (2026-08)

#### What happened

A user running **Ubuntu 26.04 LTS on a Raspberry Pi 5** with a Geekworm
X1201 ([issue #5](https://github.com/mor-lock/x120x-dkms/issues/5))
installed the driver, confirmed it working — battery icon, `upower`
readings, the lot — and then, after an unrelated `sudo apt upgrade`,
found the battery system simply gone.  GNOME Power Statistics showed no
battery device; the driver had not loaded at all after the reboot that
followed the update.  Reinstalling the driver brought it back every
time, but the next OS update broke it again.

The first hypotheses were wrong, and ruling them out mattered.  A full
battery drain followed by a cold boot was tested deliberately and did
**not** reproduce the failure — the driver came up fine — which
eliminated deep discharge and any hardware/fuel-gauge power-up state as
the cause.  The common factor in every break was an Ubuntu package
update, not a power event.

The diagnostic capture pinned it down.  After a break, with the driver
dead, `/boot/firmware/config.txt` **still contained** the installer's
lines:

```
[all]
# SupTronics X120x UPS HAT driver
dtoverlay=x120x
gpio=6=pu
```

but the overlay file the line refers to was gone:

```
ls: cannot access '/boot/firmware/current/overlays/x120x.dtbo': No such file or directory
```

The config was intact; the overlay it pointed at had been deleted.  A
dangling `dtoverlay=` reference loads nothing, so the driver never
bound.

#### Root cause analysis

**Ubuntu's `flash-kernel` repopulates the overlays directory on every
update, discarding out-of-tree files.**  Ubuntu for Raspberry Pi boots
via `flash-kernel`, which owns `/boot/firmware/current/overlays/`
(the `current/` prefix is set by `os_prefix=current/` in `config.txt`)
and lays that directory back down from the kernel and firmware packages
whenever either is upgraded.  The driver's overlay, `x120x.dtbo`, is not
part of any package — the installer compiles it locally and copies it in
— so a `flash-kernel` refresh simply drops it.  `config.txt` is a
separate file that `flash-kernel` does not rewrite, which is why the
`dtoverlay=x120x` line survived while the file it names did not.

**Raspberry Pi OS is not affected**, and confirming that shaped the fix.
Raspberry Pi OS does not use `flash-kernel` or the `current/` prefix;
its `raspberrypi-firmware` / `raspberrypi-kernel` packages overwrite
their *own* shipped overlays in `/boot/firmware/overlays/` but do not
purge the directory, so a custom `x120x.dtbo` is left alone.  This was
verified directly on the maintainer's own Raspberry Pi OS system: a
kernel-and-firmware upgrade (`linux-image-rpi-v8` 6.12.75 → 6.12.96,
`raspi-firmware` bumped, the boot-partition firmware files rewritten)
left the driver's overlay in place with its original timestamp,
untouched.  The two distributions genuinely differ, so the fix is
Ubuntu-only and Raspberry Pi OS installs are left byte-for-byte
unchanged.

**A secondary failure made recovery worse.**  When the user re-ran the
installer to recover, it aborted at `dkms add` with *"DKMS tree already
contains: x120x/…"*.  Because that abort happened before the overlay
copy step, the recovery reinstall died **without** putting the overlay
back — turning a one-line fix into a multi-step fight (uninstall,
reinstall, re-enable I²C, re-clone, reboot) before the driver finally
returned.  An installer meant to be the recovery tool was itself
brittle in exactly the state a recovery starts from.

#### What was added to the installer

**Overlay-persistence apt hook (Ubuntu only).**  The installer now
stashes the compiled overlay at `/usr/local/lib/x120x-overlay.dtbo` and
registers a `DPkg::Post-Invoke` hook
(`/etc/apt/apt.conf.d/99-x120x-overlay`) that runs a small helper,
`/usr/local/lib/x120x-restore-overlay.sh`, at the end of **every** apt
transaction.  `Post-Invoke` runs after all package work — including the
`flash-kernel` refresh that does the wiping — so the overlay is back in
place before the machine is ever rebooted into the updated kernel.  The
helper is written to be unable to disrupt package management: it runs
with `set +e`, always exits 0, and does nothing at all unless the
overlay is configured in `config.txt` yet missing from the active
overlays directory and a stashed copy exists.  The whole mechanism is
gated on the Ubuntu `…/current/overlays` layout and is never installed
on Raspberry Pi OS.

**Idempotent `dkms add`.**  The installer no longer treats an
already-registered DKMS tree as fatal: if `dkms add` reports the version
is already present, it verifies the tree exists and continues to build
and install rather than aborting.  A recovery re-run now always reaches
the overlay step, so re-running the installer reliably restores a broken
install instead of dying at the first hurdle.

**Uninstaller parity.**  `uninstall.sh` now resolves the Ubuntu
`current/overlays` layout the same way the installer does (previously it
only looked in `…/overlays`, so it never cleaned up on Ubuntu), and it
removes the apt hook, helper, and stash.

#### Operational lesson

**On Ubuntu / `flash-kernel`, anything you place in the boot partition
by hand does not survive a package update.**  The active-kernel overlay
directory is owned by `flash-kernel` and rebuilt from packages; an
out-of-tree overlay must be re-applied by a hook, not merely copied once
at install time.  This is a distribution-integration failure mode with
no equivalent on Raspberry Pi OS, and it is invisible until the *second*
kernel update — the install works, the first reboot works, and only a
later `apt upgrade` exposes it.

The fast diagnostic is to check the two halves of the overlay reference
independently: confirm `dtoverlay=x120x` is present in `config.txt`
**and** that `x120x.dtbo` actually exists in the active overlays
directory (`/boot/firmware/current/overlays/` on Ubuntu,
`/boot/firmware/overlays/` on Raspberry Pi OS).  A present line pointing
at an absent file is the signature of a package update having reclaimed
the directory.

#### Resolution (v0.4.10)

The apt-hook fix shipped in **v0.4.10** and was confirmed end-to-end by the
reporter on the affected system: a fresh install, then `sudo apt upgrade`
(pulling kernel/firmware updates), then a reboot — with the driver loading
normally afterwards.  The hook restored the overlay during the upgrade
transaction, so the dangling-reference failure no longer occurs.  Since the
maintainer has no Ubuntu hardware, this field confirmation is what validated
the fix.
