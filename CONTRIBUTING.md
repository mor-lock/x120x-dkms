# Contributing to x120x-dkms

This is a personal open-source project maintained on a best-effort
basis.  Contributions are welcome — bug reports, hardware reports for
the experimental boards, documentation fixes, and patches.  For
anything larger than a small fix, please open an issue first so the
approach can be discussed before you invest time in it.

## Reporting bugs and hardware results

Use the issue templates:

- **Bug report** — attach the output of `tools/collect-debug.sh`
  (one-shot, read-only diagnostics paste).  See the README's
  Troubleshooting section for what to check first.
- **Hardware report** — results from the experimental boards (X728,
  X729, X708) are especially valuable: CI cannot build for or run on
  this hardware, so real-world reports are the only validation these
  code paths get.

Security issues: do **not** open a public issue — see
[SECURITY.md](SECURITY.md) for private reporting.

## Setup

Clone the repository and install the build dependencies (Debian /
Raspberry Pi OS package names):

```bash
git clone https://github.com/mor-lock/x120x-dkms.git
cd x120x-dkms

sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) \
                 device-tree-compiler shellcheck
```

`linux-headers-$(uname -r)` must match the running kernel — see
[docs/manual-install.md](docs/manual-install.md) for the Raspberry
Pi OS metapackage caveat.  `dkms` is only needed to install the driver for
real use, not to build and test it.  The shell test suite needs
nothing beyond bash and shellcheck, and never touches the system.

## Building

The driver builds out-of-tree against your running kernel (6.3 or
newer — see the README for why):

```bash
make            # module, against /lib/modules/$(uname -r)/build
make clean
```

Dependencies and the full manual build (overlay, DKMS registration,
bootloader config) are documented step by step in
[docs/manual-install.md](docs/manual-install.md).  The device tree
overlay compiles with:

```bash
dtc -@ -I dts -O dtb -o x120x.dtbo x120x-overlay.dts
```

## Testing

The shell test suite is unprivileged and fully mocked — it never
touches the real system, and refuses to run as root:

```bash
make test
```

Run it before sending any change to `install.sh`, `uninstall.sh`, or
`tools/`.  If you change installer behaviour, extend the corresponding
`tests/test-*.sh` to cover it; the tests sed-extract individual
functions and assert on the staged files and logs, so new cases are
cheap to add.

Kernel-side changes cannot be fully exercised in CI (the runners have
no Pi and no UPS).  Compile-check locally with warnings as errors, and
if you have the hardware, include before/after sysfs or `dmesg`
evidence in the PR:

```bash
make KCFLAGS=-Werror        # must be warning-clean
make W=1 KCFLAGS=-Werror    # extra kernel checks, also clean
make C=1                    # sparse, no findings on x120x.c
```

## What CI enforces

Every push and pull request runs (see `.github/workflows/ci.yml`):

- `bash -n` and `shellcheck -S warning` on all shell scripts
- the full test suite under `tests/`
- module compile-checks with `KCFLAGS=-Werror` on two kernel flavours,
  an Ubuntu 24.04 container (oldest supported LTS floor), and a
  cross-compile against raspberrypi/linux `rpi-6.12.y` — the kernel
  Raspberry Pi OS actually ships
- `make W=1` and sparse (`make C=1`), both required clean
- `checkpatch.pl --no-tree` over `src/x120x.c` (fetched pinned from
  kernel.org), with one documented ignore (`DEEP_INDENTATION`) listed in
  the checkpatch job in `ci.yml`
- dtschema validation of the DT binding (`dt-doc-validate`)
- device tree overlay compilation
- two documentation consistency checks that are easy to trip:
  - **Repository layout** — every tracked file must appear in the
    README's "Repository layout" tree.  If you add a file, add it to
    the tree.
  - **Markdown links** — `tools/check-links.sh` validates every link
    in every tracked `.md` file: in-page `[...](#anchor)` links,
    relative `.md` targets, and cross-file anchors.  If you rename or
    remove a heading, or move a file, fix its cross-references.
  - **Version strings** — `tools/check-versions.sh` verifies the
    version agrees between `dkms.conf`, `install.sh`, `src/x120x.c`,
    and the hardcoded refs in `docs/manual-install.md`.  Bump all
    four together.

## Code style

- **C** (`src/x120x.c`): Linux kernel coding style.  The driver
  follows the conventions of `drivers/power/supply/max17040_battery.c`
  in mainline, with upstreaming as a future goal — keep changes
  upstreamable.  CI enforces `checkpatch.pl` (with one documented
  ignore, `DEEP_INDENTATION` — see the checkpatch job in `ci.yml`),
  `W=1`, and sparse cleanliness.
- **Shell** (`install.sh`, `uninstall.sh`, `lib/`, `tools/`, `tests/`):
  bash, clean under `shellcheck -S warning`.  Match the existing
  patterns — in particular, the installer functions are written to be
  extractable by the test suite, so keep functions self-contained, and
  anything install.sh and uninstall.sh must agree on belongs in
  `lib/common.sh`.
- **Documentation**: the README (with `CHANGELOG.md` and `docs/`) is
  the reference documentation; changes that alter behaviour must
  update it in the same PR.

## Commits and pull requests

- One logical change per commit, with a body that explains *why* —
  this driver's history (see
  [docs/incidents.md](docs/incidents.md)) is part of its documentation.
- Add a `Signed-off-by:` line ([Developer Certificate of
  Origin](https://developercertificate.org/)) to commits touching
  `src/` or the device tree files — these are candidates for eventual
  upstreaming, where the DCO is required.
- CI must pass.  Changes to charge control, thresholds, or the
  shutdown path get extra scrutiny: this driver protects hardware that
  runs unattended, and its safety behaviour was shaped by real
  incidents.
- Releases are cut only after on-hardware validation
  ([RELEASING.md](RELEASING.md)), done by the maintainer on a real
  Pi + UPS — merged changes may therefore wait for the next validation
  cycle before appearing in a release.

## License

The project is licensed GPL-2.0-or-later.  Contributions are accepted
under the project license; new source files carry the existing SPDX
`GPL-2.0-or-later` header.
