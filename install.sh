#!/usr/bin/env bash
# install.sh — x120x-dkms installation script
#
# Installs the SupTronics X120x UPS HAT kernel driver on Raspberry Pi.
# Run from the repository root:
#
#   sudo bash install.sh [OPTIONS]
#
# Options:
#   --battery-mah N        Total battery pack capacity in mAh (default: 1000)
#                          Example: 4x 5000mAh cells = 20000
#   --soc-source SRC       State-of-charge source: voltage (default) or gauge.
#                          voltage = NMC OCV model (avoids the fuel gauge's
#                          near-full over-read); gauge = raw MAX17043 register.
#   --charge-mode MODE     Initial charge mode: fast or longlife (default: fast)
#                          longlife limits charging to 78-80% to extend battery life
#                          Can be changed at any time via sysfs; persisted across reboots
#   --board VARIANT        Board variant (default: x120x).
#                          Supported: x120x, x728v2, x728v1, x708, x729
#                          Variants other than x120x are EXPERIMENTAL (untested).
#   --skip-eeprom          Do not modify Raspberry Pi bootloader EEPROM settings
#                          (POWER_OFF_ON_HALT, PSU_MAX_CURRENT).  You are then
#                          responsible for configuring them manually — see README.
#
# Copyright (C) 2026 Edvard Fielding <mor-lock@users.noreply.github.com>
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

# -------------------------------------------------------------------------
# Shared helpers — output, require_root, INI marker constants, legacy
# cleanup.  lib/common.sh is the single source of truth for the contract
# install.sh and uninstall.sh must agree on.
# -------------------------------------------------------------------------

# shellcheck source=lib/common.sh
. "$(cd "$(dirname "$0")" && pwd)/lib/common.sh" \
    || { echo "[x120x] ERROR: cannot load lib/common.sh — run from a full checkout" >&2; exit 1; }

# -------------------------------------------------------------------------
# Temp-file cleanup
#
# Paths pushed here are removed on exit, so a kill mid-run cannot leak
# them.  Individual steps also rm on their normal paths; this is the
# backstop.
# -------------------------------------------------------------------------

X120X_CLEANUP=()
_x120x_cleanup() {
    [ "${#X120X_CLEANUP[@]}" -eq 0 ] || rm -rf -- "${X120X_CLEANUP[@]}" 2>/dev/null || true
}
trap _x120x_cleanup EXIT

# -------------------------------------------------------------------------
# INI block helper
#
# Append a marker-wrapped block of lines under a named section in an INI
# file.  systemd-style INI honours the LAST matching key for any given
# section, so appending our block at the bottom of [Login] / [UPower]
# overrides any prior setting without touching what's already there.
#
# Marker convention:
#   # >>> x120x-dkms: <tag> (do not edit) >>>
#   <content>
#   # <<< x120x-dkms: <tag> <<<
#
# The uninstaller deletes everything between matching markers; lines
# outside the markers are never touched, so users who set their own
# values are left alone.
#
# Usage: install_ini_block FILE SECTION TAG LINE [LINE ...]
#   FILE     — path to the INI file (must exist)
#   SECTION  — section name without brackets (e.g. "Login")
#   TAG      — short identifier used in the marker comment
#   LINE...  — one or more lines to write inside the block
#
# The marker prefix constants come from lib/common.sh, shared with
# remove_ini_block in uninstall.sh.
# -------------------------------------------------------------------------

install_ini_block() {
    local file="$1" section="$2" tag="$3"
    shift 3

    [ -f "${file}" ] || { warn "${file} not found — skipping ${tag}"; return 0; }

    local marker_begin="${X120X_MARKER_BEGIN_PREFIX} ${tag} (do not edit) >>>"
    local marker_end="${X120X_MARKER_END_PREFIX} ${tag} <<<"

    # If our block is already present, remove it so we can rewrite it
    # idempotently with the current desired content.  We only ever touch
    # text between our own markers.
    if grep -qF "${marker_begin}" "${file}"; then
        # Delete from begin marker through end marker, inclusive.
        # Use a literal-string match via address-of-text by escaping for sed.
        local esc_begin esc_end
        esc_begin=$(printf '%s\n' "${marker_begin}" | sed 's/[][\/.^$*]/\\&/g')
        esc_end=$(printf '%s\n' "${marker_end}"   | sed 's/[][\/.^$*]/\\&/g')
        sed -i "/^${esc_begin}$/,/^${esc_end}$/d" "${file}"
        # The block always sat at EOF preceded by the one blank line we
        # prepend below.  Deleting the block leaves that blank as the
        # final line; drop exactly it so a reinstall stays byte-identical
        # instead of accumulating a blank line on every run.
        sed -i -e '${/^$/d}' "${file}"
    fi

    # Ensure the section header exists.  If not, append a blank line and
    # the header before our block so the keys land in the right section.
    if ! grep -qE "^\[${section}\][[:space:]]*$" "${file}"; then
        printf '\n[%s]\n' "${section}" >> "${file}"
    fi

    # Append the marker-wrapped block at end of file.  Since the section
    # header was either pre-existing further up OR just appended on the
    # line above, our block lands inside that section.  systemd reads
    # the LAST occurrence of any key in a section, so this overrides any
    # earlier setting without commenting anything out.
    {
        printf '\n%s\n' "${marker_begin}"
        local line
        for line in "$@"; do
            printf '%s\n' "${line}"
        done
        printf '%s\n' "${marker_end}"
    } >> "${file}"
}

# clean_legacy_logind / clean_legacy_upower (lib/common.sh) are called
# before each marker block is written, so a system that went through a
# pre-marker installer gets its bare legacy lines removed either way.

