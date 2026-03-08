#!/usr/bin/env python3
"""castapp - Launch an application and cast its window to Chromecast/Google TV.

Usage:
  castapp vlc movie.mp4
  castapp -d "Living Room TV" firefox
  castapp -- gimp -n

Launches the given command, waits for its window to appear, discovers
cast devices, prompts the user to pick one, and starts casting.
"""

import logging
import os
import signal
import subprocess
import sys
import time

from pychromecast import CastBrowser, SimpleCastListener
from zeroconf import Zeroconf

log = logging.getLogger("castapp")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
os.environ["PATH"] = SCRIPT_DIR + ":" + os.environ.get("PATH", "")

X11CAST_BIN = "x11cast"


def discover_devices(timeout=5):
    """Return dict of friendly_name -> CastInfo."""
    devices = {}
    zconf = Zeroconf()

    def _on_add(uuid, name):
        info = browser.devices.get(uuid)
        if info:
            devices[info.friendly_name] = info

    listener = SimpleCastListener(_on_add)
    browser = CastBrowser(listener, zconf)
    browser.start_discovery()
    time.sleep(timeout)
    browser.stop_discovery()
    zconf.close()
    return devices


def find_windows_by_pid(pid, timeout=30):
    """Wait for a window owned by pid or its children to appear.

    Returns list of (wid_hex, title) tuples.  Uses wmctrl -l -p which
    lists all windows with their PID.  We also check child PIDs since
    many apps fork (e.g. vlc, firefox).
    """
    def _get_child_pids(parent):
        """Return set of all descendant PIDs of parent."""
        pids = set()
        try:
            out = subprocess.check_output(
                ["ps", "--ppid", str(parent), "-o", "pid="],
                text=True, stderr=subprocess.DEVNULL,
            )
            for line in out.strip().splitlines():
                child = int(line.strip())
                pids.add(child)
                pids.update(_get_child_pids(child))
        except subprocess.CalledProcessError:
            pass
        return pids

    deadline = time.monotonic() + timeout if timeout else None
    while deadline is None or time.monotonic() < deadline:
        try:
            pids = {pid} | _get_child_pids(pid)
            out = subprocess.check_output(
                ["wmctrl", "-l", "-p"], text=True, stderr=subprocess.DEVNULL,
            )
            windows = []
            for line in out.strip().splitlines():
                parts = line.split(None, 4)
                if len(parts) >= 5:
                    wid, _desktop, wpid, _host = parts[:4]
                    title = parts[4]
                    if int(wpid) in pids:
                        windows.append((wid, title))
            if windows:
                return windows
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.5)
    return []


def pick_window(windows):
    """Let user pick from multiple windows, return window ID hex string."""
    if len(windows) == 1:
        return windows[0][0]

    print("Multiple windows found:")
    for i, (wid, title) in enumerate(windows):
        print(f"  [{i}] {title} ({wid})")
    try:
        idx = int(input("Select window: "))
        return windows[idx][0]
    except (ValueError, IndexError, EOFError):
        sys.exit(1)


def pick_device(devices, preselect=None):
    """Let user pick a cast device, return (host, port) tuple."""
    if preselect:
        info = devices.get(preselect)
        if info:
            return str(info.host), info.port
        print(f"Device not found: {preselect}")
        sys.exit(1)

    names = sorted(devices)
    if len(names) == 1:
        name = names[0]
    else:
        for i, n in enumerate(names):
            print(f"  [{i}] {n}")
        try:
            name = names[int(input("Select device: "))]
        except (ValueError, IndexError, EOFError):
            sys.exit(1)

    info = devices[name]
    print(f"Selected: {name}")
    return str(info.host), info.port


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Launch an app and cast its window to Chromecast",
        usage="%(prog)s [options] [--] command [args...]",
    )
    parser.add_argument("-d", "--device", help="Cast device name (skip prompt)")
    parser.add_argument("-t", "--timeout", type=float, default=None,
                        help="Seconds to wait for app window to appear (default: forever)")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="Command to launch")
    args = parser.parse_args()

    # Strip leading '--' from command
    cmd = args.command
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        parser.print_help()
        sys.exit(1)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    # Discover cast devices first
    print("Scanning for cast devices...")
    devices = discover_devices()
    if not devices:
        print("No cast devices found.")
        sys.exit(1)

    host, port = pick_device(devices, preselect=args.device)
    target = f"{host}:{port}"

    # Launch the application
    print(f"Launching: {' '.join(cmd)}")
    app_proc = subprocess.Popen(
        cmd, start_new_session=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    # Wait for its window
    print("Waiting for application window...")
    windows = find_windows_by_pid(app_proc.pid, timeout=args.timeout)
    if not windows:
        print("No window found. Is the application running?")
        rc = app_proc.poll()
        if rc is not None:
            print(f"Application exited with code {rc}.")
        sys.exit(1)

    wid_hex = pick_window(windows)
    # Find title for display
    title = next((t for w, t in windows if w == wid_hex), wid_hex)
    print(f"Window: {title} ({wid_hex})")

    # Start casting
    cast_cmd = [X11CAST_BIN, "-w", wid_hex, target]
    log.info("Running: %s", " ".join(cast_cmd))
    print(f"Casting window to {target}... Ctrl+C to stop.")
    cast_proc = subprocess.Popen(cast_cmd, start_new_session=True)

    # Wait for either the app or cast process to exit
    try:
        while True:
            if app_proc.poll() is not None:
                print("\nApplication exited.")
                break
            if cast_proc.poll() is not None:
                print("\nCast process exited.")
                break
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        print("Stopping...")
        for proc in (cast_proc, app_proc):
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except OSError:
                    proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()


if __name__ == "__main__":
    main()
