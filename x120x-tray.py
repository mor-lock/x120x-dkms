#!/usr/bin/env python3
# x120x-tray.py — system tray applet for the x120x UPS HAT driver
#
# Shows battery SoC, charge mode (Fast / Long Life), and AC state
# as a custom-drawn tray icon.  Left-click toggles between Fast and
# Long Life charge modes.  Right-click shows a menu with details.
#
# Icon design:
#   - Battery body filled proportionally to SoC%
#   - Fill colour: green (high) -> amber (mid) -> red (low)
#   - Charging (Fast, AC present): bright yellow lightning bolt overlay
#   - Long Life mode on AC: green leaf overlay
#   - Discharging on battery: no overlay, just the fill bar
#
# Requires: python3-gi, gir1.2-gtk-3.0, gir1.2-ayatanaappindicator3-0.1
#
# Install dependencies:
#   sudo apt install python3-gi gir1.2-gtk-3.0 gir1.2-ayatanaappindicator3-0.1
#
# Copyright (C) 2026 Edvard Fielding
# SPDX-License-Identifier: GPL-2.0-or-later

import gi
import math
import os
import subprocess
import sys
import tempfile
import threading

gi.require_version('Gtk', '3.0')
gi.require_version('GdkPixbuf', '2.0')
from gi.repository import Gtk, GLib, GdkPixbuf

try:
    gi.require_version('AyatanaAppIndicator3', '0.1')
    from gi.repository import AyatanaAppIndicator3 as AppIndicator3
except (ValueError, ImportError):
    try:
        gi.require_version('AppIndicator3', '0.1')
        from gi.repository import AppIndicator3
    except (ValueError, ImportError):
        AppIndicator3 = None

try:
    import cairo
    HAS_CAIRO = True
except ImportError:
    HAS_CAIRO = False

# -------------------------------------------------------------------------
# sysfs paths
# -------------------------------------------------------------------------

BATTERY = '/sys/class/power_supply/x120x-battery'
CHARGER = '/sys/class/power_supply/x120x-charger'
AC      = '/sys/class/power_supply/x120x-ac'

POLL_MS   = 2000
ICON_SIZE = 22

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
# Custom Cairo icon drawing
# -------------------------------------------------------------------------

def _soc_colour(soc):
    if soc >= 60:
        return (0.25, 0.85, 0.35)       # green
    elif soc >= 30:
        t = (soc - 30) / 30.0
        return (1.0, 0.55 + 0.10 * t, 0.05)  # amber
    else:
        return (0.95, 0.15, 0.05)       # red

def _rounded_rect(ctx, x, y, w, h, r):
    ctx.new_sub_path()
    ctx.arc(x + r,     y + r,     r, math.pi,         3 * math.pi / 2)
    ctx.arc(x + w - r, y + r,     r, 3 * math.pi / 2, 0)
    ctx.arc(x + w - r, y + h - r, r, 0,               math.pi / 2)
    ctx.arc(x + r,     y + h - r, r, math.pi / 2,     math.pi)
    ctx.close_path()

def _draw_lightning(ctx, x, y, w, h, alpha=1.0):
    ctx.save()
    ctx.set_source_rgba(1.0, 0.92, 0.1, alpha)
    ctx.move_to(x + w * 0.55, y + h * 0.08)
    ctx.line_to(x + w * 0.25, y + h * 0.52)
    ctx.line_to(x + w * 0.50, y + h * 0.52)
    ctx.line_to(x + w * 0.45, y + h * 0.92)
    ctx.line_to(x + w * 0.75, y + h * 0.48)
    ctx.line_to(x + w * 0.50, y + h * 0.48)
    ctx.line_to(x + w * 0.55, y + h * 0.08)
    ctx.close_path()
    ctx.fill()
    ctx.restore()

def _draw_leaf(ctx, x, y, w, h, alpha=1.0):
    ctx.save()
    cx, cy = x + w / 2, y + h / 2
    ctx.set_source_rgba(0.2, 0.85, 0.3, alpha)
    ctx.move_to(cx, y + h * 0.1)
    ctx.curve_to(x + w * 0.9, y + h * 0.2,
                 x + w * 0.9, y + h * 0.8,
                 cx, y + h * 0.9)
    ctx.curve_to(x + w * 0.1, y + h * 0.8,
                 x + w * 0.1, y + h * 0.2,
                 cx, y + h * 0.1)
    ctx.close_path()
    ctx.fill()
    ctx.set_source_rgba(0.1, 0.5, 0.15, alpha)
    ctx.set_line_width(h * 0.08)
    ctx.move_to(cx, y + h * 0.15)
    ctx.line_to(cx, y + h * 0.85)
    ctx.stroke()
    ctx.restore()

