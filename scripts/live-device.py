#!/usr/bin/env python3
"""Live JarvisRobot device diagnostics over the local HTTP API."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


DEFAULT_HOST = os.environ.get("JARVIS_DEVICE_HOST", "esp-claw.local")
REPORT_DIR = ".build_logs/live-device"

SIGNATURES = [
    ("FAIL", "Gateway stop timed out", "Gemini stop path blocked"),
    ("FAIL", "WS connect timeout", "Gemini WebSocket connection timeout"),
    ("FAIL", "setupComplete timeout", "Gemini setupComplete missing"),
    ("FAIL", "Current mode playback conflict", "Audio mode conflict"),
    ("OK", "Current mode(playback) and peer mode have same sample_rate", "Audio playback/record rates matched"),
    ("OK", "Tap: local scene", "Touch routed to local demo"),
    ("FAIL", "Touch controller must be initialized", "Touch controller init failed"),
    ("FAIL", "rwave_task skip: state=", "Reactive face task is skipping frames"),
    ("FAIL", "display arbiter NOT owned", "Reactive face does not own display"),
    ("FAIL", "display snapshot unavailable", "Display snapshot mirror unavailable"),
    ("FAIL", "WS start returned ESP_FAIL", "Gemini WebSocket task failed to start"),
    ("FAIL", "display flush sync disabled", "Display flush sync disabled after timeouts"),
    ("WARN", "display flush completion timeout", "Display flush completion timeout"),
    ("WARN", "i2s_channel_disable", "I2S disable warning"),
    ("WARN", "No response received from Gemini", "Gemini did not return text/audio"),
    ("WARN", "audio_parts=0", "Gemini session produced no audio parts"),
    ("OK", "WS start returned ESP_OK", "Gemini WebSocket task started"),
    ("OK", "setupComplete", "Gemini setup complete"),
    ("OK", "Tap: toggling Gemini Live on", "Touch routed to Gemini start"),
    ("OK", "Tap: toggling Gemini Live off", "Touch routed to Gemini stop"),
    ("OK", "rwave state applied", "Reactive face state applied"),
    ("OK", "display snapshot stream", "Display snapshot requested"),
    ("OK", "display snapshot saved", "Display snapshot saved to SD"),
]

BOOT_MARKERS = (
    "jarvis_logger: persistent logging ->",
    "app: Persistent logging",
)


@dataclass
class Check:
    level: str
    label: str
    detail: str


class Device:
    def __init__(self, host: str, timeout: float) -> None:
        base = host.strip()
        if not base.startswith(("http://", "https://")):
            base = "http://" + base
        self.base = base.rstrip("/")
        self.timeout = timeout

    def request(self, method: str, path: str, body: dict[str, Any] | None = None) -> tuple[int, str, Any]:
        data = None
        headers = {"Accept": "application/json,text/plain,*/*"}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
                parsed: Any = None
                try:
                    parsed = json.loads(raw)
                except json.JSONDecodeError:
                    parsed = raw
                return resp.status, raw, parsed
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            try:
                parsed = json.loads(raw)
            except json.JSONDecodeError:
                parsed = raw
            return exc.code, raw, parsed

    def get(self, path: str) -> tuple[int, str, Any]:
        return self.request("GET", path)

    def post(self, path: str, body: dict[str, Any]) -> tuple[int, str, Any]:
        return self.request("POST", path, body)

    def get_bytes(self, path: str) -> tuple[int, dict[str, str], bytes]:
        req = urllib.request.Request(self.base + path, headers={"Accept": "*/*"}, method="GET")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.status, dict(resp.headers.items()), resp.read()
        except urllib.error.HTTPError as exc:
            return exc.code, dict(exc.headers.items()), exc.read()


def now_slug() -> str:
    return dt.datetime.now().strftime("%Y%m%d-%H%M%S")


def status_value(payload: Any, *names: str) -> Any:
    if not isinstance(payload, dict):
        return None
    for name in names:
        if name in payload:
            return payload[name]
    return None


def short_json(value: Any) -> str:
    if isinstance(value, (dict, list)):
        return json.dumps(value, sort_keys=True, separators=(",", ":"))[:500]
    return str(value).strip()[:500]


def classify_logs(log_text: str) -> list[Check]:
    checks: list[Check] = []
    for level, needle, label in SIGNATURES:
        if needle in log_text:
            checks.append(Check(level, label, needle))
    return checks


def log_window_for_checks(log_text: str, max_lines: int | None = None) -> str:
    """Classify the current boot/window, not stale SD log lines from an older run."""
    lines = log_text.splitlines()
    start = 0
    for idx, line in enumerate(lines):
        if any(marker in line for marker in BOOT_MARKERS):
            start = idx
    window = lines[start:]
    if max_lines is not None:
        window = window[-max_lines:]
    return "\n".join(window)


def print_check(level: str, label: str, detail: str = "") -> None:
    suffix = f" - {detail}" if detail else ""
    print(f"{level:<4} {label}{suffix}")


def collect_snapshot(dev: Device, tail: int) -> dict[str, Any]:
    snapshot: dict[str, Any] = {
        "base": dev.base,
        "captured_at": dt.datetime.now().isoformat(timespec="seconds"),
        "endpoints": {},
    }
    for path in (
        "/api/health",
        "/api/status",
        "/api/audio/level",
        "/api/gemini/live",
        "/api/touch",
        "/api/display/face",
        "/api/display/snapshot.json",
    ):
        try:
            code, raw, parsed = dev.get(path)
            snapshot["endpoints"][path] = {"http": code, "body": parsed, "raw": raw[:2000]}
        except Exception as exc:  # noqa: BLE001 - CLI diagnostic should report transport detail.
            snapshot["endpoints"][path] = {"error": str(exc)}
    try:
        code, raw, _ = dev.get(f"/api/logs?tail={tail}")
        snapshot["logs"] = {"http": code, "tail": tail, "text": raw}
    except Exception as exc:  # noqa: BLE001
        snapshot["logs"] = {"error": str(exc), "text": ""}
    return snapshot


def print_snapshot(snapshot: dict[str, Any]) -> int:
    failures = 0
    for path, result in snapshot["endpoints"].items():
        if "error" in result:
            print_check("FAIL", path, result["error"])
            failures += 1
            continue
        code = result["http"]
        body = result.get("body")
        level = "OK" if 200 <= code < 300 else "FAIL"
        if level == "FAIL":
            failures += 1
        print_check(level, f"{path} http={code}", short_json(body))

    logs = snapshot.get("logs", {})
    if "error" in logs:
        print_check("FAIL", "/api/logs", logs["error"])
        failures += 1
    else:
        print_check("OK", f"/api/logs http={logs.get('http')} tail={logs.get('tail')}")
        for check in classify_logs(log_window_for_checks(str(logs.get("text", "")))):
            print_check(check.level, check.label, check.detail)
            if check.level == "FAIL":
                failures += 1
    return failures


def command_status(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    snapshot = collect_snapshot(dev, args.tail)
    failures = print_snapshot(snapshot)
    if args.json:
        print(json.dumps(snapshot, indent=2, sort_keys=True))
    return 1 if failures else 0


def wait_gemini_state(dev: Device, wanted: set[str], deadline: float) -> tuple[bool, Any]:
    last: Any = None
    while time.time() < deadline:
        code, _, body = dev.get("/api/gemini/live")
        last = body
        if code == 200:
            state = str(status_value(body, "state", "status") or "").upper()
            if state in wanted:
                return True, body
        time.sleep(0.5)
    return False, last


def command_gemini_cycle(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    failures = 0

    print_check("RUN", "Gemini start")
    code, _, body = dev.post("/api/gemini/live", {"action": "start"})
    print_check("OK" if 200 <= code < 300 else "FAIL", f"POST start http={code}", short_json(body))
    if not (200 <= code < 300):
        return 1

    ok, body = wait_gemini_state(dev, {"LISTENING", "SPEAKING", "THINKING"}, time.time() + args.start_timeout)
    print_check("OK" if ok else "FAIL", "Gemini active state", short_json(body))
    failures += 0 if ok else 1

    if args.text:
        print_check("RUN", "Gemini text turn", args.text)
        code, _, body = dev.post("/api/gemini/live", {"action": "text", "text": args.text})
        print_check("OK" if 200 <= code < 300 else "FAIL", f"POST text http={code}", short_json(body))
        failures += 0 if 200 <= code < 300 else 1
        time.sleep(args.turn_wait)

    code, _, body = dev.get("/api/gemini/live")
    print_check("OK" if code == 200 else "FAIL", f"Gemini status http={code}", short_json(body))
    if isinstance(body, dict):
        parts = int(body.get("audio_parts") or 0)
        if args.text and parts <= 0:
            print_check("WARN", "No audio parts observed after text turn", f"audio_parts={parts}")

    print_check("RUN", "Gemini stop")
    code, _, body = dev.post("/api/gemini/live", {"action": "stop"})
    print_check("OK" if 200 <= code < 300 else "FAIL", f"POST stop http={code}", short_json(body))
    failures += 0 if 200 <= code < 300 else 1

    ok, body = wait_gemini_state(dev, {"IDLE", "STOPPED"}, time.time() + args.stop_timeout)
    print_check("OK" if ok else "FAIL", "Gemini idle after stop", short_json(body))
    failures += 0 if ok else 1

    snapshot = collect_snapshot(dev, args.tail)
    logs = str(snapshot.get("logs", {}).get("text", ""))
    for check in classify_logs(log_window_for_checks(logs)):
        print_check(check.level, check.label, check.detail)
        if check.level == "FAIL":
            failures += 1

    if args.report:
        write_report(snapshot, "gemini-cycle")
    return 1 if failures else 0


def command_logs(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    code, raw, _ = dev.get(f"/api/logs?tail={args.tail}")
    if code != 200:
        print_check("FAIL", f"/api/logs http={code}", raw[:500])
        return 1
    needles = [x.strip() for x in args.grep.split(",") if x.strip()]
    lines = raw.splitlines()
    if needles:
        lines = [line for line in lines if any(needle.lower() in line.lower() for needle in needles)]
    visible_lines = lines[-args.lines :]
    for line in visible_lines:
        print(line)
    failures = 0
    for check in classify_logs(log_window_for_checks(raw)):
        print_check(check.level, check.label, check.detail)
        failures += 1 if check.level == "FAIL" else 0
    return 1 if failures else 0


def write_report(snapshot: dict[str, Any], prefix: str) -> str:
    os.makedirs(REPORT_DIR, exist_ok=True)
    path = os.path.join(REPORT_DIR, f"{now_slug()}-{prefix}.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(snapshot, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print_check("OK", "report written", path)
    return path


def convert_ppm_to_png(ppm_path: str) -> str | None:
    png_path = ppm_path[:-4] + ".png" if ppm_path.endswith(".ppm") else ppm_path + ".png"
    if shutil.which("sips"):
        result = subprocess.run(
            ["sips", "-s", "format", "png", ppm_path, "--out", png_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0 and os.path.exists(png_path):
            return png_path
    if shutil.which("magick"):
        result = subprocess.run(
            ["magick", ppm_path, png_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0 and os.path.exists(png_path):
            return png_path
    return None


def command_screen(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    query = "?save=1" if args.save_sd else ""
    code, headers, data = dev.get_bytes(f"/api/display/snapshot.ppm{query}")
    if code != 200:
        detail = data.decode("utf-8", errors="replace")[:500]
        print_check("FAIL", f"/api/display/snapshot.ppm http={code}", detail)
        return 1

    os.makedirs(REPORT_DIR, exist_ok=True)
    ppm_path = os.path.join(REPORT_DIR, f"{now_slug()}-screen.ppm")
    with open(ppm_path, "wb") as fh:
        fh.write(data)

    capture_source = headers.get("X-Jarvis-Display-Source", "unknown")
    display_owner = headers.get("X-Jarvis-Display-Owner", "unknown")
    panel_readback = headers.get("X-Jarvis-Panel-Readback", "false")
    mirror_fresh = headers.get("X-Jarvis-Display-Fresh", "unknown")
    print_check("OK", "display software mirror captured", ppm_path)
    print_check(
        "OK",
        "display capture source",
        f"source={capture_source} owner={display_owner} panel_readback={panel_readback} mirror_fresh={mirror_fresh}",
    )
    if headers.get("X-Jarvis-Saved-Path"):
        print_check("OK", "device SD copy", headers["X-Jarvis-Saved-Path"])
    if headers.get("X-Jarvis-Display-Frame"):
        dims = f"{headers.get('X-Jarvis-Display-Width', '?')}x{headers.get('X-Jarvis-Display-Height', '?')}"
        print_check("OK", "display frame", f"{headers['X-Jarvis-Display-Frame']} {dims}")

    png_path = convert_ppm_to_png(ppm_path) if args.png else None
    if png_path:
        print_check("OK", "PNG converted", png_path)
    elif args.png:
        print_check("WARN", "PNG conversion skipped", "sips/magick unavailable or failed")

    if args.logs:
        try:
            _, raw, _ = dev.get(f"/api/logs?tail={args.tail}")
            for check in classify_logs(log_window_for_checks(raw)):
                print_check(check.level, check.label, check.detail)
        except Exception as exc:  # noqa: BLE001
            print_check("WARN", "log integration unavailable", str(exc))
    return 0


def command_report(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    snapshot = collect_snapshot(dev, args.tail)
    failures = print_snapshot(snapshot)
    write_report(snapshot, "report")
    return 1 if failures else 0


def command_restart(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    before_uptime = None
    try:
        _, _, health = dev.get("/api/health")
        if isinstance(health, dict):
            before_uptime = health.get("uptime_ms")
    except Exception:
        pass

    code, _, body = dev.post("/api/restart", {})
    print_check("OK" if 200 <= code < 300 else "FAIL", f"POST /api/restart http={code}", short_json(body))
    if not (200 <= code < 300):
        return 1

    saw_down = False
    deadline = time.time() + args.wait
    while time.time() < deadline:
        time.sleep(args.interval)
        try:
            code, _, body = dev.get("/api/health")
            if code == 200 and isinstance(body, dict):
                uptime = body.get("uptime_ms")
                if saw_down or before_uptime is None or (isinstance(uptime, (int, float)) and uptime < before_uptime):
                    print_check("OK", "device back online", short_json(body))
                    return 0
                print_check("WAIT", "device still online", f"uptime_ms={uptime}")
            else:
                saw_down = True
                print_check("WAIT", f"health http={code}", short_json(body))
        except Exception as exc:  # noqa: BLE001
            saw_down = True
            print_check("WAIT", "device down", str(exc)[:160])
    print_check("FAIL", "device did not return after restart", f"timeout={args.wait}s")
    return 1


def command_face(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    body = {"state": args.state}
    if args.amp is not None:
        body["amp"] = args.amp
    code, _, payload = dev.post("/api/display/face", body)
    print_check("OK" if 200 <= code < 300 else "FAIL", f"POST /api/display/face http={code}", short_json(payload))
    if not (200 <= code < 300):
        return 1
    if args.screen:
        args.save_sd = args.save_sd_screen
        args.png = True
        args.logs = True
        return command_screen(args)
    return 0


def command_touch(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    failures = 0

    if args.scene:
        code, _, body = dev.post("/api/touch", {"scene": args.scene})
        print_check("OK" if 200 <= code < 300 else "FAIL", f"POST /api/touch http={code}", short_json(body))
        failures += 0 if 200 <= code < 300 else 1
        time.sleep(args.wait)

    code, _, body = dev.get("/api/touch")
    print_check("OK" if code == 200 else "FAIL", f"GET /api/touch http={code}", short_json(body))
    failures += 0 if code == 200 else 1

    code, raw, _ = dev.get(f"/api/logs?tail={args.tail}")
    if code == 200:
        for line in [ln for ln in raw.splitlines() if "touch" in ln.lower()][-args.lines :]:
            print(line)
    else:
        print_check("WARN", f"/api/logs http={code}", raw[:300])

    if args.screen:
        args.save_sd = args.save_sd_screen
        args.png = True
        args.logs = True
        screen_result = command_screen(args)
        failures += 0 if screen_result == 0 else 1
    return 1 if failures else 0


def command_audio(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    deadline = time.time() + args.seconds
    samples: list[dict[str, Any]] = []
    while time.time() < deadline:
        try:
            code, _, body = dev.get("/api/audio/level")
            if code == 200 and isinstance(body, dict):
                samples.append(body)
                print_check("OK" if body.get("valid") else "WARN", "mic level",
                            f"rms={body.get('rms_db')} peak={body.get('peak_db')} valid={body.get('valid')}")
            else:
                print_check("FAIL", f"/api/audio/level http={code}", short_json(body))
        except Exception as exc:  # noqa: BLE001
            print_check("FAIL", "/api/audio/level", str(exc))
        time.sleep(args.interval)

    valid = [s for s in samples if s.get("valid")]
    if valid:
        rms_vals = [float(s.get("rms_db", -80.0)) for s in valid]
        peak_vals = [float(s.get("peak_db", -80.0)) for s in valid]
        print_check("OK", "mic summary",
                    f"samples={len(samples)} valid={len(valid)} rms_range={min(rms_vals):.1f}..{max(rms_vals):.1f} peak_max={max(peak_vals):.1f}")
    else:
        print_check("WARN", "mic summary", f"samples={len(samples)} valid=0")

    if args.speaker:
        cycle_args = argparse.Namespace(
            host=args.host,
            timeout=args.timeout,
            tail=args.tail,
            text=args.text,
            turn_wait=args.turn_wait,
            start_timeout=12.0,
            stop_timeout=12.0,
            report=args.report,
        )
        return command_gemini_cycle(cycle_args)
    return 0 if valid else 1


def command_watch(args: argparse.Namespace) -> int:
    dev = Device(args.host, args.timeout)
    last_logs = ""
    while True:
        snapshot = collect_snapshot(dev, args.tail)
        os.system("clear")
        failures = print_snapshot(snapshot)
        logs = str(snapshot.get("logs", {}).get("text", ""))
        if logs != last_logs:
            print("\n-- recent matching logs --")
            needles = ("gemini", "touch", "rwave", "audio", "ws", "setupComplete")
            for line in [ln for ln in logs.splitlines() if any(n in ln.lower() for n in needles)][-args.lines :]:
                print(line)
            last_logs = logs
        print(f"\nrefresh={args.interval}s failures={failures} ctrl-c to exit")
        time.sleep(args.interval)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"device host or URL (default: {DEFAULT_HOST})")
    parser.add_argument("--timeout", type=float, default=4.0, help="HTTP timeout seconds")
    parser.add_argument("--tail", type=int, default=65536, help="log tail bytes")
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--host", default=argparse.SUPPRESS, help="device host or URL")
        p.add_argument("--timeout", type=float, default=argparse.SUPPRESS, help="HTTP timeout seconds")
        p.add_argument("--tail", type=int, default=argparse.SUPPRESS, help="log tail bytes")

    p_status = sub.add_parser("status", help="probe health/status/audio/Gemini/touch/logs once")
    add_common(p_status)
    p_status.add_argument("--json", action="store_true", help="also print raw JSON snapshot")
    p_status.set_defaults(func=command_status)

    p_report = sub.add_parser("report", help="write a timestamped JSON diagnostic report")
    add_common(p_report)
    p_report.set_defaults(func=command_report)

    p_restart = sub.add_parser("restart", help="restart the device and wait for it to return")
    add_common(p_restart)
    p_restart.add_argument("--wait", type=float, default=45.0, help="seconds to wait for device return")
    p_restart.add_argument("--interval", type=float, default=1.0, help="poll interval seconds")
    p_restart.set_defaults(func=command_restart)

    p_screen = sub.add_parser("screen", help="capture the live device display mirror")
    add_common(p_screen)
    p_screen.add_argument("--save-sd", action="store_true", help="also save the PPM on the device SD card")
    p_screen.add_argument("--png", action="store_true", default=True, help="convert local PPM to PNG when possible")
    p_screen.add_argument("--no-png", dest="png", action="store_false", help="keep only the local PPM")
    p_screen.add_argument("--logs", action="store_true", default=True, help="classify device logs after capture")
    p_screen.add_argument("--no-logs", dest="logs", action="store_false", help="skip log classification")
    p_screen.set_defaults(func=command_screen)

    p_face = sub.add_parser("face", help="force a display face state through HTTP")
    add_common(p_face)
    p_face.add_argument("state", choices=["off", "idle", "listen", "listening", "think", "thinking", "speak", "speaking"])
    p_face.add_argument("--amp", type=int, default=None, help="synthetic amplitude 0..1000 for listen/speak")
    p_face.add_argument("--screen", action="store_true", help="capture the screen after changing state")
    p_face.add_argument("--save-sd-screen", action="store_true", help="also save the captured screen to SD")
    p_face.set_defaults(func=command_face)

    p_touch = sub.add_parser("touch", help="read touch diagnostics or trigger a local scene")
    add_common(p_touch)
    p_touch.add_argument("scene", nargs="?", choices=["listen", "think", "speak", "showcase"],
                         help="optional local scene to trigger through /api/touch")
    p_touch.add_argument("--wait", type=float, default=1.0, help="seconds to wait after triggering a scene")
    p_touch.add_argument("--lines", type=int, default=80, help="touch log lines to print")
    p_touch.add_argument("--screen", action="store_true", help="capture the screen after touch action")
    p_touch.add_argument("--save-sd-screen", action="store_true", help="also save the captured screen to SD")
    p_touch.set_defaults(func=command_touch)

    p_audio = sub.add_parser("audio", help="sample mic levels and optionally exercise speaker via Gemini")
    add_common(p_audio)
    p_audio.add_argument("--seconds", type=float, default=5.0, help="mic sampling duration")
    p_audio.add_argument("--interval", type=float, default=0.5, help="mic sampling interval")
    p_audio.add_argument("--speaker", action="store_true", help="also run a Gemini text turn to exercise speaker path")
    p_audio.add_argument("--text", default="Say one short sentence.", help="text used for speaker path")
    p_audio.add_argument("--turn-wait", type=float, default=6.0, help="seconds to wait after text turn")
    p_audio.add_argument("--report", action="store_true", help="write a report after speaker cycle")
    p_audio.set_defaults(func=command_audio)

    p_logs = sub.add_parser("logs", help="print recent device logs and classify known signatures")
    add_common(p_logs)
    p_logs.add_argument("--grep", default="", help="comma-separated case-insensitive filters")
    p_logs.add_argument("--lines", type=int, default=120, help="lines to print")
    p_logs.set_defaults(func=command_logs)

    p_cycle = sub.add_parser("gemini-cycle", help="start Gemini, optional text turn, stop, classify logs")
    add_common(p_cycle)
    p_cycle.add_argument("--text", default="Say one short sentence.", help="text turn to send; empty disables")
    p_cycle.add_argument("--turn-wait", type=float, default=6.0, help="seconds to wait after text turn")
    p_cycle.add_argument("--start-timeout", type=float, default=12.0, help="seconds to wait for active state")
    p_cycle.add_argument("--stop-timeout", type=float, default=12.0, help="seconds to wait for idle state")
    p_cycle.add_argument("--report", action="store_true", help="write a report after the cycle")
    p_cycle.set_defaults(func=command_gemini_cycle)

    p_watch = sub.add_parser("watch", help="refresh status and matching logs")
    add_common(p_watch)
    p_watch.add_argument("--interval", type=float, default=2.0, help="refresh interval seconds")
    p_watch.add_argument("--lines", type=int, default=40, help="recent matching log lines")
    p_watch.set_defaults(func=command_watch)

    return parser


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:  # noqa: BLE001 - diagnostics should fail loudly.
        print_check("FAIL", "diagnostic crashed", str(exc))
        return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
