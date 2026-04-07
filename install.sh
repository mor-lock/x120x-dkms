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
#   --charge-mode MODE     Initial charge mode: fast or longlife (default: fast)
#                          longlife limits charging to 75-80% to extend battery life
#                          Can be changed at any time via sysfs; persisted across reboots
#   --board VARIANT        Board variant (default: x120x).
#                          Supported: x120x, x728v2, x728v1, x708, x729
#                          Variants other than x120x are EXPERIMENTAL (untested).
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
OPT_CHARGE_MODE=""
OPT_BOARD=""

while [ $# -gt 0 ]; do
    case "$1" in
        --battery-mah)
            OPT_MAH="$2"
            shift 2
            ;;
        --charge-mode)
            case "$2" in
                fast|Fast|FAST)         OPT_CHARGE_MODE="fast" ;;
                longlife|LongLife|LONGLIFE|long-life|"Long Life") OPT_CHARGE_MODE="longlife" ;;
                *) die "Unknown charge mode: $2  (use fast or longlife)" ;;
            esac
            shift 2
            ;;
        --board)
            case "$2" in
                x120x|x728v2|x728v1|x708|x729) OPT_BOARD="$2" ;;
                *) die "Unknown board variant: $2  (use x120x, x728v2, x728v1, x708, x729)" ;;
            esac
            shift 2
            ;;
        --help|-h)
            echo "Usage: sudo bash install.sh [--battery-mah N] [--charge-mode fast|longlife] [--board x120x|x728v2|x728v1|x708|x729]"
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
PKG_VERSION="0.4.1"
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

info "Step 1/10 — Installing dependencies..."
apt-get install -y dkms raspberrypi-kernel-headers \
    || die "apt-get install failed"
ok "Dependencies installed"

# -------------------------------------------------------------------------
# Step 2: Copy source to DKMS tree
# -------------------------------------------------------------------------

DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VERSION}"

info "Step 2/10 — Copying source to DKMS tree (${DKMS_SRC})..."
rm -rf "${DKMS_SRC}"
cp -r "${SRC_DIR}" "${DKMS_SRC}"
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

info "Step 4/10 — Compiling device tree overlay..."

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

CHARGE_MODE_DEFAULT="${OPT_CHARGE_MODE:-fast}"
CONSERVATION_DEFAULT=0
[ "${CHARGE_MODE_DEFAULT}" = "longlife" ] && CONSERVATION_DEFAULT=1
BOARD_VARIANT="${OPT_BOARD:-x120x}"

# Warn if experimental board selected
if [ "${BOARD_VARIANT}" != "x120x" ]; then
    warn "Board variant ${BOARD_VARIANT} is EXPERIMENTAL and untested."
    warn "Validate correct operation before relying on this driver."
fi

# Long Life not supported on boards without charge control
if [ "${BOARD_VARIANT}" = "x728v1" ] || [ "${BOARD_VARIANT}" = "x708" ] || [ "${BOARD_VARIANT}" = "x729" ]; then
    if [ "${CHARGE_MODE_DEFAULT}" = "longlife" ]; then
        warn "--charge-mode longlife ignored: ${BOARD_VARIANT} has no charge control GPIO"
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
#
# After editing, reload the driver:
#   sudo rmmod x120x && sudo modprobe x120x
# Or simply reboot.

options x120x battery_mah=${INPUT_MAH} conservation_mode_default=${CONSERVATION_DEFAULT} board=${BOARD_VARIANT}
MODPROBE_EOF
ok "Battery configuration written"

# -------------------------------------------------------------------------
# Step 6: Install overlay  (renumbered)
# -------------------------------------------------------------------------

info "Step 6/10 — Installing device tree overlay to ${OVERLAYS_DIR}..."
cp x120x.dtbo "${OVERLAYS_DIR}/" \
    || die "Failed to copy overlay to ${OVERLAYS_DIR}"
ok "Overlay installed"

# -------------------------------------------------------------------------
# Step 7: Enable overlay in config.txt
# -------------------------------------------------------------------------

info "Step 7/10 — Enabling overlay in ${CONFIG_TXT}..."

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

# Add pull-up on GPIO6 (AC-present signal).
# The X1206 drives GPIO6 high when AC is present and actively pulls it
# low on AC loss.  Without a pull-up the pin can float low at boot
# before the hardware asserts the signal, causing the driver to falsely
# report ac_online=0 and trigger an unnecessary shutdown even with the
# charger connected.
if grep -q "^gpio=6=pu" "${CONFIG_TXT}"; then
    ok "gpio=6=pu already present in ${CONFIG_TXT}"