# -------------------------------------------------------------------------
# DKMS tree copy
#
# Copy exactly what DKMS needs to build the module: dkms.conf, the
# Makefile it invokes, the two files under src/, and LICENSE for
# provenance.  An explicit allowlist rather than `cp -r` of the whole
# checkout, which dragged `.git` and the documentation into /usr/src
# on every install and would also pick up stray build artifacts from
# a developer tree.
#
# Usage: install_dkms_tree SRC_DIR DEST
# -------------------------------------------------------------------------

install_dkms_tree() {
    local src="$1" dst="$2"

    rm -rf "${dst}"
    install -d "${dst}/src"
    cp "${src}/dkms.conf" "${src}/Makefile" "${src}/LICENSE" "${dst}/"
    cp "${src}/src/x120x.c" "${src}/src/Kbuild" "${dst}/src/"
    # Root builds from this tree, so strip any group/other-writable
    # bits inherited from the user's checkout — a non-root user must
    # not be able to alter sources that are then compiled and
    # installed as root.
    chmod -R go-w "${dst}"
}

# -------------------------------------------------------------------------
# logind drop-in
#
# Low-battery shutdown is configured through a drop-in file under
# /etc/systemd/logind.conf.d/ instead of editing logind.conf: the
# packaged file stays pristine (dpkg never sees it as modified, so no
# conffile prompt on systemd upgrades), every line in the drop-in is
# ours, and uninstall is a plain rm.  UPower has no drop-in mechanism,
# so UPower.conf keeps the marker-block approach above.
#
# Usage: install_logind_dropin FILE
# -------------------------------------------------------------------------

install_logind_dropin() {
    local file="$1"
    mkdir -p "$(dirname "${file}")"
    cat > "${file}" << 'DROPIN_EOF'
# Installed by x120x-dkms — removed by uninstall.sh.
#
# Clean shutdown when the UPS battery reaches UPower's
# warning-level: action (2% SoC as configured in UPower.conf).
# To opt out, set HandleLowBattery=ignore in a later drop-in
# (e.g. 99-local.conf) or delete this file and restart
# systemd-logind.
[Login]
HandleLowBattery=poweroff
DROPIN_EOF
}

# -------------------------------------------------------------------------
# Battery-setting resolution
#
# Precedence per setting, independently: explicit CLI flag > value
# parsed from an existing /etc/modprobe.d/x120x.conf > built-in
# default (1000 / 0 / x120x).  An omitted flag means "keep what's
# there", so the natural update command — git pull && sudo bash
# install.sh — no longer resets pack capacity or a persisted Long
# Life mode.  This matches how the udev persistence already treats
# conservation_mode_default.  A value that parses but fails
# validation warns and falls back to the default for that setting
# only.  With no existing conf this reduces to flag-or-default,
# keeping the first install identical.
#
# Sets INPUT_MAH, CONSERVATION_DEFAULT, BOARD_VARIANT (+ *_SRC
# provenance strings and CHARGE_MODE_DEFAULT for the summary).
#
# Usage: resolve_battery_settings [CONF_FILE]
# -------------------------------------------------------------------------

resolve_battery_settings() {
    local conf="${1:-/etc/modprobe.d/x120x.conf}"
    local opts="" conf_mah="" conf_cons="" conf_board="" conf_soc=""

    if [ -f "${conf}" ]; then
        # Last "options x120x" line wins, mirroring modprobe semantics;
        # per-key extraction tolerates any order and missing keys.
        opts=$(sed -n 's/^options[[:space:]]\{1,\}x120x[[:space:]]\{1,\}//p' "${conf}" | tail -1)
        # `|| true` on each: under set -euo pipefail a grep with no match
        # returns 1, which pipefail propagates and set -e would treat as
        # fatal — aborting the installer on any old conf that predates a
        # given key (e.g. a pre-board or pre-soc_source config on upgrade).
        # The value is already empty in that case; we just want no abort.
        conf_mah=$(printf '%s\n' "${opts}" | grep -o 'battery_mah=[^[:space:]]*' | tail -1 | cut -d= -f2) || true
        conf_cons=$(printf '%s\n' "${opts}" | grep -o 'conservation_mode_default=[^[:space:]]*' | tail -1 | cut -d= -f2) || true
        conf_board=$(printf '%s\n' "${opts}" | grep -o 'board=[^[:space:]]*' | tail -1 | cut -d= -f2) || true
        conf_soc=$(printf '%s\n' "${opts}" | grep -o 'soc_source=[^[:space:]]*' | tail -1 | cut -d= -f2) || true
    fi

    if [ -n "${OPT_MAH}" ]; then
        INPUT_MAH="${OPT_MAH}"; MAH_SRC="from --battery-mah"
    elif [ -n "${conf_mah}" ]; then
        case "${conf_mah}" in
            *[!0-9]*|0)
                warn "Ignoring invalid battery_mah='${conf_mah}' in ${conf} — using default 1000"
                INPUT_MAH=1000; MAH_SRC="default"
                ;;
            *)
                INPUT_MAH="${conf_mah}"; MAH_SRC="kept from existing configuration"
                ;;
        esac
    else
        INPUT_MAH=1000; MAH_SRC="default"
    fi

    if [ -n "${OPT_CHARGE_MODE}" ]; then
        CONSERVATION_DEFAULT=0
        [ "${OPT_CHARGE_MODE}" = "longlife" ] && CONSERVATION_DEFAULT=1
        CONS_SRC="from --charge-mode"
    elif [ -n "${conf_cons}" ]; then
        case "${conf_cons}" in
            0|1)
                CONSERVATION_DEFAULT="${conf_cons}"
                CONS_SRC="kept from existing configuration"
                ;;
            *)
                warn "Ignoring invalid conservation_mode_default='${conf_cons}' in ${conf} — using default 0"
                CONSERVATION_DEFAULT=0; CONS_SRC="default"
                ;;
        esac
    else
        CONSERVATION_DEFAULT=0; CONS_SRC="default"
    fi

    if [ -n "${OPT_BOARD}" ]; then
        BOARD_VARIANT="${OPT_BOARD}"; BOARD_SRC="from --board"
    elif [ -n "${conf_board}" ]; then
        case "${conf_board}" in
            x120x|x728v2|x728v1|x708|x729)
                BOARD_VARIANT="${conf_board}"
                BOARD_SRC="kept from existing configuration"
                ;;
            *)
                warn "Ignoring invalid board='${conf_board}' in ${conf} — using default x120x"
                BOARD_VARIANT="x120x"; BOARD_SRC="default"
                ;;
        esac
    else
        BOARD_VARIANT="x120x"; BOARD_SRC="default"
    fi

    if [ -n "${OPT_SOC_SOURCE}" ]; then
        SOC_SOURCE="${OPT_SOC_SOURCE}"; SOC_SRC="from --soc-source"
    elif [ -n "${conf_soc}" ]; then
        case "${conf_soc}" in
            voltage|gauge)
                SOC_SOURCE="${conf_soc}"
                SOC_SRC="kept from existing configuration"
                ;;
            *)
                warn "Ignoring invalid soc_source='${conf_soc}' in ${conf} — using default voltage"
                SOC_SOURCE="voltage"; SOC_SRC="default"
                ;;
        esac
    else
        SOC_SOURCE="voltage"; SOC_SRC="default"
    fi

    # An if, not `[ ... ] && ...`: as the function's last command the
    # && form would make the function return 1 whenever the mode is
    # Fast, and under set -e that kills the caller silently.  Caught
    # by the v0.4.8 Phase-2 hardware validation.
    CHARGE_MODE_DEFAULT="fast"
    if [ "${CONSERVATION_DEFAULT}" = "1" ]; then
        CHARGE_MODE_DEFAULT="longlife"
    fi
}

