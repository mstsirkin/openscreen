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


def get_default_sink():
    """Return the current default PulseAudio sink name."""
    return subprocess.check_output(
        ["pactl", "get-default-sink"], text=True,
    ).strip()


def _get_all_pids(pid):
    """Return set containing pid and all its descendants."""
    pids = {pid}
    try:
        out = subprocess.check_output(
            ["ps", "--ppid", str(pid), "-o", "pid="],
            text=True, stderr=subprocess.DEVNULL,
        )
        for line in out.strip().splitlines():
            child = int(line.strip())
            pids.update(_get_all_pids(child))
    except subprocess.CalledProcessError:
        pass
    return pids


def _get_client_pid(client_id):
    """Look up the PID for a PipeWire/PulseAudio client by its ID."""
    try:
        out = subprocess.check_output(
            ["pactl", "-f", "json", "list", "clients"], text=True,
        )
        for client in json.loads(out):
            if str(client.get("index")) == str(client_id):
                pid = client.get("properties", {}).get(
                    "application.process.id")
                if pid is not None:
                    return int(pid)
    except (subprocess.CalledProcessError, json.JSONDecodeError,
            TypeError, ValueError):
        pass
    return None


def move_sink_inputs_by_pid(pid, sink_name, clear_wireplumber=False):
    """Move all PulseAudio sink-inputs belonging to pid (or children) to sink.

    If clear_wireplumber is True, immediately clear WirePlumber's saved
    routing after each move so that other instances of the same app are
    not affected.
    """
    pids = _get_all_pids(pid)
    log.debug("App PID tree: %s", pids)

    try:
        out = subprocess.check_output(
            ["pactl", "-f", "json", "list", "sink-inputs"], text=True,
        )
        inputs = json.loads(out)
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        return 0

    moved = 0
    for si in inputs:
        props = si.get("properties", {})
        si_index = si.get("index", -1)
        # Try application.process.id first (PulseAudio native clients),
        # then fall back to looking up the client's PID (PipeWire native).
        si_pid = props.get("application.process.id")
        if si_pid is None:
            client_id = si.get("client")
            if client_id is not None:
                si_pid = _get_client_pid(client_id)
        si_name = (props.get("application.name") or
                   props.get("node.name", ""))
        log.debug("Sink-input #%s: pid=%s name=%s",
                  si_index, si_pid, si_name)
        try:
            if int(si_pid) in pids:
                subprocess.run(
                    ["pactl", "move-sink-input", str(si_index), sink_name],
                    capture_output=True, check=True,
                )
                moved += 1
        except (TypeError, ValueError):
            pass

    # Clear WirePlumber's saved target immediately so it doesn't
    # affect other instances of the same app.
    if moved and clear_wireplumber:
        _clear_wireplumber_targets(sink_name)

    return moved


def _clear_wireplumber_targets(sink_name):
    """Remove saved WirePlumber routing targets pointing to sink_name.

    WirePlumber saves stream routing in stream-properties. If we moved
    an app's audio to a null sink, WirePlumber remembers that and will
    route it there on future runs too. This clears those entries.
    """
    state_file = os.path.expanduser(
        "~/.local/state/wireplumber/stream-properties")
    try:
        with open(state_file) as f:
            lines = f.readlines()
    except FileNotFoundError:
        return

    needle = f'"target":"{sink_name}"'
    changed = False
    new_lines = []
    for line in lines:
        if needle in line:
            key, val_str = line.strip().split("=", 1)
            val = json.loads(val_str)
            del val["target"]
            new_lines.append(f"{key}={json.dumps(val)}\n")
            changed = True
        else:
            new_lines.append(line)

    if changed:
        with open(state_file, "w") as f:
            f.writelines(new_lines)
        log.debug("Cleared WirePlumber routing targets for %s", sink_name)


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

    wid_hex, window_pid = pick_window(windows)
    title = next((t for w, p, t in windows if w == wid_hex), wid_hex)
    print(f"Window: {title} ({wid_hex}, pid {window_pid})")

    # Set up audio muting if requested
    sink_name = args.sink
    if args.mute:
        ensure_null_sink(sink_name)
        print(f"Muting local audio (sink: {sink_name})")

    # Start casting
    cast_cmd = [X11CAST_BIN, "-w", wid_hex]
    if args.mute:
        cast_cmd += ["-s", f"{sink_name}.monitor"]
    cast_cmd.append(target)
    log.info("Running: %s", " ".join(cast_cmd))
    print(f"Casting window to {target}... Ctrl+C to stop.")
    cast_proc = subprocess.Popen(cast_cmd, start_new_session=True)

    # Move app's audio to null sink (retry since streams may appear late)
    if args.mute:
        for attempt in range(5):
            time.sleep(1)
            moved = move_sink_inputs_by_pid(
                window_pid, sink_name, clear_wireplumber=True)
            if moved:
                print(f"Muted {moved} audio stream(s) (redirected to {sink_name})")
                break
        else:
            print("Warning: no audio streams found for app (will keep trying)")

    # Wait for either the app or cast process to exit
    try:
        while True:
            if app_proc.poll() is not None:
                print("\nApplication exited.")
                break
            if cast_proc.poll() is not None:
                print("\nCast process exited.")
                break
            # Periodically re-check for new audio streams from the app
            if args.mute:
                move_sink_inputs_by_pid(
                    window_pid, sink_name, clear_wireplumber=True)
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        # Restore audio routing. WirePlumber saves per-role routing
        # in memory and re-persists it, so we must: clear the file,
        # then restart WirePlumber to drop its in-memory state.
        if args.mute:
            default_sink = subprocess.check_output(
                ["pactl", "get-default-sink"], text=True,
            ).strip()
            move_sink_inputs_by_pid(window_pid, default_sink)
            _clear_wireplumber_targets(sink_name)
            subprocess.run(
                ["systemctl", "--user", "restart", "wireplumber"],
                capture_output=True,
            )

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
