#!/usr/bin/env bash
# uninstall.sh — x120x-dkms uninstallation script
#
# Removes the SupTronics X120x UPS HAT kernel driver and all changes
# made by install.sh.  Run from the repository root:
#
#   sudo bash uninstall.sh
#
# What this script removes:
#   - DKMS kernel module (all installed kernel versions)
#   - DKMS source tree from /usr/src/
#   - Device tree overlay from /boot/firmware/overlays/ (or /boot/overlays/)
#   - dtoverlay=x120x and gpio=6=pu lines from config.txt
#   - /etc/modprobe.d/x120x.conf
#   - Charge mode persistence script and udev rule
#   - HandleLowBattery=poweroff from /etc/systemd/logind.conf
#   - CriticalPowerAction=PowerOff and NoPollBatteries=true from UPower.conf
#
# What this script does NOT touch:
#   - The raspberrypi-kernel-headers and dkms packages (installed as
#     system dependencies; removing them may affect other software)
#   - Bootloader EEPROM settings (POWER_OFF_ON_HALT, PSU_MAX_CURRENT)
#   - Any config.txt lines not added by the installer
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
    [ "$(id -u)" -eq 0 ] || die "This script must be run with sudo: sudo bash uninstall.sh"
}

# -------------------------------------------------------------------------
# Configuration
# -------------------------------------------------------------------------

PKG_NAME="x120x"
PKG_VERSION="0.3.0"

# Detect Pi model for correct boot path
if [ -f /boot/firmware/config.txt ]; then
    CONFIG_TXT="/boot/firmware/config.txt"
    OVERLAYS_DIR="/boot/firmware/overlays"
elif [ -f /boot/config.txt ]; then
    CONFIG_TXT="/boot/config.txt"
    OVERLAYS_DIR="/boot/overlays"
else
    warn "Cannot find config.txt — skipping overlay and config.txt cleanup"
    CONFIG_TXT=""
    OVERLAYS_DIR=""
fi

require_root

info "x120x-dkms ${PKG_VERSION} uninstaller"
echo

# -------------------------------------------------------------------------
# Step 1: Unload module if currently loaded
# -------------------------------------------------------------------------

info "Step 1/7 — Unloading kernel module (if loaded)..."

if lsmod | grep -q "^x120x "; then
    rmmod x120x 2>/dev/null \
        || warn "Could not unload x120x module — it may be in use. Continuing anyway."
    ok "Module unloaded"
else
    ok "Module not currently loaded"
fi

# -------------------------------------------------------------------------
# Step 2: Remove DKMS module
# -------------------------------------------------------------------------

info "Step 2/7 — Removing DKMS kernel module..."

if dkms status "${PKG_NAME}/${PKG_VERSION}" 2>/dev/null | grep -q .; then
    dkms remove "${PKG_NAME}/${PKG_VERSION}" --all \
        || warn "dkms remove reported an error — continuing anyway"
    ok "DKMS module removed"
else
    ok "No DKMS installation found for ${PKG_NAME}/${PKG_VERSION}"
fi

# Remove DKMS source tree
DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VERSION}"
if [ -d "${DKMS_SRC}" ]; then
    rm -rf "${DKMS_SRC}"
    ok "Removed DKMS source tree: ${DKMS_SRC}"
else
    ok "DKMS source tree not found (already removed)"
fi

# -------------------------------------------------------------------------
# Step 3: Remove device tree overlay and config.txt entries
# -------------------------------------------------------------------------

info "Step 3/7 — Removing device tree overlay and config.txt entries..."

if [ -n "${OVERLAYS_DIR}" ] && [ -f "${OVERLAYS_DIR}/x120x.dtbo" ]; then
    rm -f "${OVERLAYS_DIR}/x120x.dtbo"
    ok "Removed ${OVERLAYS_DIR}/x120x.dtbo"
else
    ok "Overlay not found (already removed)"
fi

if [ -n "${CONFIG_TXT}" ] && [ -f "${CONFIG_TXT}" ]; then
    # Remove the dtoverlay=x120x line and its comment
    sed -i '/^# SupTronics X120x UPS HAT driver$/d' "${CONFIG_TXT}"
    sed -i '/^dtoverlay=x120x$/d'                   "${CONFIG_TXT}"

    # Remove the gpio=6=pu pull-up line added by the installer
    sed -i '/^gpio=6=pu$/d' "${CONFIG_TXT}"

    # Remove any [all] section header the installer may have added
    # (only safe to remove if the installer added it; we match the exact
    # pattern: a blank line followed by [all] at end of file, with our
    # comment following immediately after — too fragile to guess.
    # Instead just remove a trailing bare [all] if the two lines
    # immediately below it are now gone, leaving it orphaned.)
    # Safest approach: remove a [all] line only if it is now followed
    # immediately by another section header or end of file.
    perl -i -0pe 's/\n\[all\]\n(?=\[|\z)/\n/g' "${CONFIG_TXT}" 2>/dev/null || true

    ok "Removed x120x entries from ${CONFIG_TXT}"