# -------------------------------------------------------------------------
# Kernel floor
#
# The driver needs kernel 6.3+ (one-arg i2c .probe, the sys-off
# handler framework, void i2c .remove — see the README).  Check up
# front so an old image fails with the requirement and the fix,
# instead of a screenful of DKMS compiler errors that the
# troubleshooting section would misread as missing headers.  An
# unparseable version string warns and continues — never block on a
# weird uname.
#
# X120X_UNAME_R is overridable for testing (same safety rationale as
# RPI_EEPROM_CONFIG above).
# -------------------------------------------------------------------------

check_kernel_floor() {
    local kver="${X120X_UNAME_R:-$(uname -r)}" kmaj kmin
    kmaj=$(printf '%s' "${kver}" | sed -n 's/^\([0-9]\{1,\}\)\..*/\1/p')
    kmin=$(printf '%s' "${kver}" | sed -n 's/^[0-9]\{1,\}\.\([0-9]\{1,\}\).*/\1/p')
    if [ -z "${kmaj}" ] || [ -z "${kmin}" ]; then
        warn "Cannot parse kernel version '${kver}' — skipping the 6.3+ kernel check"
        return 0
    fi
    if [ "${kmaj}" -lt 6 ] || { [ "${kmaj}" -eq 6 ] && [ "${kmin}" -lt 3 ]; }; then
        die "Kernel ${kver} is too old — this driver needs kernel 6.3 or newer.  Run a fully-updated Raspberry Pi OS Bookworm or later (sudo apt update && sudo apt full-upgrade, then reboot) and try again."
    fi
}

# -------------------------------------------------------------------------
# Pi 5 bootloader EEPROM configuration
#
# POWER_OFF_ON_HALT=1 is required for the driver's core behaviour (clean
# power-off and automatic restart when mains returns); PSU_MAX_CURRENT=5000
# suppresses spurious low-power warnings.  Anyone installing this driver
# has an X120x UPS board, so both values are unconditionally correct — no
# negotiation with pre-existing values is needed.
#
# `rpi-eeprom-config --apply` only stages the update on the boot
# partition; the bootloader flashes it early during the next boot, so it
# lands with the same reboot this installer already requests.
#
# RPI_EEPROM_CONFIG and DT_MODEL_PATH are overridable so the test harness
# can inject mocks.
# -------------------------------------------------------------------------

