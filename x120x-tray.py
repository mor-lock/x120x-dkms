#!/usr/bin/env python3
# x120x-tray.py — system tray applet for the x120x UPS HAT driver
#
# Shows battery SoC, charge mode (Fast / Long Life), and AC state
# as a tray icon.  Left-click toggles between Fast and Long Life
# charge modes.  Right-click shows a menu with details and quit.
#
# Requires: python3-gi, gir1.2-gtk-3.0, gir1.2-appindicator3-0.1
#           (or gir1.2-ayatanaappindicator3-0.1 on newer systems)
#
# Install dependencies:
#   sudo apt install python3-gi gir1.2-gtk-3.0 \
#        gir1.2-ayatanaappindicatorglib-2.0 libayatana-appindicator3-dev
#
# Copyright (C) 2026 Edvard Fielding
# SPDX-License-Identifier: GPL-2.0-or-later

import gi
import os
import subprocess
import sys
import threading

gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

# Try ayatana (newer) first, fall back to legacy appindicator
try:
    gi.require_version('AyatanaAppIndicator3', '0.1')
    from gi.repository import AyatanaAppIndicator3 as AppIndicator3
except (ValueError, ImportError):
    try:
        gi.require_version('AppIndicator3', '0.1')
        from gi.repository import AppIndicator3
    except (ValueError, ImportError):
        AppIndicator3 = None

# -------------------------------------------------------------------------
# sysfs paths
# -------------------------------------------------------------------------

BATTERY   = '/sys/class/power_supply/x120x-battery'
CHARGER   = '/sys/class/power_supply/x120x-charger'
AC        = '/sys/class/power_supply/x120x-ac'

POLL_MS   = 2000   # refresh every 2 seconds

# -------------------------------------------------------------------------
# sysfs helpers
# -------------------------------------------------------------------------