else
    printf 'gpio=6=pu\n' >> "${CONFIG_TXT}"
    ok "Added gpio=6=pu to ${CONFIG_TXT}"
fi

# -------------------------------------------------------------------------
# Step 8: Bootloader check (Pi 5 only)
# -------------------------------------------------------------------------

info "Step 8/10 — Checking bootloader configuration..."

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

info "Step 9/10 — Configuring low-battery shutdown..."

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

# UPower configuration:
#   CriticalPowerAction=PowerOff  — HybridSleep hangs on Raspberry Pi.
#   NoPollBatteries=true          — The driver sends uevents on all state
#                                   changes and on a 30s heartbeat.  UPower
#                                   polling the kernel independently causes
#                                   races that produce spurious 0%/unknown
#                                   entries in the history files and corrupt
#                                   gnome-power-statistics graphs.
UPOWER_CONF="/etc/UPower/UPower.conf"
if [ -f "${UPOWER_CONF}" ]; then
    if grep -q "^CriticalPowerAction=PowerOff" "${UPOWER_CONF}" 2>/dev/null; then
        ok "CriticalPowerAction=PowerOff already set in ${UPOWER_CONF}"
    else
        sed -i 's/^CriticalPowerAction=/#CriticalPowerAction=/' "${UPOWER_CONF}" 2>/dev/null || true
        echo "" >> "${UPOWER_CONF}"
        echo "# Added by x120x-dkms installer — HybridSleep hangs on Raspberry Pi." >> "${UPOWER_CONF}"
        echo "CriticalPowerAction=PowerOff" >> "${UPOWER_CONF}"
        ok "CriticalPowerAction=PowerOff set in ${UPOWER_CONF}"
    fi
    if grep -q "^NoPollBatteries=true" "${UPOWER_CONF}" 2>/dev/null; then
        ok "NoPollBatteries=true already set in ${UPOWER_CONF}"
    else
        sed -i 's/^NoPollBatteries=/#NoPollBatteries=/' "${UPOWER_CONF}" 2>/dev/null || true
        echo "" >> "${UPOWER_CONF}"
        echo "# Added by x120x-dkms installer — driver sends uevents; polling causes races." >> "${UPOWER_CONF}"
        echo "NoPollBatteries=true" >> "${UPOWER_CONF}"
        ok "NoPollBatteries=true set in ${UPOWER_CONF}"
    fi
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
CONF=/etc/modprobe.d/x120x.conf
CHARGE_TYPE=$(cat /sys/class/power_supply/x120x-charger/charge_type 2>/dev/null)
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
# Persistence: udev rule to save charge mode to modprobe.d on change
# -------------------------------------------------------------------------

PERSIST_SCRIPT="/usr/local/lib/x120x-persist-mode.sh"
UDEV_RULE="/etc/udev/rules.d/90-x120x-persist.rules"

cat > "${PERSIST_SCRIPT}" << 'PERSIST_EOF'
#!/bin/sh
# x120x-persist-mode.sh — called by udev when charge_type changes.
# Writes conservation_mode_default to /etc/modprobe.d/x120x.conf so
# the charge mode survives reboots.
CONF=/etc/modprobe.d/x120x.conf
CHARGE_TYPE=$(cat /sys/class/power_supply/x120x-charger/charge_type 2>/dev/null)
case "$CHARGE_TYPE" in
    "Long Life") MODE=1 ;;
    *)           MODE=0 ;;
esac
if [ -f "$CONF" ]; then
    # Update existing conservation_mode_default if present
    if grep -q "conservation_mode_default" "$CONF"; then
        sed -i "s/conservation_mode_default=[0-9]*/conservation_mode_default=${MODE}/" "$CONF"
    else
        # Append to the options line
        sed -i "s/^options x120x /options x120x conservation_mode_default=${MODE} /" "$CONF"
    fi
fi
PERSIST_EOF
chmod 755 "${PERSIST_SCRIPT}"
ok "Installed persistence script to ${PERSIST_SCRIPT}"

cat > "${UDEV_RULE}" << 'UDEV_EOF'
# Persist x120x charge mode to /etc/modprobe.d/x120x.conf on change
ACTION=="change", SUBSYSTEM=="power_supply", KERNEL=="x120x-charger",     RUN+="/usr/local/lib/x120x-persist-mode.sh"
UDEV_EOF
udevadm control --reload-rules 2>/dev/null || true
ok "Installed udev persistence rule to ${UDEV_RULE}"

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