def draw_battery_icon(soc, status, charge_type, ac_online, size=ICON_SIZE):
    if not HAS_CAIRO:
        return None

    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
    ctx  = cairo.Context(surf)
    ctx.set_antialias(cairo.ANTIALIAS_SUBPIXEL)

    S   = size
    bx  = S * 0.05
    by  = S * 0.20
    bw  = S * 0.78
    bh  = S * 0.60
    br  = S * 0.06
    nub_w = S * 0.07
    nub_h = bh * 0.38
    nub_x = bx + bw
    nub_y = by + (bh - nub_h) / 2

    # Outer border
    ctx.set_source_rgba(0.85, 0.85, 0.85, 1.0)
    ctx.set_line_width(S * 0.07)
    _rounded_rect(ctx, bx, by, bw, bh, br)
    ctx.stroke()

    # Terminal nub
    ctx.set_source_rgba(0.75, 0.75, 0.75, 1.0)
    ctx.rectangle(nub_x, nub_y, nub_w, nub_h)
    ctx.fill()

    # Inner fill
    pad  = S * 0.07
    ibx  = bx + pad
    iby  = by + pad
    ibw  = bw - 2 * pad
    ibh  = bh - 2 * pad
    ibr  = max(br - pad * 0.5, 1)

    if soc is not None:
        fill_w = ibw * (soc / 100.0)
        r, g, b = _soc_colour(soc)
        ctx.save()
        ctx.rectangle(ibx, iby, fill_w, ibh)
        ctx.clip()
        _rounded_rect(ctx, ibx, iby, ibw, ibh, ibr)
        ctx.set_source_rgba(r, g, b, 1.0)
        ctx.fill()
        ctx.restore()
        # Shine
        if fill_w > 2:
            ctx.save()
            ctx.rectangle(ibx, iby, fill_w, ibh * 0.45)
            ctx.clip()
            _rounded_rect(ctx, ibx, iby, ibw, ibh, ibr)
            ctx.set_source_rgba(1.0, 1.0, 1.0, 0.18)
            ctx.fill()
            ctx.restore()
    else:
        ctx.save()
        _rounded_rect(ctx, ibx, iby, ibw, ibh, ibr)
        ctx.set_source_rgba(0.45, 0.45, 0.45, 0.6)
        ctx.fill()
        ctx.restore()

    # Overlay symbol
    ox = bx + bw * 0.15
    oy = by + bh * 0.10
    ow = bw * 0.70
    oh = bh * 0.80

    st       = (status or '').lower()
    charging = st == 'charging'
    longlife = (charge_type or '') == 'Long Life'

    if charging and not longlife:
        _draw_lightning(ctx, ox, oy, ow, oh, alpha=1.0)
    elif longlife and ac_online:
        _draw_leaf(ctx, ox, oy, ow, oh, alpha=0.92)
    elif charging and longlife:
        _draw_leaf(ctx, ox, oy, ow, oh, alpha=0.92)

    # Convert cairo surface to GdkPixbuf
    # cairo uses ARGB premultiplied; GdkPixbuf wants RGBA straight
    data = surf.get_data()
    pixbuf = GdkPixbuf.Pixbuf.new_from_bytes(
        GLib.Bytes(bytes(data)),
        GdkPixbuf.Colorspace.RGB,
        True, 8,
        size, size,
        surf.get_stride(),
    )
    # cairo ARGB32 is BGRA on little-endian; swap R and B channels
    pixbuf = pixbuf.copy()
    arr = pixbuf.get_pixels_with_length()[0]
    ba  = bytearray(arr)
    for i in range(0, len(ba), 4):
        ba[i], ba[i+2] = ba[i+2], ba[i]   # swap R and B
    pixbuf2 = GdkPixbuf.Pixbuf.new_from_bytes(
        GLib.Bytes(bytes(ba)),
        GdkPixbuf.Colorspace.RGB,
        True, 8,
        size, size,
        surf.get_stride(),
    )
    return pixbuf2

# -------------------------------------------------------------------------
# Fallback icon name (when Cairo unavailable)
# -------------------------------------------------------------------------

def icon_name_for_state(state):
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

def label_for_state(state):
    if state is None:
        return 'x120x: no driver'
    soc         = state.get('capacity')
    charge_type = state.get('charge_type') or '?'
    ac          = state.get('ac_online')
    soc_s  = f'{soc}%' if soc is not None else '?%'
    ac_s   = 'AC' if ac else 'BAT'
    mode_s = 'LL' if charge_type == 'Long Life' else 'Fast'
    return f'{soc_s} {ac_s} [{mode_s}]'

