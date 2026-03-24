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
#   --voltage-empty-mv N   Cell voltage at shutdown/Critical threshold in mV (default: 3200)
#                          Raise to e.g. 4100 to test the logind shutdown path
#
# Copyright (C) 2026 Edvard Fielding <mor-lock@users.noreply.github.com>
# SPDX-License-Identifier: GPL-2.0-or-later

set -e

# -------------------------------------------------------------------------
# Helpers
# -------------------------------------------------------------------------

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
BLD='\033[1m'
RST='\033[0m'

info()  { echo -e "${BLD}[x120x]${RST} $*"; }
ok()    { echo -e "${GRN}[x120x]${RST} $*"; }
warn()  { echo -e "${YLW}[x120x] WARNING:${RST} $*"; }
die()   { echo -e "${RED}[x120x] ERROR:${RST} $*" >&2; exit 1; }

require_root() {
    [ "$(id -u)" -eq 0 ] || die "This script must be run with sudo: sudo bash install.sh"
}

# -------------------------------------------------------------------------
# Argument parsing
# -------------------------------------------------------------------------

OPT_MAH=""
OPT_VEMPTY=""

while [ $# -gt 0 ]; do
    case "$1" in
        --battery-mah)
            OPT_MAH="$2"
            shift 2
            ;;
        --voltage-empty-mv)
            OPT_VEMPTY="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: sudo bash install.sh [--battery-mah N] [--voltage-empty-mv N]"
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
PKG_VERSION="0.1.0"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

# Detect Pi model for correct boot path
if [ -f /boot/firmware/config.txt ]; then
    CONFIG_TXT="/boot/firmware/config.txt"
    OVERLAYS_DIR="/boot/firmware/overlays"
elif [ -f /boot/config.txt ]; then
    CONFIG_TXT="/boot/config.txt"
    OVERLAYS_DIR="/boot/overlays"
else
    die "Cannot find config.txt — is this a Raspberry Pi running Raspberry Pi OS?"
fi

# -------------------------------------------------------------------------
# Pre-flight checks
# -------------------------------------------------------------------------

require_root

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

info "Step 1/9 — Installing dependencies..."
apt-get install -y dkms raspberrypi-kernel-headers \
    || die "apt-get install failed"
ok "Dependencies installed"

# -------------------------------------------------------------------------
# Step 2: Copy source to DKMS tree
# -------------------------------------------------------------------------

DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VERSION}"

info "Step 2/9 — Copying source to DKMS tree (${DKMS_SRC})..."
rm -rf "${DKMS_SRC}"
cp -r "${SRC_DIR}" "${DKMS_SRC}"
ok "Source copied"

# -------------------------------------------------------------------------
# Step 3: Build and install kernel module
# -------------------------------------------------------------------------

info "Step 3/9 — Building kernel module (this takes about a minute)..."

# Remove any previous installation cleanly
if dkms status "${PKG_NAME}/${PKG_VERSION}" 2>/dev/null | grep -q .; then
    info "  Removing previous installation..."
    dkms remove "${PKG_NAME}/${PKG_VERSION}" --all 2>/dev/null || true
fi

dkms add     "${PKG_NAME}/${PKG_VERSION}" \
    || die "dkms add failed"
dkms build   "${PKG_NAME}/${PKG_VERSION}" \
    || die "dkms build failed — check /var/lib/dkms/${PKG_NAME}/${PKG_VERSION}/build/make.log"
dkms install "${PKG_NAME}/${PKG_VERSION}" \
    || die "dkms install failed"

ok "Kernel module built and installed"

# -------------------------------------------------------------------------
# Step 4: Compile device tree overlay
# -------------------------------------------------------------------------

info "Step 4/9 — Compiling device tree overlay..."

if ! command -v dtc &>/dev/null; then
    info "  dtc not found, installing device-tree-compiler..."
    apt-get install -y device-tree-compiler \
        || die "Failed to install device-tree-compiler"
fi

cd "${SRC_DIR}"
dtc -@ -I dts -O dtb -o x120x.dtbo x120x-overlay.dts \
    || die "Failed to compile device tree overlay"
ok "Overlay compiled"

# -------------------------------------------------------------------------
# Step 5: Write battery configuration to modprobe.d
# -------------------------------------------------------------------------

MODPROBE_CONF="/etc/modprobe.d/x120x.conf"
INPUT_MAH="${OPT_MAH:-1000}"
INPUT_VEMPTY="${OPT_VEMPTY:-3200}"