configure_bootloader() {
    if [ "${SKIP_EEPROM}" -eq 1 ]; then
        info "Skipping bootloader EEPROM configuration (--skip-eeprom)"
        return 0
    fi

    # Only the Pi 5 needs these settings (the X1209 runs on Pi 3/4, where
    # the power-off GPIO pulse handles shutdown).  Read the NUL-terminated
    # device-tree model; skip on any other model, or if the file is absent
    # (a non-Pi build/test environment).
    local model=""
    if [ -r "${DT_MODEL_PATH}" ]; then
        model=$(tr -d '\0' < "${DT_MODEL_PATH}")
    fi
    case "${model}" in
        # Trailing space so "Raspberry Pi 500 ..." does not match "Pi 5".
        *"Raspberry Pi 5 "*) ;;
        *)
            info "Bootloader configuration not required on this model"
            return 0
            ;;
    esac

    if ! command -v "${RPI_EEPROM_CONFIG}" >/dev/null 2>&1; then
        warn "rpi-eeprom-config not found — bootloader EEPROM not configured."
        warn "Set POWER_OFF_ON_HALT=1 and PSU_MAX_CURRENT=5000 manually; see"
        warn "the README (Required bootloader settings)."
        return 0
    fi

    local cur new
    cur=$(mktemp -t x120x-eeprom-cur.XXXXXX) \
        || { warn "mktemp failed — skipping bootloader EEPROM setup."; return 0; }
    new=$(mktemp -t x120x-eeprom-new.XXXXXX) \
        || { warn "mktemp failed — skipping bootloader EEPROM setup."; rm -f -- "${cur}"; return 0; }
    chmod 600 -- "${cur}" "${new}"
    X120X_CLEANUP+=("${cur}" "${new}")

    if ! "${RPI_EEPROM_CONFIG}" > "${cur}" 2>/dev/null; then
        warn "Could not read current bootloader config — skipping EEPROM setup."
        rm -f -- "${cur}" "${new}"
        return 0
    fi

    # A successful-but-empty read (e.g. the unprivileged VideoCore mailbox
    # path returned nothing) must never be turned into a two-line config
    # that wipes BOOT_ORDER and everything else.
    if [ ! -s "${cur}" ]; then
        warn "Current bootloader config read back empty — skipping EEPROM setup."
        rm -f -- "${cur}" "${new}"
        return 0
    fi

    if grep -qx 'POWER_OFF_ON_HALT=1' "${cur}" \
       && grep -qx 'PSU_MAX_CURRENT=5000' "${cur}"; then
        ok "Bootloader already configured (POWER_OFF_ON_HALT=1, PSU_MAX_CURRENT=5000)"
        rm -f -- "${cur}" "${new}"
        return 0
    fi

    # Drop any existing values for our two keys, then append the required
    # ones.  This rewrites a differing prior value (e.g. =0) with no
    # special case, and leaves every other key untouched.  `|| true` keeps
    # set -e happy if grep -v matches nothing.
    grep -vE '^(POWER_OFF_ON_HALT|PSU_MAX_CURRENT)=' "${cur}" > "${new}" || true
    printf 'POWER_OFF_ON_HALT=1\nPSU_MAX_CURRENT=5000\n' >> "${new}"

    if "${RPI_EEPROM_CONFIG}" --apply "${new}" >/dev/null 2>&1; then
        ok "Bootloader update staged (POWER_OFF_ON_HALT=1, PSU_MAX_CURRENT=5000) — takes effect at the reboot below."
    else
        warn "rpi-eeprom-config --apply failed — configure the bootloader"
        warn "manually; see the README (Required bootloader settings)."
    fi

    rm -f -- "${cur}" "${new}"
    return 0
}

# -------------------------------------------------------------------------
# Argument parsing
# -------------------------------------------------------------------------

OPT_MAH=""
OPT_CHARGE_MODE=""
OPT_BOARD=""
OPT_SOC_SOURCE=""
SKIP_EEPROM=0

while [ $# -gt 0 ]; do
    case "$1" in
        --battery-mah)
            case "${2:-}" in
                ''|*[!0-9]*)
                    die "--battery-mah requires a positive integer (got: ${2:-<missing>})"
                    ;;
            esac
            # Strip leading zeros for cleanliness; reject 0.
            OPT_MAH=$((10#$2))
            [ "${OPT_MAH}" -gt 0 ] \
                || die "--battery-mah must be greater than zero"
            shift 2
            ;;
        --charge-mode)
            case "${2:-}" in
                '') die "--charge-mode requires a value  (use fast or longlife)" ;;
                fast|Fast|FAST)         OPT_CHARGE_MODE="fast" ;;
                longlife|LongLife|LONGLIFE|long-life|"Long Life") OPT_CHARGE_MODE="longlife" ;;
                *) die "Unknown charge mode: $2  (use fast or longlife)" ;;
            esac
            shift 2
            ;;
        --board)
            case "${2:-}" in
                '') die "--board requires a value  (use x120x, x728v2, x728v1, x708, x729)" ;;
                x120x) OPT_BOARD="$2" ;;
                x728v2|x728v1|x708|x729)
                    # The driver accepts board=$2, but no device tree
                    # overlay ships for these boards yet: the power-off
                    # GPIO pulse they need after shutdown cannot work, and
                    # the UPS would keep draining the pack indefinitely.
                    # Refuse rather than install a silently broken setup.
                    die "--board $2 is not installable yet: no $2 device tree overlay ships with this release, so the power-off pulse after shutdown cannot work.  See 'Experimental board support' in the README for the manual development path."
                    ;;
                *) die "Unknown board variant: $2  (use x120x, x728v2, x728v1, x708, x729)" ;;
            esac
            shift 2
            ;;
        --soc-source)
            case "${2:-}" in
                '') die "--soc-source requires a value  (use voltage or gauge)" ;;
                voltage|gauge) OPT_SOC_SOURCE="$2" ;;
                *) die "Unknown SoC source: $2  (use voltage or gauge)" ;;
            esac
            shift 2
            ;;
        --skip-eeprom)
            SKIP_EEPROM=1
            shift
            ;;
        --help|-h)
            echo "Usage: sudo bash install.sh [--battery-mah N] [--charge-mode fast|longlife] [--board x120x|x728v2|x728v1|x708|x729] [--soc-source voltage|gauge] [--skip-eeprom]"
            echo
            echo "  --soc-source   State-of-charge source: voltage (default) or gauge."
            echo "                 voltage uses an NMC open-circuit-voltage model (charge"
            echo "                 and discharge curves) that avoids the fuel gauge's"
            echo "                 near-full over-read; gauge uses the raw MAX17043 register."
            echo
            echo "  --skip-eeprom  Do not modify Raspberry Pi bootloader EEPROM settings"
            echo "                 (POWER_OFF_ON_HALT, PSU_MAX_CURRENT).  You are then"
            echo "                 responsible for configuring them manually — see README."
            echo
            echo "  --board        Only x120x is installable.  The x728v2/x728v1/x708/x729"
            echo "                 variants are refused until per-board device tree overlays"
            echo "                 ship — see 'Experimental board support' in the README."
            exit 0
            ;;
        *)
            die "Unknown option: $1  (use --help for usage)"
            ;;
    esac