def tooltip_for_state(state):
    if state is None:
        return 'x120x driver not found'
    soc      = state.get('capacity')
    status   = state.get('status') or 'Unknown'
    voltage  = state.get('voltage_v')
    energy_n = state.get('energy_now')
    energy_f = state.get('energy_full')
    power    = state.get('power_w')
    ct       = state.get('charge_type') or '?'
    ac       = state.get('ac_online')

    soc_s = f'{soc}%' if soc is not None else '?'
    lines = [f'Battery: {soc_s}  ({status})']
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
    current  = state.get('charge_type') or 'Fast'
    new_mode = 'Fast' if current == 'Long Life' else 'Long Life'
    ok = write_sysfs(f'{CHARGER}/charge_type', new_mode)
    if not ok:
        dialog = Gtk.MessageDialog(
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text=f'Failed to set charge mode to "{new_mode}".\n'
                 'Make sure the sudoers rule is installed:\n'
                 '  ALL ALL=(ALL) NOPASSWD: /usr/bin/tee /sys/class/power_supply/*'
        )
        dialog.run()
        dialog.destroy()

# -------------------------------------------------------------------------
# AppIndicator icon: must be a file path, not a pixbuf
# -------------------------------------------------------------------------

_icon_tmp = None

def _pixbuf_to_tmp_png(pixbuf):
    global _icon_tmp
    if _icon_tmp is None:
        fd, _icon_tmp = tempfile.mkstemp(suffix='.png', prefix='x120x-icon-')
        os.close(fd)
    pixbuf.savev(_icon_tmp, 'png', [], [])
    return _icon_tmp

# -------------------------------------------------------------------------
# Main applet
# -------------------------------------------------------------------------

class X120xTray:

    def __init__(self):
        self._state = None
        self._lock  = threading.Lock()
        self._use_indicator = AppIndicator3 is not None

        if self._use_indicator:
            self._indicator = AppIndicator3.Indicator.new(
                'x120x-tray',
                'battery-missing-symbolic',
                AppIndicator3.IndicatorCategory.HARDWARE
            )
            self._indicator.set_status(AppIndicator3.IndicatorStatus.ACTIVE)
            self._indicator.set_label('...', '100% AC [LL]')
            self._indicator.set_menu(self._build_menu())
        else:
            self._status_icon = Gtk.StatusIcon()
            self._status_icon.set_from_icon_name('battery-missing-symbolic')
            self._status_icon.set_visible(True)
            self._status_icon.connect('activate',   self._on_activate)
            self._status_icon.connect('popup-menu', self._on_popup)

        self._refresh()
        GLib.timeout_add(POLL_MS, self._refresh)

    def _build_menu(self):
        menu = Gtk.Menu()

        self._details_item = Gtk.MenuItem(label='Loading...')
        self._details_item.set_sensitive(False)
        menu.append(self._details_item)

        menu.append(Gtk.SeparatorMenuItem())

        self._toggle_item = Gtk.MenuItem(label='Toggle charge mode')
        self._toggle_item.connect('activate', lambda _: self._do_toggle())
        menu.append(self._toggle_item)

        menu.append(Gtk.SeparatorMenuItem())

        quit_item = Gtk.MenuItem(label='Quit')
        quit_item.connect('activate', lambda _: Gtk.main_quit())
        menu.append(quit_item)

        menu.show_all()
        return menu

    def _refresh(self):
        state = read_state()
        with self._lock:
            self._state = state

        tooltip      = tooltip_for_state(state)
        label        = label_for_state(state)
        ct           = (state or {}).get('charge_type') or 'Fast'
        toggle_label = f'Switch to {"Fast" if ct == "Long Life" else "Long Life"}'

        # Generate icon
        pixbuf = None
        if HAS_CAIRO and state is not None:
            pixbuf = draw_battery_icon(
                soc         = state.get('capacity'),
                status      = state.get('status'),
                charge_type = state.get('charge_type'),
                ac_online   = state.get('ac_online'),
                size        = ICON_SIZE,
            )

        if self._use_indicator:
            if pixbuf:
                icon_path = _pixbuf_to_tmp_png(pixbuf)
                self._indicator.set_icon_full(icon_path, tooltip)
            else:
                self._indicator.set_icon_full(icon_name_for_state(state), tooltip)
            self._indicator.set_label(label, '100% AC [LL]')
            self._details_item.set_label(tooltip.replace('\n', '  |  '))
            self._toggle_item.set_label(toggle_label)
        else:
            if pixbuf:
                self._status_icon.set_from_pixbuf(pixbuf)
            else:
                self._status_icon.set_from_icon_name(icon_name_for_state(state))
            self._status_icon.set_tooltip_text(tooltip)

        return True

    def _do_toggle(self):
        with self._lock:
            state = self._state
        toggle_charge_mode(state)
        GLib.timeout_add(300, self._refresh)

    def _on_activate(self, icon):
        self._do_toggle()

    def _on_popup(self, icon, button, time):
        menu = self._build_menu()
        menu.popup(None, None, None, None, button, time)


# -------------------------------------------------------------------------
# Entry point
# -------------------------------------------------------------------------

if __name__ == '__main__':
    if not HAS_CAIRO:
        print('Warning: pycairo not available -- using fallback icon names', file=sys.stderr)
    if not driver_present():
        print('x120x driver not found -- is the module loaded?', file=sys.stderr)

    applet = X120xTray()
    Gtk.main()
