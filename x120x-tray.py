#!/usr/bin/env python3
# x120x-tray.py — system tray applet for the x120x UPS HAT driver
#
# Shows a battery status icon in the system tray.
# - Hover: tooltip with SoC%, status, and charge mode
# - Left-click: open/close detail window
# - Detail window: full battery info + charge mode toggle button
#
# Uses Gtk.StatusIcon (works reliably on wf-panel-pi / Wayfire).
# No AppIndicator, SNI, or ayatana dependencies required.
#
# Requires: python3-gi, gir1.2-gtk-3.0
# (both already present on Raspberry Pi OS Bookworm)
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

BATTERY = '/sys/class/power_supply/x120x-battery'
CHARGER = '/sys/class/power_supply/x120x-charger'
AC      = '/sys/class/power_supply/x120x-ac'
POLL_MS = 3000

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
        result = subprocess.run(['sudo', 'tee', path],
                                input=value.encode(), capture_output=True)
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

    try:    capacity = int(read_sysfs(f'{BATTERY}/capacity'))
    except: capacity = None

    try:    voltage_v = int(read_sysfs(f'{BATTERY}/voltage_now')) / 1_000_000
    except: voltage_v = None

    # power_now is in µW; signed: negative = discharging, positive = charging
    try:
        power_uw = int(read_sysfs(f'{BATTERY}/power_now'))
        power_w  = power_uw / 1_000_000
    except:
        power_w = None

    try:    energy_now  = int(read_sysfs(f'{BATTERY}/energy_now'))  / 1_000_000
    except: energy_now  = None
    try:    energy_full = int(read_sysfs(f'{BATTERY}/energy_full')) / 1_000_000
    except: energy_full = None

    try:    ac_online = int(read_sysfs(f'{AC}/online')) == 1
    except: ac_online = None

    status      = read_sysfs(f'{BATTERY}/status') or 'Unknown'
    charge_type = read_sysfs(f'{CHARGER}/charge_type') or 'Unknown'

    return {
        'capacity':    capacity,
        'status':      status,
        'voltage_v':   voltage_v,
        'power_w':     power_w,
        'energy_now':  energy_now,
        'energy_full': energy_full,
        'charge_type': charge_type,
        'ac_online':   ac_online,
    }

# -------------------------------------------------------------------------
# Icon name selection
# -------------------------------------------------------------------------

def icon_for_state(state):
    if state is None:
        return 'battery-missing-symbolic'
    soc    = state.get('capacity')
    status = (state.get('status') or '').lower()
    if soc is None:
        return 'battery-missing-symbolic'
    if soc >= 90:   level = 'full'
    elif soc >= 60: level = 'good'
    elif soc >= 30: level = 'low'
    else:           level = 'caution'
    if status == 'charging':
        return f'battery-{level}-charging-symbolic'
    return f'battery-{level}-symbolic'

# -------------------------------------------------------------------------
# Text helpers
# -------------------------------------------------------------------------

def tooltip_for_state(state):
    if state is None:
        return 'x120x: driver not found'
    soc = state.get('capacity')
    status = state.get('status') or 'Unknown'
    ct = state.get('charge_type') or '?'
    soc_s = f'{soc}%' if soc is not None else '?'
    return f'Battery: {soc_s}  ({status})  [{ct}]'

def rate_text(state):
    """Return a human-readable rate string showing direction."""
    if state is None:
        return 'Rate:     —'
    power = state.get('power_w')
    status = (state.get('status') or '').lower()
    if power is None:
        return 'Rate:     —'
    if power == 0.0:
        # Zero can mean floating/full or pending — show context
        if status in ('full', 'not charging', 'pending-charge'):
            return 'Rate:     0.00 W  (idle)'
        return 'Rate:     0.00 W'
    if status == 'discharging':
        return f'Rate:     -{abs(power):.2f} W'
    return f'Rate:     +{abs(power):.2f} W'

# -------------------------------------------------------------------------
# Toggle charge mode
# -------------------------------------------------------------------------

def toggle_charge_mode(state, parent_window=None):
    if state is None:
        return
    current  = state.get('charge_type') or 'Fast'
    new_mode = 'Fast' if current == 'Long Life' else 'Long Life'
    ok = write_sysfs(f'{CHARGER}/charge_type', new_mode)
    if not ok:
        dialog = Gtk.MessageDialog(
            transient_for=parent_window,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text=f'Failed to write "{new_mode}" to charge_type.\n'
                 'Is the sudoers rule installed?'
        )
        dialog.run()
        dialog.destroy()

# -------------------------------------------------------------------------
# Detail window
# -------------------------------------------------------------------------