done

# -------------------------------------------------------------------------
# Configuration
# -------------------------------------------------------------------------

PKG_NAME="x120x"
PKG_VERSION="0.5.9"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

# External command and device-tree model path — overridable for testing.
# Safe: install.sh runs as root, and setting root's environment already
# requires root, so these overrides grant no privilege a caller lacks.
# X120X_UNAME_R (see check_kernel_floor) follows the same pattern.
RPI_EEPROM_CONFIG="${RPI_EEPROM_CONFIG:-rpi-eeprom-config}"
DT_MODEL_PATH="${DT_MODEL_PATH:-/proc/device-tree/model}"

# Detect the firmware/boot path.  Raspberry Pi OS mounts the firmware
# partition at /boot/firmware (newer) or writes directly to /boot (older);
# Ubuntu for Raspberry Pi also mounts it at /boot/firmware.  All three put
# config.txt at the root of that directory.
if [ -f /boot/firmware/config.txt ]; then
    CONFIG_TXT="/boot/firmware/config.txt"
    BOOT_DIR="/boot/firmware"
elif [ -f /boot/config.txt ]; then
    CONFIG_TXT="/boot/config.txt"
    BOOT_DIR="/boot"
else
    die "Cannot find config.txt — is this a Raspberry Pi running Raspberry Pi OS or Ubuntu?"
fi

# Resolve the overlays directory.  Raspberry Pi OS keeps overlays in an
# "overlays" subdirectory; Ubuntu's flash-kernel layout keeps the active
# kernel's overlays under "current/overlays" instead.  Prefer the
# flash-kernel path when it exists (it is a Debian/Ubuntu-only convention
# and never present on Raspberry Pi OS), otherwise fall back to the
# standard location.
if [ -d "${BOOT_DIR}/current/overlays" ]; then
    OVERLAYS_DIR="${BOOT_DIR}/current/overlays"
elif [ -d "${BOOT_DIR}/overlays" ]; then
    OVERLAYS_DIR="${BOOT_DIR}/overlays"
else
    die "Cannot find an overlays directory under ${BOOT_DIR} (looked for current/overlays and overlays)"
fi

# -------------------------------------------------------------------------
# Pre-flight checks
# -------------------------------------------------------------------------

require_root

check_kernel_floor

info "x120x-dkms ${PKG_VERSION} installer"
info "Source: ${SRC_DIR}"
info "Config: ${CONFIG_TXT}"
echo

# Verify required files are present
for f in src/x120x.c src/Kbuild Makefile dkms.conf x120x-overlay.dts; do
    [ -f "${SRC_DIR}/${f}" ] || die "Missing file: ${f} — run this script from the repository root"
done

# -------------------------------------------------------------------------
# Step 1: Dependencies
# -------------------------------------------------------------------------

info "Step 1/10 — Installing dependencies..."
apt-get update \
    || warn "apt-get update failed — continuing with existing package index"
apt-get install -y dkms "linux-headers-$(uname -r)" \
    || die "apt-get install failed"
ok "Dependencies installed"

# -------------------------------------------------------------------------
# Step 2: Copy source to DKMS tree
# -------------------------------------------------------------------------

DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VERSION}"

info "Step 2/10 — Copying source to DKMS tree (${DKMS_SRC})..."
install_dkms_tree "${SRC_DIR}" "${DKMS_SRC}"
ok "Source copied"

# -------------------------------------------------------------------------
# Step 3: Build and install kernel module
# -------------------------------------------------------------------------

info "Step 3/10 — Building kernel module (this takes about a minute)..."

# Remove any previous installation of this version cleanly
if dkms status "${PKG_NAME}/${PKG_VERSION}" 2>/dev/null | grep -q .; then
    info "  Removing previous installation of ${PKG_NAME}/${PKG_VERSION}..."
    dkms remove "${PKG_NAME}/${PKG_VERSION}" --all 2>/dev/null || true
fi

# Remove any older installed versions of this driver that may have been
# left behind by previous installs (e.g. x120x/0.1.0, x120x/0.2.0).
# These show up as 'built' or 'installed' in dkms status but are no
# longer needed once the current version is in place.
while IFS= read -r old_ver; do
    info "  Removing leftover ${PKG_NAME}/${old_ver}..."
    dkms remove "${PKG_NAME}/${old_ver}" --all 2>/dev/null || true
    rm -rf "/usr/src/${PKG_NAME}-${old_ver}"
    ok "  Removed ${PKG_NAME}/${old_ver}"
done < <(dkms status "${PKG_NAME}" 2>/dev/null \
    | grep -oP "${PKG_NAME}/\K[0-9]+\.[0-9]+\.[0-9]+" \
    | grep -v "^${PKG_VERSION}$" \
    | sort -uV)

# `dkms add` errors out if this version's tree is already registered
# (e.g. a previous run aborted after add but before build/install, or the
# remove above could not fully clear a wedged entry).  That is not a real
# failure — the source is present and can still be built — so tolerate it
# and only treat a genuinely absent tree as fatal.  Without this an
# already-added tree aborts the whole installer before the overlay is
# (re)installed in Step 6, leaving a broken driver a reinstall cannot fix.
if ! add_out=$(dkms add "${PKG_NAME}/${PKG_VERSION}" 2>&1); then
    if dkms status "${PKG_NAME}/${PKG_VERSION}" 2>/dev/null | grep -q .; then
        info "  ${PKG_NAME}/${PKG_VERSION} already in the DKMS tree — continuing"
    else
        printf '%s\n' "${add_out}" >&2
        die "dkms add failed"
    fi
