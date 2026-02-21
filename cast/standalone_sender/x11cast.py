#!/usr/bin/env python3
"""x11cast - Cast X11 desktop, windows, or video files to Chromecast/Google TV.

GTK3 system tray app for XFCE. Uses pychromecast for device discovery
and invokes native x11cast/cast_sender binaries for streaming.
"""

import logging
import os
import signal
import subprocess
import sys
import threading
import time

import gi
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, GLib

import pychromecast
from pychromecast import CastBrowser, SimpleCastListener
from zeroconf import Zeroconf

log = logging.getLogger("x11cast")

# Look for binaries in the same directory as this script first.
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
os.environ["PATH"] = SCRIPT_DIR + ":" + os.environ.get("PATH", "")

X11CAST_BIN = "x11cast"
CAST_SENDER_BIN = "cast_sender"


def list_windows():
    """Return list of (window_id_hex, title) for all normal windows."""
    windows = []
    try:
        out = subprocess.check_output(
            ["wmctrl", "-l"], text=True, stderr=subprocess.DEVNULL
        )
        for line in out.strip().splitlines():
            parts = line.split(None, 3)
            if len(parts) >= 4:
                windows.append((parts[0], parts[3]))
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return windows


class CastController:
    """Manages device discovery and native binary invocation."""

    def __init__(self):
        self.zconf = None
        self.browser = None
        self.devices = {}  # friendly_name -> CastInfo
        self.selected_device = None
        self.selected_host = None
        self.selected_port = None
        self.proc = None  # running native binary
        self.casting = False
        self.cast_mode = None

    def discover(self, callback=None):
        self.devices = {}

        def _on_add(uuid, name):
            info = self.browser.devices.get(uuid)
            if info:
                self.devices[info.friendly_name] = info
                log.info("Discovered: %s @ %s:%d",
                         info.friendly_name, info.host, info.port)

        def _run():
            self.zconf = Zeroconf()
            listener = SimpleCastListener(_on_add)
            self.browser = CastBrowser(listener, self.zconf)
            self.browser.start_discovery()
            time.sleep(5)
            self.browser.stop_discovery()
            log.info("Discovery done: %d device(s)", len(self.devices))
            if callback:
                callback(dict(self.devices))

        if callback:
            threading.Thread(target=_run, daemon=True).start()
        else:
            _run()

    def select(self, friendly_name):
        info = self.devices.get(friendly_name)
        if not info:
            return False
        self.selected_device = friendly_name
        self.selected_host = str(info.host)
        self.selected_port = info.port
        log.info("Selected: %s @ %s:%d",
                 friendly_name, self.selected_host, self.selected_port)
        return True

    def _target(self):
        return f"{self.selected_host}:{self.selected_port}"

    def _start_proc(self, cmd):
        log.info("Running: %s", " ".join(cmd))
        self.proc = subprocess.Popen(
            cmd, start_new_session=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def cast_desktop(self):
        if not self.selected_host:
            return False
        self.stop_casting()
        cmd = [X11CAST_BIN, self._target()]
        self._start_proc(cmd)
        self.casting = True
        self.cast_mode = "desktop"
        return True

    def cast_window(self, wid):
        if not self.selected_host:
            return False
        self.stop_casting()
        cmd = [X11CAST_BIN, "-w", wid, self._target()]
        self._start_proc(cmd)
        self.casting = True
        self.cast_mode = "window"
        return True

    def cast_file(self, filepath):
        if not self.selected_host:
            return False
        self.stop_casting()
        cmd = [CAST_SENDER_BIN, "-n", self._target(), filepath]
        self._start_proc(cmd)
        self.casting = True
        self.cast_mode = "file"
        return True

    def stop_casting(self):
        if self.proc and self.proc.poll() is None:
            log.info("Stopping cast (pid %d)", self.proc.pid)
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None
        self.casting = False
        self.cast_mode = None

    def disconnect(self):
        self.stop_casting()
        if self.zconf:
            try:
                self.zconf.close()
            except Exception:
                pass
            self.zconf = None


class X11CastApp:
    """GTK3 StatusIcon tray application."""

    def __init__(self):
        self.ctrl = CastController()
        self.icon = Gtk.StatusIcon()
        self.icon.set_from_icon_name("video-display")
        self.icon.set_title("x11cast")
        self.icon.set_tooltip_text("x11cast - Click to cast")
        self.icon.connect("popup-menu", self._on_popup)
        self.icon.connect("activate", self._on_popup)
        self.icon.set_visible(True)
        self._scanning = False

    def _on_popup(self, icon, button=None, activate_time=None):
        if button is None:
            button = 0
            activate_time = Gtk.get_current_event_time()

        menu = Gtk.Menu()

        if self.ctrl.casting:
            s = Gtk.MenuItem(
                label=f"Casting {self.ctrl.cast_mode} to {self.ctrl.selected_device}")
            s.set_sensitive(False)
            menu.append(s)
            menu.append(Gtk.SeparatorMenuItem())

        if self._scanning:
            s = Gtk.MenuItem(label="Scanning...")
            s.set_sensitive(False)
        else:
            s = Gtk.MenuItem(label="Scan for devices")
            s.connect("activate", self._on_scan)
        menu.append(s)

        if self.ctrl.devices:
            sub = Gtk.Menu()
            for name in sorted(self.ctrl.devices):
                item = Gtk.CheckMenuItem(label=name)
                if name == self.ctrl.selected_device:
                    item.set_active(True)
                item.connect("activate", self._on_select_device, name)
                sub.append(item)
            d = Gtk.MenuItem(label="Devices")
            d.set_submenu(sub)
            menu.append(d)

        menu.append(Gtk.SeparatorMenuItem())

        connected = self.ctrl.selected_host is not None

        item = Gtk.MenuItem(label="Cast Desktop")
        item.set_sensitive(connected and not self.ctrl.casting)
        item.connect("activate", self._on_cast_desktop)
        menu.append(item)

        item = Gtk.MenuItem(label="Cast Window (list)...")
        item.set_sensitive(connected and not self.ctrl.casting)
        item.connect("activate", self._on_cast_window)
        menu.append(item)

        item = Gtk.MenuItem(label="Cast Window (click)...")
        item.set_sensitive(connected and not self.ctrl.casting)
        item.connect("activate", self._on_cast_window_pick)
        menu.append(item)

        item = Gtk.MenuItem(label="Cast File...")
        item.set_sensitive(connected and not self.ctrl.casting)
        item.connect("activate", self._on_cast_file)
        menu.append(item)

        if self.ctrl.casting:
            item = Gtk.MenuItem(label="Stop Casting")
            item.connect("activate", self._on_stop)
            menu.append(item)

        menu.append(Gtk.SeparatorMenuItem())
        item = Gtk.MenuItem(label="Quit")
        item.connect("activate", self._on_quit)
        menu.append(item)

        menu.show_all()
        menu.popup(None, None, Gtk.StatusIcon.position_menu,
                   icon, button, activate_time)

    def _on_scan(self, item):
        self._scanning = True
        self.icon.set_tooltip_text("x11cast - Scanning...")

        def done(devices):
            self._scanning = False
            n = len(devices)
            GLib.idle_add(self.icon.set_tooltip_text,
                          f"x11cast - {n} device(s)")

        self.ctrl.discover(callback=done)

    def _on_select_device(self, item, name):
        if not item.get_active():
            return
        self.ctrl.select(name)
        self.icon.set_tooltip_text(f"x11cast - {name}")

    def _on_cast_desktop(self, item):
        self.ctrl.cast_desktop()
        self._update_icon()

    def _on_cast_window(self, item):
        windows = list_windows()
        if not windows:
            return

        dialog = Gtk.Dialog(title="Select Window", flags=Gtk.DialogFlags.MODAL)
        dialog.add_button("Cancel", Gtk.ResponseType.CANCEL)
        dialog.add_button("Cast", Gtk.ResponseType.OK)
        dialog.set_default_size(400, 300)

        scroll = Gtk.ScrolledWindow()
        listbox = Gtk.ListBox()
        listbox.set_selection_mode(Gtk.SelectionMode.SINGLE)

        for wid, title in windows:
            row = Gtk.ListBoxRow()
            label = Gtk.Label(label=title, xalign=0)
            label.set_margin_start(8)
            label.set_margin_end(8)
            label.set_margin_top(4)
            label.set_margin_bottom(4)
            row.add(label)
            row.wid = wid
            row.title = title
            listbox.add(row)

        scroll.add(listbox)
        dialog.get_content_area().pack_start(scroll, True, True, 0)
        dialog.show_all()
        if listbox.get_children():
            listbox.select_row(listbox.get_children()[0])

        response = dialog.run()
        selected = listbox.get_selected_row()
        dialog.destroy()

        if response == Gtk.ResponseType.OK and selected:
            self.ctrl.cast_window(selected.wid)
            self._update_icon()

    def _on_cast_window_pick(self, item):
        try:
            # xwininfo prompts user to click on a window, then we
            # extract the window ID from its output.
            out = subprocess.check_output(
                ["xwininfo"], text=True, stderr=subprocess.DEVNULL,
            )
            for line in out.splitlines():
                if "Window id:" in line:
                    # "xwininfo: Window id: 0x04800003 ..."
                    wid = line.split("Window id:")[1].strip().split()[0]
                    self.ctrl.cast_window(wid)
                    self._update_icon()
                    break
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass

    def _on_cast_file(self, item):
        dialog = Gtk.FileChooserDialog(
            title="Select Video File",
            action=Gtk.FileChooserAction.OPEN,
        )
        dialog.add_button("Cancel", Gtk.ResponseType.CANCEL)
        dialog.add_button("Cast", Gtk.ResponseType.OK)

        filt = Gtk.FileFilter()
        filt.set_name("Video files")
        for p in ["*.mp4", "*.mkv", "*.avi", "*.webm", "*.mov",
                   "*.flv", "*.wmv", "*.m4v", "*.ts"]:
            filt.add_pattern(p)
        dialog.add_filter(filt)

        filt_all = Gtk.FileFilter()
        filt_all.set_name("All files")
        filt_all.add_pattern("*")
        dialog.add_filter(filt_all)

        response = dialog.run()
        filepath = dialog.get_filename()
        dialog.destroy()

        if response == Gtk.ResponseType.OK and filepath:
            self.ctrl.cast_file(filepath)
            self._update_icon()

    def _on_stop(self, item):
        self.ctrl.stop_casting()
        self.icon.set_from_icon_name("video-display")
        self.icon.set_tooltip_text("x11cast - Stopped")

    def _on_quit(self, item):
        self.ctrl.disconnect()
        Gtk.main_quit()

    def _update_icon(self):
        if self.ctrl.casting:
            self.icon.set_from_icon_name("media-playback-start")
            self.icon.set_tooltip_text(
                f"x11cast - {self.ctrl.cast_mode} -> {self.ctrl.selected_device}")

    def run(self):
        Gtk.main()


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Cast X11 desktop, windows, or video files to Chromecast"
    )
    parser.add_argument(
        "mode", nargs="?", default="tray",
        choices=["tray", "desktop", "window", "file", "list"],
    )
    parser.add_argument("-f", "--file", dest="filepath")
    parser.add_argument("-w", "--window", dest="window_id")
    parser.add_argument("-d", "--device", help="Device name")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    if args.mode == "tray":
        signal.signal(signal.SIGINT, signal.SIG_DFL)
        app = X11CastApp()
        log.info("x11cast started - click the tray icon")
        app.run()
        return

    ctrl = CastController()

    if args.mode == "list":
        print("Scanning...")
        ctrl.discover()
        for name in sorted(ctrl.devices):
            info = ctrl.devices[name]
            print(f"  {name} @ {info.host}:{info.port}")
        ctrl.disconnect()
        return

    # Need a device for desktop/window/file modes
    print("Scanning...")
    ctrl.discover()
    if not ctrl.devices:
        print("No devices found.")
        sys.exit(1)

    if args.device:
        device_name = args.device
    else:
        names = sorted(ctrl.devices)
        if len(names) == 1:
            device_name = names[0]
        else:
            for i, n in enumerate(names):
                print(f"  [{i}] {n}")
            try:
                device_name = names[int(input("Select device: "))]
            except (ValueError, IndexError, EOFError):
                sys.exit(1)

    if not ctrl.select(device_name):
        print(f"Device not found: {device_name}")
        sys.exit(1)

    if args.mode == "desktop":
        print(f"Casting desktop to {device_name}... Ctrl+C to stop.")
        ctrl.cast_desktop()
    elif args.mode == "window":
        wid = args.window_id
        if not wid:
            windows = list_windows()
            for i, (w, t) in enumerate(windows):
                print(f"  [{i}] {t} ({w})")
            try:
                wid = windows[int(input("Select window: "))][0]
            except (ValueError, IndexError, EOFError):
                sys.exit(1)
        print(f"Casting window {wid} to {device_name}... Ctrl+C to stop.")
        ctrl.cast_window(wid)
    elif args.mode == "file":
        if not args.filepath:
            print("--file required")
            sys.exit(1)
        print(f"Casting {args.filepath} to {device_name}... Ctrl+C to stop.")
        ctrl.cast_file(args.filepath)

    try:
        ctrl.proc.wait()
    except KeyboardInterrupt:
        pass
    finally:
        print("\nStopping...")
        ctrl.disconnect()


if __name__ == "__main__":
    main()