class DetailWindow:
    def __init__(self, on_toggle_cb):
        self._on_toggle_cb = on_toggle_cb
        self._win = Gtk.Window(title='x120x Battery')
        self._win.set_default_size(230, 1)
        self._win.set_resizable(False)
        self._win.set_keep_above(True)
        self._win.connect('delete-event', self._on_close)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        box.set_margin_top(10)
        box.set_margin_bottom(10)
        box.set_margin_start(14)
        box.set_margin_end(14)
        self._win.add(box)

        self._labels = {}
        for key in ('soc', 'status', 'voltage', 'energy', 'rate', 'grid', 'mode'):
            lbl = Gtk.Label(label='—')
            lbl.set_halign(Gtk.Align.START)
            box.pack_start(lbl, False, False, 0)
            self._labels[key] = lbl

        box.pack_start(Gtk.Separator(), False, False, 4)

        self._btn = Gtk.Button(label='Toggle charge mode')
        self._btn.connect('clicked', self._on_btn)
        box.pack_start(self._btn, False, False, 0)

        self._state = None

    def _on_close(self, win, event):
        self._win.hide()
        return True  # don't destroy, just hide

    def _on_btn(self, _):
        self._on_toggle_cb(self._state, self._win)
        GLib.timeout_add(400, lambda: self.update(read_state()) or False)

    def update(self, state):
        self._state = state
        if state is None:
            self._labels['soc'].set_text('Driver not found')
            return

        soc     = state.get('capacity')
        status  = state.get('status') or '?'
        voltage = state.get('voltage_v')
        energy_n = state.get('energy_now')
        energy_f = state.get('energy_full')
        ac      = state.get('ac_online')
        ct      = state.get('charge_type') or '?'

        self._labels['soc'].set_text(
            f'Battery:  {soc}%' if soc is not None else 'Battery:  ?')
        self._labels['status'].set_text(f'Status:   {status}')
        self._labels['voltage'].set_text(
            f'Voltage:  {voltage:.3f} V' if voltage is not None else 'Voltage:  ?')
        if energy_n is not None and energy_f is not None:
            self._labels['energy'].set_text(
                f'Energy:   {energy_n:.1f} / {energy_f:.1f} Wh')
        else:
            self._labels['energy'].set_text('Energy:   ?')
        self._labels['rate'].set_text(rate_text(state))
        self._labels['grid'].set_text(f'Grid:     {"yes" if ac else "no"}')
        self._labels['mode'].set_text(f'Mode:     {ct}')

        new_mode = 'Fast' if ct == 'Long Life' else 'Long Life'
        self._btn.set_label(f'Switch to {new_mode}')

    def show(self):
        self._win.show_all()
        self._win.present()

    def hide(self):
        self._win.hide()

    def is_visible(self):
        return self._win.get_visible()

# -------------------------------------------------------------------------
# Main applet
# -------------------------------------------------------------------------

class X120xTray:
    def __init__(self):
        self._state = None
        self._lock  = threading.Lock()

        self._detail = DetailWindow(on_toggle_cb=self._do_toggle)

        # Gtk.StatusIcon — works reliably with wf-panel-pi tray widget
        self._icon = Gtk.StatusIcon()
        self._icon.set_from_icon_name('battery-missing-symbolic')
        self._icon.set_visible(True)
        self._icon.set_has_tooltip(True)
        self._icon.connect('activate',        self._on_activate)
        self._icon.connect('popup-menu',      self._on_popup)
        self._icon.connect('query-tooltip',   self._on_tooltip)

        self._refresh()
        GLib.timeout_add(POLL_MS, self._refresh)

    def _refresh(self):
        state = read_state()
        with self._lock:
            self._state = state

        self._icon.set_from_icon_name(icon_for_state(state))
        self._icon.set_tooltip_text(tooltip_for_state(state))

        if self._detail.is_visible():
            self._detail.update(state)

        return True

    def _on_activate(self, icon):
        if self._detail.is_visible():
            self._detail.hide()
        else:
            with self._lock:
                state = self._state
            self._detail.update(state)
            self._detail.show()

    def _on_tooltip(self, icon, x, y, keyboard, tooltip):
        with self._lock:
            state = self._state
        tooltip.set_text(tooltip_for_state(state))
        return True

    def _on_popup(self, icon, button, time):
        menu = Gtk.Menu()

        with self._lock:
            state = self._state
        ct = (state or {}).get('charge_type') or 'Fast'
        new_mode = 'Fast' if ct == 'Long Life' else 'Long Life'

        toggle_item = Gtk.MenuItem(label=f'Switch to {new_mode}')
        toggle_item.connect('activate', lambda _: self._do_toggle(state))
        menu.append(toggle_item)

        menu.append(Gtk.SeparatorMenuItem())

        quit_item = Gtk.MenuItem(label='Quit')
        quit_item.connect('activate', lambda _: Gtk.main_quit())
        menu.append(quit_item)

        menu.show_all()
        menu.popup(None, None, None, None, button, time)

    def _do_toggle(self, state, parent=None):
        toggle_charge_mode(state, parent)
        GLib.timeout_add(400, self._refresh)

# -------------------------------------------------------------------------
# Entry point
# -------------------------------------------------------------------------

if __name__ == '__main__':
    if not driver_present():
        print('x120x driver not found — is the module loaded?', file=sys.stderr)
    X120xTray()
    Gtk.main()