fi
dkms build   "${PKG_NAME}/${PKG_VERSION}" \
    || die "dkms build failed — check /var/lib/dkms/${PKG_NAME}/${PKG_VERSION}/build/make.log"
dkms install "${PKG_NAME}/${PKG_VERSION}" \
    || die "dkms install failed"

ok "Kernel module built and installed"

# -------------------------------------------------------------------------
# Step 4: Compile device tree overlay
# -------------------------------------------------------------------------

info "Step 4/10 — Compiling device tree overlay..."

if ! command -v dtc &>/dev/null; then
    info "  dtc not found, installing device-tree-compiler..."
    apt-get install -y device-tree-compiler \
        || die "Failed to install device-tree-compiler"
fi

# Compile the overlay in a root-owned tmpdir rather than the source
# directory.  If SRC_DIR happens to live on a path an unprivileged user
# can write to, compiling in-place would open a TOCTOU window between
# the dtc output and the cp into /boot/firmware/overlays/.  A private
# tmpdir created by root closes that window.
DTBO_TMPDIR=$(mktemp -d -t x120x-dtbo.XXXXXX) \
    || die "Failed to create temporary directory for overlay compile"
chmod 700 "${DTBO_TMPDIR}"
X120X_CLEANUP+=("${DTBO_TMPDIR}")

dtc -@ -I dts -O dtb \
    -o "${DTBO_TMPDIR}/x120x.dtbo" \
    "${SRC_DIR}/x120x-overlay.dts" \
    || die "Failed to compile device tree overlay"
ok "Overlay compiled"

# -------------------------------------------------------------------------
# Step 5: Write battery configuration to modprobe.d
# -------------------------------------------------------------------------

MODPROBE_CONF="/etc/modprobe.d/x120x.conf"
resolve_battery_settings "${MODPROBE_CONF}"
info "battery_mah=${INPUT_MAH} (${MAH_SRC})"
info "conservation_mode_default=${CONSERVATION_DEFAULT} (${CONS_SRC})"
info "board=${BOARD_VARIANT} (${BOARD_SRC})"
info "soc_source=${SOC_SOURCE} (${SOC_SRC})"

# Warn if an experimental board is in effect — reachable via the
# --board flag once per-board overlays ship, or today via a board=
# value preserved from a hand-edited conf (the manual dev path).
if [ "${BOARD_VARIANT}" != "x120x" ]; then
    warn "Board variant ${BOARD_VARIANT} is EXPERIMENTAL and untested."
    warn "Validate correct operation before relying on this driver."
fi

# Long Life not supported on boards without charge control.  Applies
# to the effective values, whichever source they came from.
if [ "${BOARD_VARIANT}" = "x728v1" ] || [ "${BOARD_VARIANT}" = "x708" ] || [ "${BOARD_VARIANT}" = "x729" ]; then
    if [ "${CONSERVATION_DEFAULT}" = "1" ]; then
        warn "Long Life mode ignored: ${BOARD_VARIANT} has no charge control GPIO"
        CHARGE_MODE_DEFAULT="fast"
        CONSERVATION_DEFAULT=0
    fi
fi

info "Step 5/10 — Writing battery configuration to ${MODPROBE_CONF}..."
cat > "${MODPROBE_CONF}" << MODPROBE_EOF
# x120x driver configuration
# Generated by x120x-dkms installer — edit to change battery parameters.
#
# battery_mah     — total pack capacity in mAh
#                   (number of cells × per-cell capacity)
# soc_source      — state-of-charge source: voltage (NMC OCV model, default)
#                   or gauge (raw MAX17043 register)
#
# After editing, reload the driver:
#   sudo rmmod x120x && sudo modprobe x120x
# Or simply reboot.

options x120x battery_mah=${INPUT_MAH} conservation_mode_default=${CONSERVATION_DEFAULT} board=${BOARD_VARIANT} soc_source=${SOC_SOURCE}
MODPROBE_EOF
ok "Battery configuration written"

# -------------------------------------------------------------------------
# Step 6: Install overlay  (renumbered)
# -------------------------------------------------------------------------

info "Step 6/10 — Installing device tree overlay to ${OVERLAYS_DIR}..."
cp "${DTBO_TMPDIR}/x120x.dtbo" "${OVERLAYS_DIR}/" \
    || die "Failed to copy overlay to ${OVERLAYS_DIR}"
ok "Overlay installed"

# Overlay persistence across package updates (Ubuntu / flash-kernel only).
#
# On Ubuntu's flash-kernel layout the overlays directory
# (…/current/overlays) is repopulated from the kernel/firmware packages on
# every `apt upgrade`, which deletes this out-of-tree overlay.  config.txt
# keeps its `dtoverlay=x120x` line, so the reference then dangles and the
# driver silently fails to load after the next reboot (issue #5).  There is
# no supported "user overlay" location that survives, so stash a copy and
# register an apt hook that restores it at the end of any transaction that
# removed it.  Raspberry Pi OS (plain overlays/) does not repopulate this
# way and is deliberately left untouched — the block below is skipped there,
# so a Raspberry Pi OS install is byte-for-byte unchanged.
case "${OVERLAYS_DIR}" in
*/current/overlays)
    if [ -d /etc/apt/apt.conf.d ]; then
        OVERLAY_STASH="/usr/local/lib/x120x-overlay.dtbo"
        RESTORE_SCRIPT="/usr/local/lib/x120x-restore-overlay.sh"
        APT_HOOK="/etc/apt/apt.conf.d/99-x120x-overlay"

        mkdir -p /usr/local/lib
        cp "${DTBO_TMPDIR}/x120x.dtbo" "${OVERLAY_STASH}" \
            || die "Failed to stash overlay to ${OVERLAY_STASH}"

        cat > "${RESTORE_SCRIPT}" << 'RESTORE_EOF'
