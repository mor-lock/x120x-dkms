#!/usr/bin/env python3
# x120x-tray.py — charge mode toggle for the x120x UPS HAT driver
#
# Creates a small always-on-top window showing battery state and a
# toggle button for Fast / Long Life charge mode.  Works reliably on
# Raspberry Pi OS Bookworm (Wayland/Wayfire) without AppIndicator,
# SNI, or ayatana dependencies.
#
# Requires: python3-gi, gir1.2-gtk-3.0  (both already on Raspberry Pi OS)
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

def read_state():
    if not driver_present():
        return None
    try:    capacity = int(read_sysfs(f'{BATTERY}/capacity'))
    except: capacity = None
    try:    voltage_v = int(read_sysfs(f'{BATTERY}/voltage_now')) / 1_000_000
    except: voltage_v = None
    try:    power_w = abs(int(read_sysfs(f'{BATTERY}/power_now'))) / 1_000_000
    except: power_w = None
    try:    ac_online = int(read_sysfs(f'{AC}/online')) == 1
    except: ac_online = None
    return {
        'capacity':    capacity,
        'status':      read_sysfs(f'{BATTERY}/status') or '?',
        'voltage_v':   voltage_v,
        'power_w':     power_w,
        'charge_type': read_sysfs(f'{CHARGER}/charge_type') or '?',
        'ac_online':   ac_online,
    }

class X120xWindow:
    def __init__(self):
        self._state = None
        self._lock  = threading.Lock()

        self._win = Gtk.Window(title='x120x Battery')
        self._win.set_default_size(220, 1)
        self._win.set_resizable(False)
        self._win.set_keep_above(True)
        self._win.connect('delete-event', Gtk.main_quit)

        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        box.set_margin_top(10)
        box.set_margin_bottom(10)
        box.set_margin_start(14)
        box.set_margin_end(14)
        self._win.add(box)

        # Status label
        self._lbl_soc     = Gtk.Label(label='—')
        self._lbl_status  = Gtk.Label(label='—')
        self._lbl_voltage = Gtk.Label(label='—')
        self._lbl_rate    = Gtk.Label(label='—')
        self._lbl_grid    = Gtk.Label(label='—')
        self._lbl_mode    = Gtk.Label(label='—')

        for lbl in (self._lbl_soc, self._lbl_status, self._lbl_voltage,
                    self._lbl_rate, self._lbl_grid, self._lbl_mode):
            lbl.set_halign(Gtk.Align.START)
            box.pack_start(lbl, False, False, 0)

        box.pack_start(Gtk.Separator(), False, False, 4)

        self._btn = Gtk.Button(label='Toggle charge mode')
        self._btn.connect('clicked', self._on_toggle)
        box.pack_start(self._btn, False, False, 0)

        self._win.show_all()

        self._refresh()
        GLib.timeout_add(POLL_MS, self._refresh)

    def _refresh(self):
        state = read_state()
        with self._lock:
            self._state = state

        if state is None:
            self._lbl_soc.set_text('Driver not found')
            return True

        soc     = state.get('capacity')
        status  = state.get('status') or '?'
        voltage = state.get('voltage_v')
        power   = state.get('power_w')
        ac      = state.get('ac_online')
        ct      = state.get('charge_type') or '?'

        self._lbl_soc.set_text(f'Battery:  {soc}%' if soc is not None else 'Battery:  ?')
        self._lbl_status.set_text(f'Status:   {status}')
        self._lbl_voltage.set_text(f'Voltage:  {voltage:.3f} V' if voltage else 'Voltage:  ?')
        self._lbl_rate.set_text(f'Rate:     {power:.2f} W' if power is not None else 'Rate:     ?')
        self._lbl_grid.set_text(f'Grid:     {"yes" if ac else "no"}')
        self._lbl_mode.set_text(f'Mode:     {ct}')

        new_mode = 'Fast' if ct == 'Long Life' else 'Long Life'
        self._btn.set_label(f'Switch to {new_mode}')

        return True

    def _on_toggle(self, _btn):
        with self._lock:
            state = self._state
        if state is None:
            return
        current  = state.get('charge_type') or 'Fast'
        new_mode = 'Fast' if current == 'Long Life' else 'Long Life'
        ok = write_sysfs(f'{CHARGER}/charge_type', new_mode)
        if not ok:
            dialog = Gtk.MessageDialog(
                transient_for=self._win,
                message_type=Gtk.MessageType.ERROR,
                buttons=Gtk.ButtonsType.OK,
                text=f'Failed to write "{new_mode}" to charge_type.\n'
                     'Is the sudoers rule installed?'
            )
            dialog.run()
            dialog.destroy()
        GLib.timeout_add(300, self._refresh)

if __name__ == '__main__':
    if not driver_present():
        print('x120x driver not found', file=sys.stderr)
    X120xWindow()
    Gtk.main()