def read_sysfs(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None

def write_sysfs(path, value):
    try:
        # Use sudo tee so we don't need to run the whole applet as root
        result = subprocess.run(
            ['sudo', 'tee', path],
            input=value.encode(),
            capture_output=True
        )
        return result.returncode == 0
    except Exception:
        return False

def driver_present():
    return os.path.isdir(BATTERY)

# -------------------------------------------------------------------------
# State reading
# -------------------------------------------------------------------------

def read_state():
    if not driver_present():
        return None

    capacity    = read_sysfs(f'{BATTERY}/capacity')
    status      = read_sysfs(f'{BATTERY}/status')
    voltage_uv  = read_sysfs(f'{BATTERY}/voltage_now')
    energy_now  = read_sysfs(f'{BATTERY}/energy_now')
    energy_full = read_sysfs(f'{BATTERY}/energy_full')
    power_now   = read_sysfs(f'{BATTERY}/power_now')
    charge_type = read_sysfs(f'{CHARGER}/charge_type')
    ac_online   = read_sysfs(f'{AC}/online')

    try:    capacity    = int(capacity)
    except: capacity    = None
    try:    voltage_v   = int(voltage_uv) / 1_000_000
    except: voltage_v   = None
    try:    energy_now  = int(energy_now) / 1_000_000
    except: energy_now  = None
    try:    energy_full = int(energy_full) / 1_000_000
    except: energy_full = None
    try:    power_w     = abs(int(power_now)) / 1_000_000
    except: power_w     = None
    try:    ac_online   = int(ac_online) == 1
    except: ac_online   = None

    return {
        'capacity':    capacity,
        'status':      status,
        'voltage_v':   voltage_v,
        'energy_now':  energy_now,
        'energy_full': energy_full,
        'power_w':     power_w,
        'charge_type': charge_type,
        'ac_online':   ac_online,
    }

# -------------------------------------------------------------------------
# Icon selection
# -------------------------------------------------------------------------

# Map (conservation_mode, ac_online, capacity_bucket) → icon name
# We use standard freedesktop icon names so no custom icons needed.

def icon_for_state(state):
    if state is None:
        return 'battery-missing-symbolic'

    ac       = state.get('ac_online')
    soc      = state.get('capacity')
    status   = (state.get('status') or '').lower()
    longlife = (state.get('charge_type') or '') == 'Long Life'

    charging = status in ('charging',)
    pending  = status in ('pending-charge', 'not charging')

    if soc is None:
        return 'battery-missing-symbolic'

    # Bucket SoC
    if soc >= 90:   level = 'full'
    elif soc >= 60: level = 'good'
    elif soc >= 30: level = 'low'
    else:           level = 'caution'

    if charging:
        return f'battery-{level}-charging-symbolic'
    elif pending and longlife:
        # Conservation mode holding — use a distinct icon
        return f'battery-{level}-symbolic'
    elif not ac:
        return f'battery-{level}-symbolic'
    else:
        return f'battery-{level}-symbolic'

# -------------------------------------------------------------------------
# Tooltip / label text
# -------------------------------------------------------------------------

def label_for_state(state):
    if state is None:
        return 'x120x: no driver'

    soc        = state.get('capacity')
    charge_type = state.get('charge_type') or '?'
    ac         = state.get('ac_online')

    soc_s  = f'{soc}%' if soc is not None else '?%'
    ac_s   = 'AC' if ac else 'BAT'
    mode_s = 'LL' if charge_type == 'Long Life' else 'Fast'
    return f'{soc_s} {ac_s} [{mode_s}]'

def tooltip_for_state(state):
    if state is None:
        return 'x120x driver not found'

    lines = []
    soc       = state.get('capacity')
    status    = state.get('status') or '?'
    voltage   = state.get('voltage_v')
    energy_n  = state.get('energy_now')
    energy_f  = state.get('energy_full')
    power     = state.get('power_w')
    ct        = state.get('charge_type') or '?'
    ac        = state.get('ac_online')

    lines.append(f'Battery: {soc}%  ({status})')
    if voltage   is not None: lines.append(f'Voltage:  {voltage:.3f} V')
    if energy_n  is not None and energy_f is not None:
        lines.append(f'Energy:   {energy_n:.1f} / {energy_f:.1f} Wh')
    if power     is not None: lines.append(f'Rate:     {power:.2f} W')
    lines.append(f'Grid:     {"yes" if ac else "no"}')
    lines.append(f'Mode:     {ct}')
    return '\n'.join(lines)

# -------------------------------------------------------------------------
# Toggle charge mode
# -------------------------------------------------------------------------

def toggle_charge_mode(state):
    if state is None:
        return
    current = state.get('charge_type') or 'Fast'
    new_mode = 'Fast' if current == 'Long Life' else 'Long Life'
    path = f'{CHARGER}/charge_type'
    ok = write_sysfs(path, new_mode)
    if not ok:
        dialog = Gtk.MessageDialog(
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text=f'Failed to set charge mode to "{new_mode}".\n'
                 'Make sure the sudoers rule is installed:\n'
                 '  x120x ALL=(ALL) NOPASSWD: /usr/bin/tee /sys/class/power_supply/*'
        )
        dialog.run()
        dialog.destroy()

# -------------------------------------------------------------------------
# Main applet
# -------------------------------------------------------------------------

class X120xTray:

    def __init__(self):
        self._state = None
        self._lock  = threading.Lock()

        # Build indicator or fall back to StatusIcon
        self._use_indicator = AppIndicator3 is not None

        if self._use_indicator:
            self._indicator = AppIndicator3.Indicator.new(
                'x120x-tray',
                'battery-missing-symbolic',
                AppIndicator3.IndicatorCategory.HARDWARE
            )
            self._indicator.set_status(AppIndicator3.IndicatorStatus.ACTIVE)
            self._indicator.set_label('…', '100% AC [LL]')
            menu = self._build_menu()
            self._indicator.set_menu(menu)
        else:
            # Fallback: classic StatusIcon (deprecated but works on Wayfire/LXDE)
            self._status_icon = Gtk.StatusIcon()
            self._status_icon.set_from_icon_name('battery-missing-symbolic')
            self._status_icon.set_visible(True)
            self._status_icon.connect('activate',      self._on_activate)
            self._status_icon.connect('popup-menu',    self._on_popup)

        # Initial read and schedule polling
        self._refresh()
        GLib.timeout_add(POLL_MS, self._refresh)

    def _build_menu(self):
        menu = Gtk.Menu()

        self._details_item = Gtk.MenuItem(label='Loading…')
        self._details_item.set_sensitive(False)
        menu.append(self._details_item)

        sep = Gtk.SeparatorMenuItem()
        menu.append(sep)

        self._toggle_item = Gtk.MenuItem(label='Toggle charge mode')
        self._toggle_item.connect('activate', lambda _: self._do_toggle())
        menu.append(self._toggle_item)

        sep2 = Gtk.SeparatorMenuItem()
        menu.append(sep2)

        quit_item = Gtk.MenuItem(label='Quit')
        quit_item.connect('activate', lambda _: Gtk.main_quit())
        menu.append(quit_item)

        menu.show_all()
        return menu

    def _refresh(self):
        state = read_state()
        with self._lock:
            self._state = state

        icon    = icon_for_state(state)
        label   = label_for_state(state)
        tooltip = tooltip_for_state(state)
        ct      = (state or {}).get('charge_type') or 'Fast'
        toggle_label = f'Switch to {"Fast" if ct == "Long Life" else "Long Life"}'

        if self._use_indicator:
            self._indicator.set_icon_full(icon, tooltip)
            self._indicator.set_label(label, '100% AC [LL]')
            self._details_item.set_label(tooltip.replace('\n', '  |  '))
            self._toggle_item.set_label(toggle_label)
        else:
            self._status_icon.set_from_icon_name(icon)
            self._status_icon.set_tooltip_text(tooltip)

        return True  # keep polling

    def _do_toggle(self):
        with self._lock:
            state = self._state
        toggle_charge_mode(state)
        # Refresh immediately after toggle
        GLib.timeout_add(300, self._refresh)

    # Fallback StatusIcon handlers
    def _on_activate(self, icon):
        self._do_toggle()

    def _on_popup(self, icon, button, time):
        menu = self._build_menu()
        menu.popup(None, None, None, None, button, time)


# -------------------------------------------------------------------------
# Entry point
# -------------------------------------------------------------------------

if __name__ == '__main__':
    if not driver_present():
        print('x120x driver not found — is the module loaded?', file=sys.stderr)
        # Still start — will show missing icon and retry on poll

    applet = X120xTray()
    Gtk.main()