#!/bin/sh
# x120x-restore-overlay.sh — restore the x120x device-tree overlay if a
# package update (flash-kernel repopulating …/current/overlays) deleted it.
# Called from an apt DPkg::Post-Invoke hook after every transaction.
#
# This runs inside apt, so it must NEVER fail a package operation: it always
# exits 0 and changes nothing unless the overlay is genuinely configured yet
# missing and a stashed copy exists.  Paths default to the real locations
# and are overridable (X120X_*) for testing only.
set +e
STASH="${X120X_OVERLAY_STASH:-/usr/local/lib/x120x-overlay.dtbo}"
BOOT_DIR="${X120X_BOOT_DIR:-}"
if [ -z "${BOOT_DIR}" ]; then
    if [ -f /boot/firmware/config.txt ]; then
        BOOT_DIR=/boot/firmware
    elif [ -f /boot/config.txt ]; then
        BOOT_DIR=/boot
    else
        exit 0
    fi
fi
CONFIG_TXT="${X120X_CONFIG_TXT:-${BOOT_DIR}/config.txt}"
# Resolve the overlays directory exactly as install.sh does.
if [ -d "${BOOT_DIR}/current/overlays" ]; then
    OVERLAYS_DIR="${BOOT_DIR}/current/overlays"
elif [ -d "${BOOT_DIR}/overlays" ]; then
    OVERLAYS_DIR="${BOOT_DIR}/overlays"
else
    exit 0
fi
# Act only when the overlay is enabled in config.txt, its file is gone, and
# we have a stash to restore from — otherwise this is a no-op.
[ -f "${STASH}" ]      || exit 0
[ -f "${CONFIG_TXT}" ] || exit 0
grep -qE '^[[:space:]]*dtoverlay=x120x([[:space:]]|$)' "${CONFIG_TXT}" || exit 0
[ -f "${OVERLAYS_DIR}/x120x.dtbo" ] && exit 0
if cp "${STASH}" "${OVERLAYS_DIR}/x120x.dtbo" 2>/dev/null; then
    logger -t x120x "restored device-tree overlay to ${OVERLAYS_DIR} after a package update" 2>/dev/null || true
fi
exit 0
RESTORE_EOF
        chmod 755 "${RESTORE_SCRIPT}"

        cat > "${APT_HOOK}" << 'APTHOOK_EOF'
// Installed by x120x-dkms — removed by uninstall.sh.
// Restore the x120x UPS HAT device-tree overlay if a package update
// (flash-kernel) deleted it from the boot partition.  The helper exits 0
// and is a no-op unless the overlay is configured but missing.
DPkg::Post-Invoke { "/usr/local/lib/x120x-restore-overlay.sh || true"; };
APTHOOK_EOF
        ok "Installed overlay-persistence hook (survives Ubuntu package updates)"
    fi
    ;;
esac

# -------------------------------------------------------------------------
# Step 7: Enable overlay in config.txt
# -------------------------------------------------------------------------

info "Step 7/10 — Enabling overlay in ${CONFIG_TXT}..."

if grep -qE '^[[:space:]]*dtoverlay=x120x([[:space:]]|$)' "${CONFIG_TXT}"; then
    ok "dtoverlay=x120x already present in ${CONFIG_TXT}"
else
    # Always append at the bottom. If the last section header in the
    # file is not [all], insert [all] first so the overlay applies to
    # all boards unconditionally (including Pi 5).
    LAST_SECTION=$(grep "^\[" "${CONFIG_TXT}" | tail -1)
    if [ "${LAST_SECTION}" != "[all]" ]; then
        printf '\n[all]\n' >> "${CONFIG_TXT}"
    fi
    printf '# SupTronics X120x UPS HAT driver\ndtoverlay=x120x\n'         >> "${CONFIG_TXT}"
    ok "Added dtoverlay=x120x to ${CONFIG_TXT}"
fi

# Add pull-up on GPIO6 (AC-present signal).
# The X1206 drives GPIO6 high when AC is present and actively pulls it
# low on AC loss.  Without a pull-up the pin can float low at boot
# before the hardware asserts the signal, causing the driver to falsely
# report ac_online=0 and trigger an unnecessary shutdown even with the
# charger connected.
if grep -qE '^[[:space:]]*gpio=6=pu([[:space:]]|$)' "${CONFIG_TXT}"; then
    ok "gpio=6=pu already present in ${CONFIG_TXT}"
else
    printf 'gpio=6=pu\n' >> "${CONFIG_TXT}"
    ok "Added gpio=6=pu to ${CONFIG_TXT}"
fi

# -------------------------------------------------------------------------
# Step 8: Configure bootloader EEPROM (Pi 5 only)
# -------------------------------------------------------------------------

info "Step 8/10 — Configuring bootloader (Raspberry Pi 5)..."

configure_bootloader

# -------------------------------------------------------------------------
# Step 9: Configure low-battery shutdown via systemd-logind
# -------------------------------------------------------------------------

info "Step 9/10 — Configuring low-battery shutdown..."

# Migration: strip anything an earlier installer put into logind.conf
# itself — the marker-wrapped block (pre-drop-in versions) and bare
# legacy lines (pre-marker versions).  On a fresh system both are
# no-ops, and afterwards the packaged file is pristine again.
clean_legacy_logind "${LOGIND_CONF:=/etc/systemd/logind.conf}"
remove_ini_block "${LOGIND_CONF}" "logind-low-battery"

