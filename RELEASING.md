# Releasing x120x-dkms

A release is cut only after the build passes **on-hardware validation**
on a real Pi + UPS.  This checklist codifies the v0.4.6 process — follow
it verbatim.  Save all Phase 0–4 captures under
`~/x120x-validate-$(date +%F)/`.

## Versioning convention

Versions bump **per change**, not per release.  `MODULE_VERSION`
(src/x120x.c), `PACKAGE_VERSION` (dkms.conf), and `PKG_VERSION`
(install.sh) advance in lockstep with each substantive commit — CI's
check-versions step enforces their agreement — and every change is logged
under the single **`Unreleased`** section of `CHANGELOG.md`.  At tag time
that section is renamed to the release number and the three version
strings are reconciled to it: the release is whatever version the tree
already carries, not a round number chosen after the fact.

**History is never squashed to a round version.**  The per-commit
`MODULE_VERSION` strings surface in kernel logs, `modinfo`, and the
on-hardware validation reports, so each must stay resolvable to the exact
commit that produced it.  Rewriting the tree back to a tidy `0.5.0` (or
any round number) would break that mapping and is not done — the tree
ships as whatever it says it ships as.

## 0. Pre-flight snapshot (read-only)

- `dkms status`; `modinfo x120x | grep -E 'version|filename'`
- `sudo rpi-eeprom-config > eeprom.before`
- copies of `/etc/systemd/logind.conf`,
  `/etc/systemd/logind.conf.d/90-x120x.conf` (if present),
  `/etc/UPower/UPower.conf`, `/boot/firmware/config.txt`,
  `/etc/modprobe.d/x120x.conf`
- sysfs: `capacity`, `voltage_now`, `status`, `health`, `charge_type`,
  both `charge_control_*_threshold`, AC `online`,
  `ls -l /sys/module/x120x/parameters/`
- `upower -i /org/freedesktop/UPower/devices/battery_x120x_battery`
- Confirm no critical workload is mid-operation — a reboot is coming.

## 1. Repo verification (on target, unprivileged)

- `bash -n install.sh uninstall.sh tools/*.sh tests/*.sh`
- `make test` — all suites pass.

## 2. Install over the existing version

- `sudo bash install.sh --battery-mah <capacity>` — capture full output.
  Expect: old DKMS removed and the new version built/installed; EEPROM
  "already configured" skip (no `--apply`) if already set; both
  config.txt "already present"; the logind drop-in written (and any
  pre-drop-in marker block removed from `logind.conf`); exactly one
  UPower marker block.
- Diff each config copy against its post-install state: **config.txt and
  EEPROM must be byte-identical**.

## 3. Reboot

- `sudo reboot` at a quiet moment.  Watchdog, UPS auto-restart, and
  service `Restart=always` cover the failure modes.

## 4. Post-boot validation

- `modinfo x120x` shows the new version; `dmesg | grep x120x` is clean.
- `/sys/module/x120x/parameters/` — `conservation_*` are `-r--r--r--`.
- Param side-door: `echo N | sudo tee .../parameters/conservation_end`
  fails with **Permission denied**.
- Thresholds: read 75/80; write a valid pair; an inverted value and
  out-of-range (`101`, `-1`) each fail with **Invalid argument**;
  restore 75/80.
- `charge_type` round-trip: `Long Life` → conf
  `conservation_mode_default=1`, `Fast` → `0` (udev persistence); leave
  in **Fast**.
- `upower -i .../battery` sane; hwmon `in0_input`, `power1_input`,
  `curr1_input`, `energy1_input` read.
- Host services healthy; logs clean since boot.
- `sudo rpi-eeprom-config` equals `eeprom.before`.

## 5. Optional live AC test

- Briefly pull UPS input at high SoC: `ac_online` 0↔1, status flips
  Discharging and back, no spurious shutdown.  Optional; skip if not
  convenient.

## 6. Tag and release (only if everything above passes)

- Bump `PKG_VERSION` (install.sh), `PACKAGE_VERSION` (dkms.conf),
  `MODULE_VERSION` (src/x120x.c), and the manual-install version refs
  (docs/manual-install.md) together — CI's check-versions step
  enforces agreement; finalize the `CHANGELOG.md` entry.
- Update the maintainer row in README's "Tested hardware" table to the
  version just validated (only after Phase 4 has passed).
- Commit, push, confirm CI green on `main`.
- `git tag -a vX.Y.Z -m "…" <validated-commit>`; `git push origin vX.Y.Z`.
- Confirm CI green on the tag.
- `gh release create vX.Y.Z --title "…" --notes-file <CHANGELOG.md-section>
  --verify-tag --latest`.
