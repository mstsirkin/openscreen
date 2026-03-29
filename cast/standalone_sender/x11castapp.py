#!/usr/bin/env python3
"""castapp - Launch an application and cast its window to Chromecast/Google TV.

Usage:
  castapp vlc movie.mp4
  castapp -d "Living Room TV" firefox
  castapp -- gimp -n

Launches the given command, waits for its window to appear, discovers
cast devices, prompts the user to pick one, and starts casting.
"""

import json
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
DEFAULT_NULL_SINK = "x11cast"


def _get_all_pids(root_pid):
    """Return root_pid plus all descendant PIDs discoverable via /proc."""
    pids = {root_pid}
    children_by_parent = {}

    try:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            status_path = os.path.join("/proc", entry, "status")
            try:
                with open(status_path, "r", encoding="utf-8") as status_file:
                    ppid = None
                    for line in status_file:
                        if line.startswith("PPid:"):
                            ppid = int(line.split()[1])
                            break
            except OSError:
                continue
            if ppid is None:
                continue
            pid = int(entry)
            children_by_parent.setdefault(ppid, []).append(pid)
    except OSError:
        return pids

    stack = [root_pid]
    while stack:
        pid = stack.pop()
        for child_pid in children_by_parent.get(pid, []):
            if child_pid not in pids:
                pids.add(child_pid)
                stack.append(child_pid)

    return pids


def ensure_null_sink(sink_name):
    """Create a PulseAudio/PipeWire null sink if it doesn't already exist."""
    try:
        out = subprocess.check_output(
            ["pactl", "-f", "json", "list", "sinks"], text=True,
        )
        for sink in json.loads(out):
            if sink.get("name") == sink_name:
                log.debug("Null sink %s already exists", sink_name)
                return
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        pass

    subprocess.run(
        ["pactl", "load-module", "module-null-sink",
         f"sink_name={sink_name}",
         f"sink_properties=device.description={sink_name}"],
        check=True, capture_output=True,
    )
    log.info("Created null sink: %s", sink_name)


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

    Returns list of (wid_hex, wpid, title) tuples.  Uses wmctrl -l -p
    which lists all windows with their PID.  We also check child PIDs
    since many apps fork (e.g. vlc, firefox).
    """
    deadline = time.monotonic() + timeout if timeout else None
    while deadline is None or time.monotonic() < deadline:
        try:
            pids = _get_all_pids(pid)
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
                        windows.append((wid, int(wpid), title))
            if windows:
                return windows
        except subprocess.CalledProcessError:
            pass
        time.sleep(0.5)
    return []


def wait_for_window_ready(wid_hex, timeout=5):
    """Wait until xwininfo reports a stable, viewable window geometry."""
    deadline = time.monotonic() + timeout
    last_geometry = None
    stable_polls = 0

    while time.monotonic() < deadline:
        try:
            out = subprocess.check_output(
                ["xwininfo", "-id", wid_hex],
                text=True,
                stderr=subprocess.DEVNULL,
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            time.sleep(0.2)
            continue

        width = None
        height = None
        viewable = False
        for line in out.splitlines():
            stripped = line.strip()
            if stripped.startswith("Width:"):
                width = int(stripped.split(":", 1)[1].strip())
            elif stripped.startswith("Height:"):
                height = int(stripped.split(":", 1)[1].strip())
            elif stripped == "Map State: IsViewable":
                viewable = True

        geometry = (width, height)
        if viewable and width and height:
            if geometry == last_geometry:
                stable_polls += 1
                if stable_polls >= 2:
                    return True
            else:
                last_geometry = geometry
                stable_polls = 1
        time.sleep(0.2)

    return False


def pick_window(windows):
    """Let user pick from multiple windows, return (wid_hex, wpid) tuple."""
    if len(windows) == 1:
        return windows[0][0], windows[0][1]

    print("Multiple windows found:")
    for i, (wid, wpid, title) in enumerate(windows):
        print(f"  [{i}] {title} ({wid})")
    try:
        idx = int(input("Select window: "))
        return windows[idx][0], windows[idx][1]
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
    parser.add_argument("-m", "--mute", action="store_true",
                        help="Mute local audio while casting (redirects app audio to a null sink)")
    parser.add_argument("--sink", default=DEFAULT_NULL_SINK,
                        help=f"PulseAudio null sink name for --mute (default: {DEFAULT_NULL_SINK})")
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
    app_env = None

    # Set up audio muting if requested. This only affects the launched app by
    # setting its PulseAudio-compatible sink selection at process start.
    sink_name = args.sink
    if args.mute:
        ensure_null_sink(sink_name)
        app_env = os.environ.copy()
        app_env["PULSE_SINK"] = sink_name
        print(f"Muting local audio for launched app with PULSE_SINK={sink_name}")

    # Launch the application
    print(f"Launching: {' '.join(cmd)}")
    app_proc = subprocess.Popen(
        cmd, start_new_session=True,
        env=app_env,
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

    wid_hex, window_pid = pick_window(windows)
    title = next((t for w, p, t in windows if w == wid_hex), wid_hex)
    wait_for_window_ready(wid_hex)
    print(f"Window: {title} ({wid_hex}, pid {window_pid})")

    # Start casting
    cast_cmd = [X11CAST_BIN, "-w", wid_hex]
    if args.mute:
        cast_cmd += ["-s", f"{sink_name}.monitor"]
    cast_cmd.append(target)
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