# Drop-ins under logind.conf.d override logind.conf, so the setting
# wins regardless of what the packaged file says.
install_logind_dropin "${LOGIND_DROPIN:=/etc/systemd/logind.conf.d/90-x120x.conf}"
ok "HandleLowBattery=poweroff set via ${LOGIND_DROPIN}"

# UPower configuration:
#   CriticalPowerAction=PowerOff  — HybridSleep hangs on Raspberry Pi.
#   UsePercentageForPolicy=true   — Act on SoC %, not time-to-empty
#                                   (a UPS HAT reports no time estimate).
#   PercentageAction=2            — Power off at 2% SoC.  Debian/RPi-OS
#                                   ship PercentageAction=0, which would
#                                   only act at 0% — no margin above the
#                                   3.20 V floor.  GKeyFile honours the
#                                   last value, so this overrides the 0.
#   NoPollBatteries=true          — The driver sends uevents on all state
#                                   changes and on a 30s heartbeat.  UPower
#                                   polling the kernel independently causes
#                                   races that produce spurious 0%/unknown
#                                   entries in the history files and corrupt
#                                   gnome-power-statistics graphs.
UPOWER_CONF="/etc/UPower/UPower.conf"
if [ -f "${UPOWER_CONF}" ]; then
    # Strip legacy bare lines first (no-op on a fresh system).
    clean_legacy_upower "${UPOWER_CONF}"

    install_ini_block "${UPOWER_CONF}" "UPower" "upower-pi-tweaks" \
        "# HybridSleep hangs on Raspberry Pi — use PowerOff instead." \
        "CriticalPowerAction=PowerOff" \
        "# Act on battery percentage; a UPS HAT reports no time-to-empty." \
        "UsePercentageForPolicy=true" \
        "# Fire the PowerOff action at 2% SoC.  Debian/RPi-OS default" \
        "# PercentageAction=0 would only act at 0% — no margin above the" \
        "# 3.20 V floor.  UPower (GKeyFile) honours this last value." \
        "PercentageAction=2" \
        "# Driver sends uevents on all state changes; polling causes" \
        "# races that produce spurious 0%/unknown history entries." \
        "NoPollBatteries=true"
    ok "UPower tweaks installed in ${UPOWER_CONF}"
    systemctl restart upower 2>/dev/null || true
else
    warn "UPower config not found at ${UPOWER_CONF} — skipping UPower configuration"
fi

# -------------------------------------------------------------------------
# Step 10: Persist charge mode across reboots
# -------------------------------------------------------------------------

info "Step 10/10 — Installing charge mode persistence..."

PERSIST_SCRIPT="/usr/local/lib/x120x-persist-mode.sh"
UDEV_RULE="/etc/udev/rules.d/90-x120x-persist.rules"

mkdir -p /usr/local/lib

cat > "${PERSIST_SCRIPT}" << 'PERSIST_EOF'
#!/bin/sh
# x120x-persist-mode.sh — called by udev when charge_type changes.
# Writes conservation_mode_default to /etc/modprobe.d/x120x.conf so
# the charge mode (Fast or Long Life) survives reboots.
# CONF and the charge_type source default to the real paths but are
# overridable (X120X_CONF, X120X_CHARGE_TYPE_PATH) for testing.  Safe:
# udev runs this as root and setting root's environment already requires
# root, so the overrides grant no privilege a caller lacks.
CONF="${X120X_CONF:-/etc/modprobe.d/x120x.conf}"
CHARGE_TYPE_PATH="${X120X_CHARGE_TYPE_PATH:-/sys/class/power_supply/x120x-charger/charge_type}"
CHARGE_TYPE=$(cat "$CHARGE_TYPE_PATH" 2>/dev/null)
case "$CHARGE_TYPE" in
    "Long Life") MODE=1 ;;
    *)           MODE=0 ;;
esac
if [ -f "$CONF" ]; then
    if grep -q "conservation_mode_default" "$CONF"; then
        sed -i "s/conservation_mode_default=[0-9]*/conservation_mode_default=${MODE}/" "$CONF"
    else
        sed -i "s/^options x120x /options x120x conservation_mode_default=${MODE} /" "$CONF"
    fi
fi
PERSIST_EOF
chmod 755 "${PERSIST_SCRIPT}"
ok "Installed persistence script to ${PERSIST_SCRIPT}"

cat > "${UDEV_RULE}" << 'UDEV_EOF'
# Persist x120x charge mode to /etc/modprobe.d/x120x.conf on every change
ACTION=="change", SUBSYSTEM=="power_supply", KERNEL=="x120x-charger",     RUN+="/usr/local/lib/x120x-persist-mode.sh"
UDEV_EOF
udevadm control --reload-rules 2>/dev/null || true
ok "Installed charge mode persistence rule to ${UDEV_RULE}"

# -------------------------------------------------------------------------
# Done
# -------------------------------------------------------------------------

echo
echo -e "${GRN}${BLD}Installation complete.${RST}"
echo
echo -e "  ${BLD}Next step:${RST} reboot your Raspberry Pi"
echo
echo -e "    sudo reboot"
echo
echo -e "  ${BLD}After reboot, verify with:${RST}"
echo
echo -e "    dmesg | grep x120x"
echo -e "    upower -i /org/freedesktop/UPower/devices/battery_x120x_battery"
echo
echo -e "  ${BLD}Battery configuration written to:${RST} ${MODPROBE_CONF}"
echo
echo -e "    battery_mah              = ${INPUT_MAH} mAh"
echo -e "    conservation_mode_default = ${CONSERVATION_DEFAULT}  (${CHARGE_MODE_DEFAULT} mode)"
echo -e "    board                    = ${BOARD_VARIANT}"
echo
echo -e "  To change these values, edit ${MODPROBE_CONF} and reboot."
echo