else
    ok "config.txt not found — skipping"
fi

# -------------------------------------------------------------------------
# Step 4: Remove modprobe configuration
# -------------------------------------------------------------------------

info "Step 4/7 — Removing modprobe configuration..."

MODPROBE_CONF="/etc/modprobe.d/x120x.conf"
if [ -f "${MODPROBE_CONF}" ]; then
    rm -f "${MODPROBE_CONF}"
    ok "Removed ${MODPROBE_CONF}"
else
    ok "Modprobe config not found (already removed)"
fi

# -------------------------------------------------------------------------
# Step 5: Remove charge mode persistence
# -------------------------------------------------------------------------

info "Step 5/7 — Removing charge mode persistence..."

PERSIST_SCRIPT="/usr/local/lib/x120x-persist-mode.sh"
UDEV_RULE="/etc/udev/rules.d/90-x120x-persist.rules"

if [ -f "${PERSIST_SCRIPT}" ]; then
    rm -f "${PERSIST_SCRIPT}"
    ok "Removed ${PERSIST_SCRIPT}"
else
    ok "Persistence script not found (already removed)"
fi

if [ -f "${UDEV_RULE}" ]; then
    rm -f "${UDEV_RULE}"
    udevadm control --reload-rules 2>/dev/null || true
    ok "Removed ${UDEV_RULE}"
else
    ok "udev rule not found (already removed)"
fi

# -------------------------------------------------------------------------
# Step 6: Restore logind.conf
# -------------------------------------------------------------------------

info "Step 6/7 — Restoring logind.conf..."

LOGIND_CONF="/etc/systemd/logind.conf"
if [ -f "${LOGIND_CONF}" ]; then
    # Remove the lines added by the installer
    sed -i '/^# Added by x120x-dkms installer.*$/d'        "${LOGIND_CONF}"
    sed -i '/^# capacity_level=Critical.*$/d'              "${LOGIND_CONF}"
    sed -i '/^# To disable: set HandleLowBattery.*$/d'     "${LOGIND_CONF}"
    sed -i '/^HandleLowBattery=poweroff$/d'                "${LOGIND_CONF}"

    # Restore any previously commented-out HandleLowBattery line
    sed -i 's/^#HandleLowBattery=/HandleLowBattery=/'      "${LOGIND_CONF}"

    ok "Restored ${LOGIND_CONF}"
else
    ok "logind.conf not found — skipping"
fi

# -------------------------------------------------------------------------
# Step 7: Restore UPower configuration
# -------------------------------------------------------------------------

info "Step 7/7 — Restoring UPower configuration..."

UPOWER_CONF="/etc/UPower/UPower.conf"
if [ -f "${UPOWER_CONF}" ]; then
    # Remove lines added by the installer
    sed -i '/^# Added by x120x-dkms installer.*$/d'                "${UPOWER_CONF}"
    sed -i '/^# HybridSleep hangs on Raspberry Pi.*$/d'            "${UPOWER_CONF}"
    sed -i '/^CriticalPowerAction=PowerOff$/d'                     "${UPOWER_CONF}"
    sed -i '/^# driver sends uevents.*$/d'                         "${UPOWER_CONF}"
    sed -i '/^NoPollBatteries=true$/d'                             "${UPOWER_CONF}"

    # Restore previously commented-out settings
    sed -i 's/^#CriticalPowerAction=/CriticalPowerAction=/'        "${UPOWER_CONF}"
    sed -i 's/^#NoPollBatteries=/NoPollBatteries=/'                "${UPOWER_CONF}"

    systemctl restart upower 2>/dev/null || true
    ok "Restored ${UPOWER_CONF}"
else
    ok "UPower config not found — skipping"
fi

# -------------------------------------------------------------------------
# Done
# -------------------------------------------------------------------------

echo
echo -e "${GRN}${BLD}Uninstallation complete.${RST}"
echo
echo -e "  The x120x kernel module and all installer changes have been removed."
echo
echo -e "  ${BLD}Reboot to complete the removal:${RST}"
echo
echo -e "    sudo reboot"
echo
echo -e "  ${YLW}Note:${RST} bootloader EEPROM settings (POWER_OFF_ON_HALT, PSU_MAX_CURRENT)"
echo -e "  were not changed. To revert them, run:"
echo
echo -e "    sudo rpi-eeprom-config -e"
echo