info "Step 5/9 — Writing battery configuration to ${MODPROBE_CONF}..."
cat > "${MODPROBE_CONF}" << MODPROBE_EOF
# x120x driver configuration
# Generated by x120x-dkms installer — edit to change battery parameters.
#
# battery_mah     — total pack capacity in mAh
#                   (number of cells × per-cell capacity)
# voltage_empty_mv — cell voltage at shutdown threshold in mV (typically 3200)
#
# After editing, reload the driver:
#   sudo rmmod x120x && sudo modprobe x120x
# Or simply reboot.

options x120x battery_mah=${INPUT_MAH} voltage_empty_mv=${INPUT_VEMPTY}
MODPROBE_EOF
ok "Battery configuration written"

# -------------------------------------------------------------------------
# Step 6: Install overlay  (renumbered)
# -------------------------------------------------------------------------

info "Step 6/9 — Installing device tree overlay to ${OVERLAYS_DIR}..."
cp x120x.dtbo "${OVERLAYS_DIR}/" \
    || die "Failed to copy overlay to ${OVERLAYS_DIR}"
ok "Overlay installed"

# -------------------------------------------------------------------------
# Step 7: Enable overlay in config.txt
# -------------------------------------------------------------------------

info "Step 7/9 — Enabling overlay in ${CONFIG_TXT}..."

if grep -q "dtoverlay=x120x" "${CONFIG_TXT}"; then
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

# -------------------------------------------------------------------------
# Step 8: Bootloader check (Pi 5 only)
# -------------------------------------------------------------------------

info "Step 8/9 — Checking bootloader configuration..."

if grep -q "Raspberry Pi 5" /proc/cpuinfo 2>/dev/null; then
    CURRENT_EEPROM=$(rpi-eeprom-config 2>/dev/null || true)
    NEEDS_UPDATE=0

    if echo "${CURRENT_EEPROM}" | grep -q "POWER_OFF_ON_HALT=1"; then
        ok "POWER_OFF_ON_HALT=1 already set"
    else
        NEEDS_UPDATE=1
        warn "POWER_OFF_ON_HALT is not set to 1."
        warn "Without this, the Pi may not restart automatically after a"
        warn "safe shutdown triggered by the UPS on battery exhaustion."
    fi

    if echo "${CURRENT_EEPROM}" | grep -q "PSU_MAX_CURRENT=5000"; then
        ok "PSU_MAX_CURRENT=5000 already set"
    else
        NEEDS_UPDATE=1
        warn "PSU_MAX_CURRENT is not set to 5000."
        warn "Without this, the Pi may log power supply warnings when"
        warn "drawing high current through the UPS board."
    fi

    if [ "${NEEDS_UPDATE}" -eq 1 ]; then
        warn ""
        warn "To fix, run: sudo rpi-eeprom-config -e"
        warn "Then add any missing lines:"
        warn "    POWER_OFF_ON_HALT=1"
        warn "    PSU_MAX_CURRENT=5000"
        warn "Save, exit, and reboot."
    fi
else
    ok "Not a Pi 5 — bootloader check skipped"

fi

# -------------------------------------------------------------------------
# Step 9: Configure low-battery shutdown via systemd-logind
# -------------------------------------------------------------------------

info "Step 9/9 — Configuring low-battery shutdown..."

LOGIND_CONF="/etc/systemd/logind.conf"
if grep -q "^HandleLowBattery=poweroff" "${LOGIND_CONF}" 2>/dev/null; then
    ok "HandleLowBattery=poweroff already set in ${LOGIND_CONF}"
else
    # Append a drop-in line. Comment out any existing HandleLowBattery line
    # first to avoid duplicates.
    sed -i 's/^HandleLowBattery=/#HandleLowBattery=/' "${LOGIND_CONF}" 2>/dev/null || true
    echo "" >> "${LOGIND_CONF}"
    echo "# Added by x120x-dkms installer — clean shutdown when battery reaches" >> "${LOGIND_CONF}"
    echo "# capacity_level=Critical (cell voltage ≤ 3.20 V on battery)." >> "${LOGIND_CONF}"
    echo "# To disable: set HandleLowBattery=ignore in ${LOGIND_CONF}" >> "${LOGIND_CONF}"
    echo "HandleLowBattery=poweroff" >> "${LOGIND_CONF}"
    ok "HandleLowBattery=poweroff set in ${LOGIND_CONF}"
fi

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
echo -e "    battery_mah      = ${INPUT_MAH} mAh"
echo -e "    voltage_empty_mv = ${INPUT_VEMPTY} mV"
echo
echo -e "  To change these values, edit ${MODPROBE_CONF} and reboot."
echo
