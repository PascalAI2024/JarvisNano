#!/usr/bin/env python3
"""jarvisctl — the JarvisNano operator & Dev Workbench 2.0 CLI.

One command for everything an operator (human or AI session) does with the
device over LAN. Host comes from --host or $JARVIS_DEVICE_HOST.

Core Operations:
    jarvisctl status                     # one-line health verdict + key state
    jarvisctl listen | mute              # arm / privacy-pause the voice
    jarvisctl say "text"                 # speak a turn through Gemini
    jarvisctl demo                       # queue the 27 s on-glass showcase reel
    jarvisctl screen [out.png]           # capture the glass (PNG via PIL/sips)
    jarvisctl canvas image.png [--ttl S] # push an image/banner to the glass
    jarvisctl canvas --clear
    jarvisctl tune [pbgain=250] [speakmic=21] [mic=24] [vol=90] [barge=1]
    jarvisctl taps [outdir]              # WAV taps + metrics
    jarvisctl vadlog [out.csv]           # barge/VAD decision log
    jarvisctl voiceperf [bench]          # voice-loop fps/timing
    jarvisctl logs [tail_bytes]          # device log ring — read AFTER, no monitor
    jarvisctl gestures [lines]           # recent physical inputs + resolved actions
    jarvisctl input tap|double|long|swipe [left|right|up|down] [edge]
    jarvisctl takeover [ttl_s]           # Codex owns glass; double-tap exits
    jarvisctl normal                     # release Codex mode explicitly
    jarvisctl desk present ...           # bounded interactive Desk surface
    jarvisctl doctor [--repair]          # diagnose logs/counters safely
    jarvisctl volume 10..100             # paired persistent speaker level
    jarvisctl brightness 10..100         # paired persistent mood ceiling
    jarvisctl ota                        # upload built firmware over Wi-Fi
    jarvisctl art                        # upload face clips (emote_assets.bin)
    jarvisctl update                     # flash built firmware over USB
    jarvisctl reboot                     # watchdog reset via esptool (USB)

Codex Mode 2.0:
    jarvisctl ask "<title>" [--body "..."] [--choices "A,B,C"] [--consent]
                            [--ttl 120] [--wait] [--json]
    jarvisctl run [--title NAME] [--takeover] [--no-result] -- <cmd...>
    jarvisctl agent status|post|clear ...
    jarvisctl mode [--renew [ttl]] [--json]
    jarvisctl inbox [--tag work|life|all] [--done ID] [--add TEXT] [--json]

Dev Workbench 2.0:
    jarvisctl ship [--skip-tests] [--skip-build] [--fast] [--screens] [--force]
    jarvisctl bench [--dials] [--split]
    jarvisctl power [--watch [sec]] [--care on|off] [--json]
    jarvisctl tail [--grep PAT] [--interval 0.8] [--lines N]
    jarvisctl crash [dump [out.bin]|clear|decode [--elf PATH]]
    jarvisctl tasks [--cpu] [--interval 2.0] [--json]
    jarvisctl board [--watch pwr|touch] [--gyro] [--json]

Exit code 0 = healthy/ok. `status` exits 1 when the device is deaf/muted so
scripts and agents can gate on it.
"""
from __future__ import annotations

import argparse
import getpass
import glob
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Sequence

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
KEYCHAIN_SERVICE = "com.ingeniousdigital.jarvisnano.desk"
OTA_SLOT_BYTES = 4 * 1024 * 1024  # 4 MiB OTA partition (4194304)

WATCH_STYLES: tuple[str, ...] = (
    "jarvis",
    "diver",
    "dress",
    "pilot",
    "minimal",
    "future",
    "skeleton",
    "celestial",
    "orrery",
    "reactor",
)
CORE_SCREENS: tuple[str, ...] = ("home", "watch", "weather", "status", "activity", "desk")
SPACE_INDEX_TO_SCREEN: dict[int, str] = {
    0: "home",
    1: "watch",
    2: "weather",
    3: "status",
    4: "desk",
    5: "activity",
}
LAYER_NAMES: tuple[str, ...] = ("dial", "canvas", "hud", "shell", "surface")
CHARGE_DIRECTIONS: dict[int, str] = {0: "STANDBY", 1: "CHARGING", 2: "DISCHARGING"}
CHARGE_PHASES: dict[int, str] = {
    0: "TRICKLE",
    1: "PRECHARGE",
    2: "CC",
    3: "CV",
    4: "DONE",
    5: "NOT_CHARGING",
}

_RGB565_LUT: list[bytes] | None = None
_GLASS_ALLOWED_RE = re.compile(r"[^A-Za-z0-9\-_.:/?!+,'()&% ]+")
_AGENT_DISALLOWED_RE = re.compile(r"[\x00-\x1f\x7f<>\"\\]+")
_UNICODE_PUNCT = str.maketrans({
    "\u2014": "-",
    "\u2013": "-",
    "\u2018": "'",
    "\u2019": "'",
    "\u201c": "'",
    "\u201d": "'",
    "\u2026": "...",
    "\u00b7": "-",
    "\u2022": "-",
})


def ensure_utf8_console() -> None:
    """Reconfigure Windows console streams to UTF-8 so Unicode symbols never crash."""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except Exception:  # noqa: BLE001
                pass


def clear_terminal() -> None:
    """Cross-platform terminal clear without calling POSIX-only `clear`."""
    if os.name == "nt" and not (
        os.environ.get("WT_SESSION")
        or os.environ.get("TERM")
        or os.environ.get("ANSICON")
    ):
        os.system("cls")
    else:
        sys.stdout.write("\x1b[2J\x1b[H")
        sys.stdout.flush()


def _extract_host_flag(argv: Sequence[str]) -> tuple[str | None, list[str]]:
    """Strip --host VAL (or --host=VAL) appearing before any `--` separator."""
    found: str | None = None
    out: list[str] = []
    i = 0
    n = len(argv)
    while i < n:
        arg = argv[i]
        if arg == "--":
            out.extend(argv[i:])
            break
        if arg == "--host" and i + 1 < n:
            found = argv[i + 1]
            i += 2
            continue
        if arg.startswith("--host="):
            found = arg.split("=", 1)[1]
            i += 1
            continue
        out.append(arg)
        i += 1
    return found, out


def host() -> str:
    value, _ = _extract_host_flag(sys.argv[1:])
    value = value or os.environ.get("JARVIS_DEVICE_HOST")
    if not value:
        # One line, exit 2 — an operator error, not a traceback.
        print("jarvisctl: --host or JARVIS_DEVICE_HOST required", file=sys.stderr)
        raise SystemExit(2)
    return value


def normalize_host(value: str) -> str:
    value = value.strip().rstrip("/")
    return value if value.startswith(("http://", "https://")) else "http://" + value


def base_url() -> str:
    return normalize_host(host())


def valid_pairing_token(token: str) -> bool:
    encoded = token.encode("utf-8")
    return (32 <= len(encoded) <= 64) and not any(
        byte <= 0x20 or byte == 0x7F for byte in encoded
    )


def load_pairing_token(target_host: str) -> str | None:
    """Load target_host's pairing token without exposing it in argv or output.

    Checks macOS Keychain, Windows HKCU Registry (`Software\\JarvisNano\\Desk`),
    and host-bound environment variables (`JARVIS_PAIRING_TOKEN` or
    `JARVIS_DESK_TOKEN`, only when `JARVIS_DEVICE_HOST` matches `target_host`).
    A token bound to one Nano never rides along to another because --host changed.
    """
    if not target_host or not target_host.strip():
        return None
    base = normalize_host(target_host)
    account = "jarvis-desk@" + urllib.parse.urlsplit(base).netloc
    try:
        result = subprocess.run(
            ["security", "find-generic-password", "-a", account,
             "-s", KEYCHAIN_SERVICE, "-w"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except (FileNotFoundError, OSError):
        result = None
    if result is not None and result.returncode == 0:
        token = result.stdout.rstrip("\r\n")
        return token if valid_pairing_token(token) else None

    if sys.platform == "win32":
        # jarvis-desk.py pair stores Windows tokens here, keyed by account.
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                                r"Software\JarvisNano\Desk") as key:
                token, kind = winreg.QueryValueEx(key, account)
            if kind == winreg.REG_SZ and valid_pairing_token(token):
                return token
        except OSError:
            pass

    token = (os.environ.get("JARVIS_PAIRING_TOKEN")
             or os.environ.get("JARVIS_DESK_TOKEN") or "")
    bound_host = os.environ.get("JARVIS_DEVICE_HOST", "").strip()
    if not token or not bound_host:
        return None
    if normalize_host(bound_host) != base or not valid_pairing_token(token):
        return None
    return token


def pairing_token() -> str | None:
    """Load the active CLI host's pairing token."""
    return load_pairing_token(host())


def _get_rgb565_lut() -> list[bytes]:
    global _RGB565_LUT
    if _RGB565_LUT is None:
        lut: list[bytes] = []
        for v in range(65536):
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            lut.append(bytes((
                (r << 3) | (r >> 2),
                (g << 2) | (g >> 4),
                (b << 3) | (b >> 2),
            )))
        _RGB565_LUT = lut
    return _RGB565_LUT


def rgb565_to_pil(
    data: bytes,
    width: int = 466,
    height: int = 466,
    *,
    big_endian: bool = True,
):
    """Decode raw RGB565 frame bytes (default CO5300 big-endian) to a Pillow RGB Image."""
    from PIL import Image

    expected = width * height * 2
    if width <= 0 or height <= 0 or len(data) != expected:
        raise ValueError(
            f"expected {expected} bytes for {width}x{height} RGB565, got {len(data)}"
        )
    lut = _get_rgb565_lut()
    fmt = f"{'>' if big_endian else '<'}{width * height}H"
    rgb = b"".join(lut[px] for px in struct.unpack(fmt, data))
    return Image.frombytes("RGB", (width, height), rgb)


def api(path: str, method: str = "GET", body: bytes | None = None,
        timeout: int = 10):
    req = urllib.request.Request(base_url() + path, data=body, method=method)
    req.add_header("X-JarvisNano-Control", "1")
    token = pairing_token()
    if token:
        req.add_header("X-JarvisNano-Token", token)
    if body is not None:
        req.add_header("Content-Type", "application/octet-stream")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def api_with_headers(
    path: str,
    method: str = "GET",
    body: bytes | None = None,
    timeout: float = 10.0,
    content_type: str | None = None,
) -> tuple[int, dict[str, str], bytes]:
    """HTTP helper returning (status, headers_dict, raw_bytes)."""
    req = urllib.request.Request(base_url() + path, data=body, method=method)
    req.add_header("X-JarvisNano-Control", "1")
    token = pairing_token()
    if token:
        req.add_header("X-JarvisNano-Token", token)
    if body is not None:
        req.add_header("Content-Type", content_type or "application/octet-stream")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        status = int(getattr(r, "status", 200))
        hdrs = dict(r.headers.items()) if hasattr(r, "headers") and r.headers else {}
        return status, hdrs, r.read()


def post_json(path: str, payload: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    _, _, resp = api_with_headers(
        path, method="POST", body=raw, timeout=timeout, content_type="application/json"
    )
    return json.loads(resp.decode("utf-8")) if resp else {}


def get_json(path: str, timeout: int = 10) -> dict[str, Any]:
    return json.loads(api(path, timeout=timeout))


def _header_ci(headers: dict[str, str], name: str) -> str | None:
    needle = name.lower()
    for k, v in headers.items():
        if k.lower() == needle:
            return v
    return None


def _http_error_detail(exc: urllib.error.HTTPError) -> str:
    """Read and sanitize a short error reason from an HTTPError body."""
    try:
        raw = exc.read(4096)
    except Exception:  # noqa: BLE001
        raw = b""
    text = raw.decode("utf-8", "replace").strip() if raw else ""
    token = None
    try:
        token = pairing_token()
    except SystemExit:
        token = None
    if text:
        try:
            obj = json.loads(text)
            if isinstance(obj, dict):
                text = str(obj.get("error") or obj.get("reason") or obj.get("message") or text)
        except json.JSONDecodeError:
            pass
    if token and text:
        text = text.replace(token, "[redacted]")
    text = " ".join(text.split())
    return text[:240]


def sanitize_glass_text(text: str, max_len: int, *, fallback: str = "-") -> str:
    """Sanitize and truncate text to firmware brain_render_text_safe() ASCII rules."""
    cleaned = str(text or "").translate(_UNICODE_PUNCT)
    cleaned = re.sub(r"https?://\S*", "", cleaned, flags=re.IGNORECASE)
    cleaned = re.sub(r"<script\b[^>]*>", "", cleaned, flags=re.IGNORECASE)
    cleaned = _GLASS_ALLOWED_RE.sub(" ", cleaned)
    cleaned = " ".join(cleaned.split()).strip()
    if len(cleaned) > max_len:
        cleaned = cleaned[:max_len].rstrip()
    if not cleaned:
        cleaned = fallback[:max_len]
    return cleaned


def sanitize_agent_text(text: str, max_len: int, *, allow_space: bool = True,
                        fallback: str = "-") -> str:
    """Sanitize text to firmware agent_text_safe() rules."""
    cleaned = str(text or "").translate(_UNICODE_PUNCT)
    cleaned = re.sub(r"https?://\S*", "", cleaned, flags=re.IGNORECASE)
    cleaned = re.sub(r"<script\b", "", cleaned, flags=re.IGNORECASE)
    cleaned = _AGENT_DISALLOWED_RE.sub(" ", cleaned)
    cleaned = cleaned.encode("ascii", "ignore").decode("ascii")
    if allow_space:
        cleaned = " ".join(cleaned.split()).strip()
    else:
        cleaned = re.sub(r"\s+", "-", cleaned.strip())
    if len(cleaned) > max_len:
        cleaned = cleaned[:max_len].rstrip(" -")
    if not cleaned:
        cleaned = fallback[:max_len]
    return cleaned


def sanitize_identifier(text: str, max_len: int, *, fallback: str = "act") -> str:
    """Produce a firmware-valid identifier [A-Za-z0-9][A-Za-z0-9._:-]*."""
    slug = re.sub(r"[^A-Za-z0-9._:-]+", "-", str(text or "").strip().lower()).strip("-._:")
    if not slug or not slug[0].isalnum():
        slug = fallback + ("-" + slug if slug else "")
    return slug[:max_len]


def _stable_session_id() -> str:
    seed = f"{getpass.getuser()}|{urllib.parse.urlsplit(base_url()).netloc}"
    digest = hashlib.sha256(seed.encode("utf-8")).hexdigest()[:24]
    return f"desk-{digest}"


def _brain_post_with_retry(envelope: dict[str, Any]) -> dict[str, Any]:
    """POST a brain surface envelope with one automatic 409 sequence retry."""
    for attempt in range(2):
        try:
            return post_json("/api/brain/inbox", envelope, timeout=10.0)
        except urllib.error.HTTPError as exc:
            if attempt != 0 or exc.code != 409:
                raise
            outbox = get_json("/api/brain/outbox?after=0")
            envelope["seq"] = int(outbox.get("next_inbox_seq", envelope["seq"] + 1))
    raise RuntimeError("brain inbox sequence conflict")


def _brain_dismiss_quiet(session: str, surface_id: str) -> None:
    """Best-effort dismiss of an active brain surface (swallows already-gone errors)."""
    try:
        outbox = get_json("/api/brain/outbox?after=0", timeout=5)
        seq = int(outbox.get("next_inbox_seq", 1))
        envelope = {
            "v": 1,
            "type": "surface.dismiss",
            "seq": seq,
            "session": session,
            "id": surface_id,
            "ttl_ms": 1000,
            "payload": {},
        }
        _brain_post_with_retry(envelope)
    except Exception:  # noqa: BLE001
        pass


_AGENT_STATE_MAP: dict[str, str] = {
    "working": "working",
    "verifying": "verifying",
    "waiting": "waiting",
    "succeeded": "succeeded",
    "failed": "failed",
    # Codex Mode 2.0 CLI aliases -> native firmware agent_state_valid() values
    "complete": "succeeded",
    "blocked": "failed",
    "idle": "succeeded",
}


def agent_link_post(
    *,
    task_id: str,
    state: str,
    progress: int,
    title: str,
    summary: str,
    evidence: Sequence[dict[str, str]] | None = None,
    ttl_s: int = 900,
) -> dict[str, Any]:
    """Post a bounded Agent Link update, automatically syncing next_revision."""
    fw_state = _AGENT_STATE_MAP.get(state.lower().strip())
    if not fw_state:
        raise ValueError(f"unsupported agent state: {state}")
    cur = get_json("/api/agent/link")
    next_rev = int(cur.get("next_revision", 1))
    clamped_prog = max(0, min(100, int(progress)))
    clamped_ttl = max(30, min(3600, int(ttl_s)))

    clean_ev: list[dict[str, str]] = []
    for item in (evidence or [])[:4]:
        ev_state = str(item.get("state") or item.get("status") or "wait").lower()
        if ev_state not in ("pass", "working", "wait", "fail"):
            ev_state = "wait"
        clean_ev.append({
            "label": sanitize_agent_text(str(item.get("label", "step")), 24, fallback="step"),
            "state": ev_state,
        })

    payload: dict[str, Any] = {
        "task_id": sanitize_identifier(task_id, 48, fallback="jarvisctl"),
        "revision": next_rev,
        "state": fw_state,
        "progress": clamped_prog,
        "title": sanitize_agent_text(title, 48, fallback="Task"),
        "summary": sanitize_agent_text(summary, 120, fallback="Running"),
        "ttl_s": clamped_ttl,
    }
    if clean_ev:
        payload["evidence"] = clean_ev

    for attempt in range(2):
        try:
            return post_json("/api/agent/link", payload, timeout=10.0)
        except urllib.error.HTTPError as exc:
            if attempt != 0 or exc.code != 409:
                raise
            refreshed = get_json("/api/agent/link")
            payload["revision"] = int(refreshed.get("next_revision", payload["revision"] + 1))
    raise RuntimeError("agent link revision conflict")


def _find_git_bash() -> str | None:
    """Locate Git Bash on Windows (avoiding WSL stub) or bash on POSIX."""
    if sys.platform == "win32":
        for cand in (
            r"C:\Program Files\Git\bin\bash.exe",
            r"C:\Program Files\Git\usr\bin\bash.exe",
            r"C:\Program Files (x86)\Git\bin\bash.exe",
        ):
            if os.path.isfile(cand):
                return cand
    found = shutil.which("bash")
    return found


def _local_elf_sha16(elf_path: str) -> str | None:
    if not os.path.isfile(elf_path):
        return None
    h = hashlib.sha256()
    with open(elf_path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def _fmt_duration_min(minutes: Any) -> str:
    if minutes is None or isinstance(minutes, bool):
        return "-"
    try:
        m = int(round(float(minutes)))
    except (ValueError, TypeError):
        return "-"
    if m < 0:
        return "-"
    if m < 60:
        return f"{m}m"
    return f"{m // 60}h {m % 60:02d}m"


def _fmt_duration_sec(seconds: Any) -> str:
    if seconds is None or isinstance(seconds, bool):
        return "-"
    try:
        s = int(round(float(seconds)))
    except (ValueError, TypeError):
        return "-"
    if s < 0:
        return "-"
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m {s % 60:02d}s"
    return f"{s // 3600}h {(s % 3600) // 60:02d}m"


# ---------------------------------------------------------------------------
# Existing commands (preserved 100% compatible)
# ---------------------------------------------------------------------------

def cmd_status() -> int:
    g = get_json("/api/gemini/live")
    d = get_json("/api/display")
    problems = []
    if g.get("privacy_paused"):
        problems.append("privacy-muted")
    if g.get("dac_muted"):
        problems.append("dac-muted")
    if g.get("phase") in ("Idle", "Backoff", "Fatal"):
        problems.append(f"voice off ({g.get('last_reason')})")
    largest = g.get("largest_internal_block")
    if isinstance(largest, (int, float)) and largest < 8192:
        problems.append(f"internal fragmented (largest={int(largest)}B)")
    if d.get("flush_errors", 0):
        problems.append(f"display flush errors={d['flush_errors']}")
    try:
        h = get_json("/api/device/health")
        pb, rx = h.get("playback", {}), h.get("rx", {})
        if pb.get("underruns"):
            problems.append(f"playback underruns={pb['underruns']} "
                            f"(max hole {pb.get('max_gap_ms')} ms)")
        if rx.get("drops"):
            problems.append(f"rx frames dropped={rx['drops']}")
        audio = (f"  playback: underruns={pb.get('underruns')} "
                 f"max_gap={pb.get('max_gap_ms')}ms "
                 f"low_water={pb.get('low_water_ms')}ms "
                 f"rx_gap={rx.get('max_gap_ms')}ms rx_drops={rx.get('drops')}")
    except Exception:  # noqa: BLE001 - an older image has no such route
        audio = "  playback: (no /api/device/health playback counters)"
    verdict = "HEALTHY" if not problems else "ATTENTION: " + ", ".join(problems)
    print(f"{verdict}\n"
          f"  phase={g.get('phase')} mic_rms={g.get('mic_rms')} "
          f"deaths={g.get('deaths')}\n"
          f"  internal={g.get('free_internal_heap')}B "
          f"largest={g.get('largest_internal_block')}B "
          f"psram={g.get('free_psram')}B "
          f"display={d.get('actual_fps')}fps\n"
          f"{audio}")
    return 0 if not problems else 1


def cmd_listen() -> int:
    api("/api/voice/control?armed=1", "POST", b"")
    print("armed")
    return 0


def cmd_mute() -> int:
    api("/api/voice/control?armed=0", "POST", b"")
    print("disarmed")
    return 0


def cmd_say(text: str) -> int:
    r = subprocess.run([sys.executable, os.path.join(HERE, "live-device.py"),
                        "gemini-cycle", "--host", host(), "--text", text,
                        "--report"], capture_output=True, text=True)
    ok = r.stdout.count("OK ")
    print(f"turn complete ({ok} checks passed)")
    api("/api/voice/control?armed=1", "POST", b"")   # leave it listening
    return 0 if ok >= 4 else 1


def cmd_demo() -> int:
    print(api("/api/demo", "POST", b"").decode())
    return 0


def ppm_to_png(ppm: str, out: str) -> bool:
    """PIL first — it is on every platform we run this from. `sips` is the
    macOS-only fallback and is consulted only when it exists."""
    try:
        from PIL import Image
    except ImportError:
        Image = None
    if Image is not None:
        Image.open(ppm).save(out)
        return True
    if shutil.which("sips"):
        return subprocess.run(["sips", "-s", "format", "png", ppm, "--out", out],
                              capture_output=True).returncode == 0
    return False


def cmd_screen(out: str = "glass.png") -> int:
    ppm = out + ".ppm"
    with open(ppm, "wb") as f:
        f.write(api("/api/display/snapshot.ppm", timeout=20))
    try:
        if not ppm_to_png(ppm, out):
            print(f"{ppm} kept — no PNG converter (pip install pillow)",
                  file=sys.stderr)
            return 1
    finally:
        if os.path.exists(out):
            os.unlink(ppm)
    print(out)
    return 0


def cmd_canvas(argv: list[str]) -> int:
    return subprocess.call([sys.executable,
                            os.path.join(HERE, "send-canvas.py"),
                            *argv, "--host", host()])


def cmd_tune(argv: list[str]) -> int:
    q = "&".join(a for a in argv if "=" in a)
    print(api(f"/api/debug/gain?{q}", "POST", b"").decode())
    return 0


def cmd_taps(outdir: str = "taps-out") -> int:
    return subprocess.call([sys.executable,
                            os.path.join(HERE, "live-device.py"),
                            "audio-taps", "--host", host(), "--output", outdir])


def cmd_vadlog(out: str = "vadlog.csv") -> int:
    with open(out, "wb") as f:
        f.write(api("/api/diag/vadlog", timeout=20))
    print(out)
    return 0


def cmd_voiceperf(argv: list[str]) -> int:
    if argv and argv[0] == "bench":
        api("/api/diag/voiceperf?bench=1", "POST", b"")
        time.sleep(4)
    print(json.dumps(get_json("/api/diag/voiceperf"), indent=2))
    return 0


def _locate_elf_for_sha(elf_sha: str, explicit_elf: str | None = None) -> tuple[str | None, bool]:
    """Return (elf_path, sha_matches)."""
    sha16 = (elf_sha or "").lower()[:16]
    if explicit_elf:
        if not os.path.isfile(explicit_elf):
            return None, False
        actual = _local_elf_sha16(explicit_elf)
        return explicit_elf, (not sha16 or actual == sha16)

    archive_dir = os.path.join(REPO_ROOT, ".build_logs", "elfs")
    if sha16 and os.path.isdir(archive_dir):
        matches = sorted(glob.glob(os.path.join(archive_dir, f"{sha16}*.elf")))
        if matches:
            return matches[-1], True
        idx_path = os.path.join(archive_dir, "index.json")
        if os.path.isfile(idx_path):
            try:
                with open(idx_path, encoding="utf-8") as fh:
                    idx = json.loads(fh.read())
                entries = idx if isinstance(idx, list) else idx.get("elfs", [])
                for entry in entries:
                    if str(entry.get("sha16", "")).lower() == sha16:
                        cand = os.path.join(archive_dir, str(entry.get("file", "")))
                        if os.path.isfile(cand):
                            return cand, True
            except Exception:  # noqa: BLE001
                pass

    build_elf = os.path.join(REPO_ROOT, "build", "jarvisrobot_v5.elf")
    if os.path.isfile(build_elf):
        actual = _local_elf_sha16(build_elf)
        return build_elf, (not sha16 or actual == sha16)
    return None, False


def _find_addr2line() -> str | None:
    for name in ("xtensa-esp32s3-elf-addr2line", "xtensa-esp32s3-elf-addr2line.exe"):
        found = shutil.which(name)
        if found:
            return found
    roots = [
        os.environ.get("IDF_TOOLS_PATH", ""),
        os.path.expanduser("~/.espressif"),
    ]
    for root in roots:
        if not root or not os.path.isdir(root):
            continue
        pattern = os.path.join(
            root, "tools", "xtensa-esp-elf", "*", "xtensa-esp-elf", "bin",
            "xtensa-esp32s3-elf-addr2line*",
        )
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[-1]
    return None


def _run_addr2line(elf_path: str, addrs: list[str]) -> tuple[int, str, str]:
    tool = _find_addr2line()
    if tool:
        proc = subprocess.run(
            [tool, "-pfiaC", "-e", elf_path, *addrs],
            capture_output=True, text=True, check=False,
        )
        return proc.returncode, proc.stdout, proc.stderr
    docker = shutil.which("docker")
    if docker:
        elf_abs = os.path.abspath(elf_path)
        elf_dir = os.path.dirname(elf_abs)
        elf_name = os.path.basename(elf_abs)
        image = os.environ.get("IDF_DOCKER_IMAGE", "espressif/idf:v5.5.4")
        env = dict(os.environ, MSYS_NO_PATHCONV="1")
        proc = subprocess.run(
            [
                docker, "run", "--rm",
                "-v", f"{elf_dir}:/elf:ro",
                image,
                "xtensa-esp32s3-elf-addr2line", "-pfiaC", "-e", f"/elf/{elf_name}",
                *addrs,
            ],
            capture_output=True, text=True, env=env, check=False,
        )
        return proc.returncode, proc.stdout, proc.stderr
    return 127, "", "xtensa-esp32s3-elf-addr2line not found in PATH, ~/.espressif, or Docker"


def cmd_crash(argv: list[str]) -> int:
    """Reset reason + stored coredump summary; dump, clear, or decode backtrace."""
    if argv and argv[0] in ("-h", "--help"):
        print("usage: jarvisctl crash [dump [out.bin] | clear | decode [--elf PATH]]")
        return 0
    sub = argv[0] if argv else ""
    if sub == "dump":
        out = argv[1] if len(argv) > 1 else "coredump.bin"
        with open(out, "wb") as f:
            f.write(api("/api/diag/coredump.bin", timeout=30))
        print(out)
        return 0
    if sub == "clear":
        print(api("/api/diag/crash?clear=1", method="POST", body=b"").decode())
        return 0
    if sub == "decode":
        ap = argparse.ArgumentParser(
            prog="jarvisctl crash decode",
            description="Decode stored crash backtrace against matching firmware ELF.",
        )
        ap.add_argument("--elf", default=None, help="explicit path to jarvisrobot_v5.elf")
        parsed = ap.parse_args(argv[1:])
        data = get_json("/api/diag/crash")
        summary = data.get("summary")
        if not isinstance(summary, dict):
            print(f"No stored crash summary on device (reset={data.get('reset')}, "
                  f"dump_present={data.get('dump_present')}).")
            return 0
        elf_sha = str(summary.get("elf_sha") or "")
        exc_pc = str(summary.get("exc_pc") or "")
        bt = summary.get("bt") or []
        raw_addrs = [exc_pc, *[str(a) for a in bt]]
        addrs = [
            a for a in raw_addrs
            if re.fullmatch(r"0x[0-9a-fA-F]+", a) and int(a, 16) != 0
        ]
        print(f"Crash Summary (reset={data.get('reset')}):")
        print(f"  task:   {summary.get('task')}  cause={summary.get('exc_cause')}  "
              f"pc={exc_pc}  vaddr={summary.get('exc_vaddr')}")
        if summary.get("panic_reason"):
            print(f"  reason: {summary.get('panic_reason')}")
        print(f"  elf_sha: {elf_sha or '(unknown)'}  bt_depth={summary.get('bt_depth')}  "
              f"corrupted={summary.get('bt_corrupted')}")
        if not addrs:
            print("  no non-zero PC/backtrace addresses to decode.")
            return 0
        elf_path, matched = _locate_elf_for_sha(elf_sha, parsed.elf)
        if not elf_path:
            print(f"No matching ELF found for sha {elf_sha[:16]} "
                  f"(checked .build_logs/elfs/ and build/jarvisrobot_v5.elf; pass --elf PATH).",
                  file=sys.stderr)
            print("  raw addresses: " + " ".join(addrs))
            return 1
        match_note = "SHA MATCH" if matched else "WARNING: ELF SHA MISMATCH"
        print(f"  elf:    {elf_path} [{match_note}]")
        rc, out, err = _run_addr2line(elf_path, addrs)
        if rc != 0:
            print(f"addr2line failed (exit {rc}): {err.strip()}", file=sys.stderr)
            print("  raw addresses: " + " ".join(addrs))
            return 1
        print("\nDecoded Backtrace:")
        for idx, line in enumerate(out.strip().splitlines()):
            tag = "PC " if idx == 0 and exc_pc in addrs else f"#{idx:<2}"
            print(f"  {tag} {line}")
        return 0
    if sub:
        print("usage: jarvisctl crash [dump [out.bin] | clear | decode [--elf PATH]]",
              file=sys.stderr)
        return 2
    print(json.dumps(get_json("/api/diag/crash"), indent=2))
    return 0


def cmd_gestures(limit_arg: str = "40") -> int:
    try:
        limit = max(1, min(200, int(limit_arg)))
    except ValueError:
        print("gesture line count must be an integer", file=sys.stderr)
        return 2
    log = api("/api/logs?tail=131072", timeout=20).decode("utf-8", "replace")
    markers = (
        "jr_hal_touch:",
        "jarvis_v5: gesture:",
        "jarvis_v5: ui:",
        "jarvis_v5: ask:",
        "jarvis_v5: operator:",
    )
    events = [line for line in log.splitlines()
              if any(marker in line for marker in markers)]
    if not events:
        print("no gesture events remain in the device log ring")
        return 1
    print("\n".join(events[-limit:]))
    return 0


def cmd_desk(argv: list[str]) -> int:
    if not argv:
        print("desk requires present|dismiss|events|status|doctor|levels",
              file=sys.stderr)
        return 2
    return subprocess.call([
        sys.executable,
        os.path.join(HERE, "jarvis-desk.py"),
        "--host",
        host(),
        *argv,
    ])


def cmd_update() -> int:
    try:
        if cmd_desk(["takeover", "--ttl", "180"]) != 0:
            raise RuntimeError("paired takeover failed")
        print("Codex mode claimed — glass announces the update")
    except Exception as exc:  # noqa: BLE001
        print(f"takeover skipped ({exc}) — flashing regardless")
    time.sleep(1.5)
    env = dict(os.environ, NO_BUILD="1")
    bash = _find_git_bash() or "bash"
    return subprocess.call([bash, os.path.join(HERE, "flash-v5.sh")], env=env)


def esptool_python() -> str:
    venv = os.path.join(HERE, "..", ".build_tools", "esptool")
    for candidate in (os.path.join(venv, "bin", "python"),
                      os.path.join(venv, "Scripts", "python.exe")):
        if os.path.exists(candidate):
            return candidate
    return sys.executable


def cmd_reboot() -> int:
    return subprocess.call([esptool_python(), "-m", "esptool", "--after",
                            "watchdog-reset", "flash-id"],
                           stdout=subprocess.DEVNULL)


# ---------------------------------------------------------------------------
# Codex Mode 2.0 commands
# ---------------------------------------------------------------------------

def _parse_ask_choices(choices_str: str | None, consent: bool) -> list[dict[str, str]]:
    if consent:
        if choices_str:
            raw_items = [c.strip() for c in choices_str.split(",") if c.strip()][:2]
        else:
            raw_items = ["deny=Deny", "allow=Allow"]
        if len(raw_items) < 2:
            raw_items = ["deny=Deny", "allow=Allow"]
    else:
        if choices_str:
            raw_items = [c.strip() for c in choices_str.split(",") if c.strip()][:3]
            if len(raw_items) == 1:
                raw_items.append("Cancel")
        else:
            raw_items = ["Yes", "No"]

    actions: list[dict[str, str]] = []
    used_ids: set[str] = set()
    for idx, item in enumerate(raw_items):
        if "=" in item:
            raw_id, raw_lbl = item.split("=", 1)
        else:
            raw_id, raw_lbl = item, item
        label = sanitize_glass_text(raw_lbl, 12, fallback=f"Opt{idx + 1}")
        base_id = sanitize_identifier(raw_id, 14, fallback=f"opt{idx + 1}")
        act_id = base_id
        suffix = 2
        while act_id in used_ids:
            act_id = f"{base_id[:13]}{suffix}"
            suffix += 1
        used_ids.add(act_id)
        actions.append({"id": act_id, "label": label})
    return actions


def cmd_ask(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl ask",
        description="Present a choice or consent card on glass and optionally wait for tap.",
    )
    ap.add_argument("title", help="card title (auto-sanitized & truncated to <=24 safe ASCII)")
    ap.add_argument("--body", default="", help="card body (<=48 safe ASCII)")
    ap.add_argument("--choices", default=None,
                    help="comma-separated choices (up to 3; or 2 for --consent, e.g. 'Deny,Allow')")
    ap.add_argument("--consent", action="store_true",
                    help="present as 2-choice consent card (default Deny,Allow; exits 1 on deny)")
    ap.add_argument("--ttl", type=int, default=120, help="card lifetime in seconds (1..600)")
    ap.add_argument("--wait", action="store_true",
                    help="poll /api/brain/outbox every 0.4s until tapped or TTL timeout (exit 124)")
    ap.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = ap.parse_args(argv)

    ttl_s = max(1, min(600, args.ttl))
    kind = "consent" if args.consent else "choice"
    title = sanitize_glass_text(args.title, 24, fallback="Question")
    default_body = "Tap glass to confirm" if args.consent else "Tap glass to answer"
    body = sanitize_glass_text(args.body or default_body, 48, fallback=default_body)
    actions = _parse_ask_choices(args.choices, args.consent)

    # Snapshot cursor BEFORE presenting so we never miss a fast tap.
    outbox = get_json("/api/brain/outbox?after=0")
    cursor = int(outbox.get("next_after", 0))
    seq = int(outbox.get("next_inbox_seq", 1))
    session = _stable_session_id()
    surface_id = sanitize_identifier(f"ask-{int(time.time())}-{os.getpid() % 10000}", 32)

    envelope = {
        "v": 1,
        "type": "surface.present",
        "seq": seq,
        "session": session,
        "id": surface_id,
        "ttl_ms": ttl_s * 1000,
        "payload": {
            "kind": kind,
            "title": title,
            "body": body,
            "actions": actions,
        },
    }
    _brain_post_with_retry(envelope)

    if not args.wait:
        if args.json:
            print(json.dumps({
                "ok": True, "id": surface_id, "kind": kind,
                "title": title, "body": body, "cursor": cursor,
                "actions": actions, "ttl_s": ttl_s,
            }))
        else:
            opts = " | ".join(a["label"] for a in actions)
            print(f"presented {kind} {surface_id}: {title} [{opts}]")
        return 0

    label_by_id = {a["id"]: a["label"] for a in actions}
    deadline = time.monotonic() + ttl_s
    try:
        while time.monotonic() < deadline:
            time.sleep(0.4)
            try:
                poll = get_json(f"/api/brain/outbox?after={cursor}", timeout=8)
            except (urllib.error.URLError, OSError):
                continue
            next_after = int(poll.get("next_after", cursor))
            for ev in poll.get("events", []):
                if (
                    isinstance(ev, dict)
                    and ev.get("type") == "surface.action"
                    and ev.get("id") == surface_id
                    and ev.get("session") == session
                ):
                    payload = ev.get("payload") or {}
                    act_id = str(payload.get("action_id", ""))
                    label = label_by_id.get(act_id, act_id)
                    denied = args.consent and (
                        act_id.lower() == "deny" or act_id == actions[0]["id"]
                    )
                    if args.json:
                        print(json.dumps({
                            "ok": not denied,
                            "id": surface_id,
                            "kind": kind,
                            "action_id": act_id,
                            "label": label,
                            "seq": ev.get("seq"),
                            "ts_ms": payload.get("ts_ms"),
                        }))
                    else:
                        print(f"{act_id}: {label}")
                    return 1 if denied else 0
            cursor = next_after
    except KeyboardInterrupt:
        _brain_dismiss_quiet(session, surface_id)
        return 130

    _brain_dismiss_quiet(session, surface_id)
    if args.json:
        print(json.dumps({"ok": False, "timeout": True, "id": surface_id}))
    else:
        print(f"ask timed out after {ttl_s}s (dismissed)", file=sys.stderr)
    return 124


_STEP_RATIO_RE = re.compile(r"(?:\[|\()(\d+)\s*/\s*(\d+)(?:\]|\))\s*(.*)")
_STEP_PCT_RE = re.compile(r"\b(\d{1,3})%")
_STEP_MARKER_RE = re.compile(
    r"^(?:==+>|==+|Step\s+\d+[:.)]|\[[A-Za-z0-9_-]+\])\s*(.+)", re.IGNORECASE
)
_ERROR_LINE_RE = re.compile(
    r"\b(?:error|failed|fail|fatal|exception|traceback)\b|^E\s*\(", re.IGNORECASE
)


def _update_run_evidence(
    evidence: list[dict[str, str]],
    label: str,
    state: str,
    max_items: int = 3,
) -> None:
    clean = sanitize_agent_text(label, 24, fallback="step")
    if state == "working":
        for item in evidence:
            if item["state"] == "working":
                item["state"] = "pass"
    for item in evidence:
        if item["label"].lower() == clean.lower():
            item["state"] = state
            return
    evidence.append({"label": clean, "state": state})
    if len(evidence) > max_items:
        del evidence[:-max_items]


def cmd_run(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl run",
        description="Run a local command while streaming live Agent Link progress & result to glass.",
    )
    ap.add_argument("--title", default=None, help="short display title on Desk screen")
    ap.add_argument("--takeover", action="store_true",
                    help="acquire Codex operator lease with 45s keepalive during execution")
    ap.add_argument("--no-result", action="store_true",
                    help="skip presenting final PASS/FAIL RESULT card on glass")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="command to execute (prefix with -- to separate flags)")
    args = ap.parse_args(argv)

    cmd = list(args.command)
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]
    if not cmd:
        ap.error("command required (example: jarvisctl run --title Build -- ./scripts/host-tests.sh)")

    title = args.title or os.path.basename(cmd[0]) or "Run"
    task_id = sanitize_identifier(f"run-{title}-{int(time.time()) % 10000}", 48, fallback="run")

    # Best-effort switch to Desk screen
    try:
        api("/api/glass?screen=desk", "POST", b"", timeout=5)
    except Exception:  # noqa: BLE001
        pass

    keepalive_stop = threading.Event()
    keepalive_thread: threading.Thread | None = None
    leased = False

    def _keepalive_loop() -> None:
        while not keepalive_stop.wait(45.0):
            try:
                api("/api/operator/lease?ttl=120&mode=codex", "POST", b"", timeout=5)
            except Exception:  # noqa: BLE001
                pass

    if args.takeover:
        try:
            api("/api/operator/lease?ttl=120&mode=codex", "POST", b"", timeout=8)
            leased = True
            keepalive_thread = threading.Thread(target=_keepalive_loop, daemon=True)
            keepalive_thread.start()
        except Exception as exc:  # noqa: BLE001
            print(f"jarvisctl run: takeover warning: {exc}", file=sys.stderr)

    evidence: list[dict[str, str]] = [{"label": sanitize_agent_text(title, 24, fallback="Start"), "state": "working"}]
    progress = 5
    last_push = 0.0
    last_line = ""
    last_error = ""
    t0 = time.monotonic()

    def _push_link(state: str, prog: int, summary: str, force: bool = False) -> None:
        nonlocal last_push
        now = time.monotonic()
        if not force and (now - last_push) < 1.0:
            return
        last_push = now
        try:
            agent_link_post(
                task_id=task_id,
                state=state,
                progress=prog,
                title=title,
                summary=summary,
                evidence=evidence,
                ttl_s=900,
            )
        except Exception:  # noqa: BLE001
            pass

    _push_link("working", progress, f"Running {os.path.basename(cmd[0])}", force=True)

    rc = 1
    proc: subprocess.Popen[str] | None = None
    try:
        with subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        ) as proc:
            assert proc.stdout is not None
            for raw_line in proc.stdout:
                sys.stdout.write(raw_line)
                sys.stdout.flush()
                stripped = raw_line.strip()
                if not stripped:
                    continue
                last_line = stripped
                if _ERROR_LINE_RE.search(stripped):
                    last_error = stripped

                changed = False
                m_ratio = _STEP_RATIO_RE.search(stripped)
                if m_ratio:
                    cur_n, tot_n = int(m_ratio.group(1)), int(m_ratio.group(2))
                    if tot_n > 0 and cur_n <= tot_n:
                        progress = max(1, min(99, int(round(100.0 * cur_n / tot_n))))
                        step_lbl = (m_ratio.group(3) or f"Step {cur_n}/{tot_n}").strip()
                        _update_run_evidence(evidence, step_lbl, "working")
                        changed = True
                else:
                    m_mark = _STEP_MARKER_RE.match(stripped)
                    if m_mark:
                        lbl = m_mark.group(1).strip()
                        st = "fail" if _ERROR_LINE_RE.search(lbl) else "working"
                        _update_run_evidence(evidence, lbl, st)
                        progress = min(95, progress + 10)
                        changed = True
                    else:
                        m_pct = _STEP_PCT_RE.search(stripped)
                        if m_pct:
                            val = int(m_pct.group(1))
                            if 0 <= val <= 100:
                                progress = max(1, min(99, val))
                                changed = True
                if changed:
                    _push_link("working", progress, stripped[:120], force=False)
            rc = proc.wait()
    except FileNotFoundError as exc:
        print(f"jarvisctl run: command not found: {cmd[0]} ({exc})", file=sys.stderr)
        last_error = f"Command not found: {cmd[0]}"
        rc = 127
    except KeyboardInterrupt:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        rc = 130
        last_error = "Interrupted by user"
    finally:
        elapsed = time.monotonic() - t0
        if rc == 0:
            for item in evidence:
                if item["state"] == "working":
                    item["state"] = "pass"
            _push_link("complete", 100, f"PASS in {elapsed:.1f}s (exit 0)", force=True)
        else:
            for item in evidence:
                if item["state"] == "working":
                    item["state"] = "fail"
            summary_msg = last_error or last_line or f"Exit {rc} after {elapsed:.1f}s"
            _push_link("blocked", max(progress, 10), summary_msg, force=True)

        if not args.no_result:
            try:
                outbox = get_json("/api/brain/outbox?after=0", timeout=6)
                seq = int(outbox.get("next_inbox_seq", 1))
                prefix = "PASS: " if rc == 0 else "FAIL: "
                card_title = sanitize_glass_text(prefix + title, 24, fallback=prefix.strip())
                card_body = (
                    sanitize_glass_text(f"OK in {elapsed:.1f}s", 48, fallback="OK")
                    if rc == 0
                    else sanitize_glass_text(last_error or f"Exit code {rc}", 48, fallback=f"Exit {rc}")
                )
                envelope = {
                    "v": 1,
                    "type": "surface.present",
                    "seq": seq,
                    "session": _stable_session_id(),
                    "id": sanitize_identifier(f"res-{int(time.time()) % 100000}", 32),
                    "ttl_ms": 15000,
                    "payload": {
                        "kind": "result",
                        "title": card_title,
                        "body": card_body,
                        "actions": [],
                    },
                }
                _brain_post_with_retry(envelope)
            except Exception:  # noqa: BLE001
                pass

        keepalive_stop.set()
        if keepalive_thread is not None:
            keepalive_thread.join(timeout=1.0)
        if leased:
            try:
                api("/api/operator/lease?release=1", "POST", b"", timeout=5)
            except Exception:  # noqa: BLE001
                pass

    return rc


def cmd_agent(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl agent",
        description="Inspect or publish Codex Agent Link state on Desk screen.",
    )
    sub = ap.add_subparsers(dest="action", required=True)

    p_status = sub.add_parser("status", help="pretty-print GET /api/agent/link")
    p_status.add_argument("--json", action="store_true", help="print raw JSON")

    p_post = sub.add_parser("post", help="publish bounded Agent Link status")
    p_post.add_argument("--title", required=True, help="task title (<=48 chars)")
    p_post.add_argument(
        "--state",
        default="working",
        choices=("working", "verifying", "waiting", "succeeded", "failed",
                 "complete", "blocked", "idle"),
        help="task state (complete->succeeded, blocked->failed, idle->succeeded)",
    )
    p_post.add_argument("--progress", type=int, default=None, help="0..100 percent")
    p_post.add_argument("--summary", default="Updated via jarvisctl", help="summary line (<=120 chars)")
    p_post.add_argument("--task-id", default=None, help="task identifier (<=48 chars)")
    p_post.add_argument("--ttl", type=int, default=900, help="TTL seconds (30..3600)")
    p_post.add_argument(
        "--step",
        action="append",
        default=[],
        metavar="LABEL:STATE",
        help="repeatable step evidence 'Label:pass|working|wait|fail' (up to 4)",
    )

    sub.add_parser("clear", help="clear/idle the Agent Link state")
    args = ap.parse_args(argv)

    if args.action == "status":
        data = get_json("/api/agent/link")
        if args.json:
            print(json.dumps(data, indent=2))
            return 0
        if not data.get("active"):
            print(f"AGENT LINK: inactive (next_revision={data.get('next_revision')}, "
                  f"updates={data.get('updates')}, rejects={data.get('rejects')})")
            return 0
        ttl_s = (data.get("ttl_ms") or 0) / 1000.0
        print(f"AGENT LINK: {str(data.get('state', '')).upper()}  {data.get('progress')}%  "
              f"title={data.get('title')!r}")
        print(f"  task_id={data.get('task_id')}  rev={data.get('revision')}  ttl={ttl_s:.0f}s")
        print(f"  summary: {data.get('summary')}")
        ev = data.get("evidence") or []
        if ev:
            print("  steps:")
            for item in ev:
                print(f"    [{item.get('state', '?'):<7}] {item.get('label', '')}")
        return 0

    if args.action == "clear":
        resp = agent_link_post(
            task_id="idle",
            state="idle",
            progress=100,
            title="Idle",
            summary="Cleared",
            evidence=[],
            ttl_s=30,
        )
        print(json.dumps(resp))
        return 0

    # post
    prog = args.progress
    if prog is None:
        prog = 100 if args.state in ("complete", "succeeded", "idle") else 0
    ttl_s = 30 if args.state == "idle" and args.ttl == 900 else args.ttl
    evidence: list[dict[str, str]] = []
    for raw_step in args.step[:4]:
        if ":" in raw_step:
            lbl, st = raw_step.rsplit(":", 1)
        else:
            lbl, st = raw_step, "wait"
        st_clean = st.strip().lower()
        if st_clean not in ("pass", "working", "wait", "fail"):
            ap.error(f"invalid step state '{st}' in '{raw_step}' (use pass|working|wait|fail)")
        evidence.append({"label": lbl.strip(), "state": st_clean})

    task_id = args.task_id or sanitize_identifier(args.title, 32, fallback="jarvisctl")
    resp = agent_link_post(
        task_id=task_id,
        state=args.state,
        progress=prog,
        title=args.title,
        summary=args.summary,
        evidence=evidence,
        ttl_s=ttl_s,
    )
    print(json.dumps(resp))
    return 0


def cmd_mode(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl mode",
        description="Read or renew the Codex operator lease.",
    )
    ap.add_argument("--renew", nargs="?", const=300, default=None, type=int,
                    metavar="TTL", help="renew Codex operator lease (default 300s)")
    ap.add_argument("--json", action="store_true", help="print raw JSON")
    args = ap.parse_args(argv)

    if args.renew is not None:
        ttl = max(10, min(900, args.renew))
        resp = json.loads(api(f"/api/operator/lease?ttl={ttl}&mode=codex", "POST", b"").decode())
        if args.json:
            print(json.dumps(resp, indent=2))
        else:
            print(f"OPERATOR: renewed  mode={resp.get('mode', 'codex')}  ttl={resp.get('ttl_s', ttl)}s")
        return 0

    lease = get_json("/api/operator/lease")
    # Enrich with cockpit.operator when available (e.g. owner field)
    if "owner" not in lease and pairing_token():
        try:
            cockpit = get_json("/api/cockpit", timeout=5)
            op = cockpit.get("operator")
            if isinstance(op, dict):
                lease = {**lease, **op}
        except Exception:  # noqa: BLE001
            pass

    if args.json:
        print(json.dumps(lease, indent=2))
        return 0
    active = bool(lease.get("active"))
    mode = lease.get("mode", "normal")
    owner = lease.get("owner") or mode
    ttl_ms = int(lease.get("ttl_ms") or 0)
    if active:
        print(f"OPERATOR: active  mode={mode}  owner={owner}  ttl={ttl_ms / 1000.0:.1f}s")
    else:
        print(f"OPERATOR: {mode} (inactive)")
    return 0


def cmd_inbox(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl inbox",
        description="Voice-to-Codex prompt queue backed by device /api/notes.",
    )
    ap.add_argument("--tag", choices=("work", "life", "all"), default="work",
                    help="filter notes by tag (default: work)")
    ap.add_argument("--done", type=int, default=None, metavar="ID",
                    help="mark note ID as completed")
    ap.add_argument("--add", default=None, metavar="TEXT",
                    help="add a new note item to the queue")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    args = ap.parse_args(argv)

    if args.add:
        tag = args.tag if args.tag != "all" else "work"
        resp = post_json("/api/notes", {"text": args.add, "tag": tag})
        if args.json:
            print(json.dumps(resp, indent=2))
        else:
            print(f"added [{tag.upper()}]: {args.add} (total={resp.get('count')})")
        return 0

    data = get_json("/api/notes")
    notes = data.get("notes") or []

    if args.done is not None:
        target = next((n for n in notes if int(n.get("id", -1)) == args.done), None)
        if target is None:
            print(f"inbox: note #{args.done} not found", file=sys.stderr)
            return 1
        if target.get("done") is True:
            if args.json:
                print(json.dumps({"ok": True, "id": args.done, "already_done": True}))
            else:
                print(f"note #{args.done} already done: {target.get('text')}")
            return 0
        resp = json.loads(api(f"/api/notes?toggle={args.done}", "POST", b"").decode())
        if args.json:
            print(json.dumps(resp, indent=2))
        else:
            print(f"marked note #{args.done} done: {target.get('text')}")
        return 0

    filtered = [
        n for n in notes
        if args.tag == "all" or str(n.get("tag", "life")).lower() == args.tag
    ]
    pending = [n for n in filtered if not n.get("done")]
    completed = [n for n in filtered if n.get("done")]

    if args.json:
        print(json.dumps({
            "tag": args.tag,
            "count": len(filtered),
            "pending": pending,
            "done": completed,
            "notes": filtered,
        }, indent=2))
        return 0

    if not filtered:
        print(f"INBOX ({args.tag}): empty")
        return 0
    print(f"INBOX ({args.tag}): {len(pending)} pending, {len(completed)} done")
    for n in pending + completed:
        mark = "x" if n.get("done") else " "
        tag = str(n.get("tag", "life")).upper()
        print(f"  [{mark}] #{n.get('id')} [{tag:<4}] {n.get('text', '')}")
    return 0


# ---------------------------------------------------------------------------
# Dev Workbench 2.0 commands
# ---------------------------------------------------------------------------

def cmd_ship(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl ship",
        description="1-command safe release loop: gates -> build -> preflight -> OTA -> verify -> flight report.",
    )
    ap.add_argument("--skip-tests", action="store_true", help="skip security, dashboard JS, and host tests")
    ap.add_argument("--skip-build", action="store_true", help="skip build-v5.sh (use existing build/jarvisrobot_v5.bin)")
    ap.add_argument("--fast", action="store_true", help="pass FAST=1 to build-v5.sh for incremental build")
    ap.add_argument("--screens", action="store_true", help="capture post-OTA screen sheet via scripts/screens.py")
    ap.add_argument("--force", action="store_true", help="bypass local battery/heap preflight checks")
    args = ap.parse_args(argv)

    bash = _find_git_bash()

    # Stage 1: Security & Test Gate
    if not args.skip_tests:
        print("==> [1/6] Security & test gate")
        r = subprocess.run([sys.executable, os.path.join(HERE, "check_secrets.py")], cwd=REPO_ROOT)
        if r.returncode != 0:
            print("ship aborted: secret scan failed", file=sys.stderr)
            return r.returncode

        node = shutil.which("node")
        if not node:
            print("ship aborted: node is required for scripts/check-dashboard-js.mjs", file=sys.stderr)
            return 1
        r = subprocess.run(
            [node, os.path.join(HERE, "check-dashboard-js.mjs"),
             os.path.join(REPO_ROOT, "main", "diagnostics.html")],
            cwd=REPO_ROOT,
        )
        if r.returncode != 0:
            print("ship aborted: dashboard JS check failed", file=sys.stderr)
            return r.returncode

        if not bash:
            print("ship aborted: Git Bash / bash required to run scripts/host-tests.sh", file=sys.stderr)
            return 1
        r = subprocess.run([bash, os.path.join(HERE, "host-tests.sh"), "-q"], cwd=REPO_ROOT)
        if r.returncode != 0:
            print("ship aborted: host-tests.sh failed", file=sys.stderr)
            return r.returncode
    else:
        print("==> [1/6] Security & test gate (skipped)")

    # Stage 2: Build Gate
    bin_path = os.path.join(REPO_ROOT, "build", "jarvisrobot_v5.bin")
    elf_path = os.path.join(REPO_ROOT, "build", "jarvisrobot_v5.elf")
    if not args.skip_build:
        print(f"==> [2/6] Build gate (FAST={'1' if args.fast else '0'})")
        if not bash:
            print("ship aborted: Git Bash / bash required to run scripts/build-v5.sh", file=sys.stderr)
            return 1
        env = dict(os.environ)
        if args.fast:
            env["FAST"] = "1"
        r = subprocess.run([bash, os.path.join(HERE, "build-v5.sh")], cwd=REPO_ROOT, env=env)
        if r.returncode != 0:
            print("ship aborted: build-v5.sh failed", file=sys.stderr)
            return r.returncode
    else:
        print("==> [2/6] Build gate (skipped)")

    if not os.path.isfile(bin_path):
        print(f"ship aborted: firmware binary missing at {bin_path}", file=sys.stderr)
        return 1
    bin_size = os.path.getsize(bin_path)
    if bin_size < 256 * 1024 or bin_size > OTA_SLOT_BYTES:
        print(f"ship aborted: implausible binary size {bin_size} B (limit {OTA_SLOT_BYTES} B)",
              file=sys.stderr)
        return 1
    headroom_kib = (OTA_SLOT_BYTES - bin_size) / 1024.0
    local_sha16 = _local_elf_sha16(elf_path)
    print(f"  binary: {bin_size:,} B ({bin_size / 1024.0:.1f} KiB, "
          f"{100.0 * bin_size / OTA_SLOT_BYTES:.1f}% of 4 MiB slot, "
          f"{headroom_kib:.1f} KiB free)  elf_sha={local_sha16 or 'n/a'}")

    # Stage 3: Device Preflight
    print("==> [3/6] Device preflight")
    sensors = get_json("/api/sensors", timeout=10)
    health = get_json("/api/device/health", timeout=10)
    bat = sensors.get("battery") or {}
    usb_present = bool(bat.get("usb_present"))
    bat_pct = bat.get("percent")
    mem = health.get("memory") or {}
    largest_internal = mem.get("largest_internal")
    old_slot = str((health.get("ota") or {}).get("running", "unknown"))
    print(f"  slot={old_slot} verdict={health.get('verdict')} "
          f"battery={bat_pct}% usb={usb_present} largest_internal={largest_internal}B")

    if not args.force:
        if not usb_present and isinstance(bat_pct, (int, float)) and 0 <= bat_pct < 30:
            print(f"ship aborted: battery is {bat_pct}% without USB power. "
                  f"Connect USB-C power or charge above 30% (or pass --force).",
                  file=sys.stderr)
            return 1
        if isinstance(largest_internal, (int, float)) and largest_internal < 8192:
            print(f"ship aborted: internal heap fragmented (largest_internal={largest_internal} B < 8192 B). "
                  f"Reboot device before OTA (or pass --force).",
                  file=sys.stderr)
            return 1

    # Stage 4: OTA Upload
    print(f"==> [4/6] OTA upload ({bin_size / 1024.0:.1f} KiB -> /api/ota/upload)")
    with open(bin_path, "rb") as fh:
        image_bytes = fh.read()
    t_up = time.monotonic()
    try:
        _, _, resp_raw = api_with_headers(
            "/api/ota/upload",
            method="POST",
            body=image_bytes,
            timeout=300.0,
            content_type="application/octet-stream",
        )
    except urllib.error.HTTPError as exc:
        detail = _http_error_detail(exc)
        print(f"OTA upload rejected (HTTP {exc.code}): {detail or exc.reason}", file=sys.stderr)
        return 1
    up_s = time.monotonic() - t_up
    print(f"  uploaded in {up_s:.1f}s ({(bin_size / 1024.0) / max(0.1, up_s):.1f} KiB/s): "
          f"{resp_raw.decode('utf-8', 'replace').strip()}")

    # Stage 5: Smart Reboot & Validation Poll
    print("==> [5/6] Waiting for reboot & OTA validation")
    time.sleep(3.0)
    deadline = time.monotonic() + 150.0
    saw_disconnect = False
    saw_pending = False
    final_health: dict[str, Any] | None = None
    rolled_back = False

    while time.monotonic() < deadline:
        try:
            h = get_json("/api/device/health", timeout=5)
        except Exception:  # noqa: BLE001
            saw_disconnect = True
            time.sleep(2.0)
            continue
        ota = h.get("ota") or {}
        running = str(ota.get("running", "unknown"))
        state = str(ota.get("state", "unknown"))
        last_inv = str(ota.get("last_invalid", "none"))
        if running != old_slot and running != "unknown":
            if state == "valid":
                final_health = h
                print(f"  VALIDATED on {running} (verdict={h.get('verdict')})")
                break
            saw_pending = True
            print(f"  probation: slot={running} state={state} verdict={h.get('verdict')}")
        else:
            if saw_pending or (saw_disconnect and state == "valid" and (time.monotonic() - t_up) > 10.0):
                final_health = h
                rolled_back = True
                print(f"ROLLBACK DETECTED: device returned to {running} "
                      f"(last_invalid={last_inv}, reset={h.get('reset')})",
                      file=sys.stderr)
                break
        time.sleep(2.0)

    if final_health is None:
        print("ship failed: timed out waiting for device OTA validation", file=sys.stderr)
        return 1

    # Stage 6: Flight Report
    print("\n==> [6/6] Flight Report")
    ota = final_health.get("ota") or {}
    fw = final_health.get("fw") or {}
    boot_ms = final_health.get("boot_ms") or {}
    disp = final_health.get("display") or {}
    mem = final_health.get("memory") or {}
    dev_sha = str(fw.get("elf_sha", "")).lower()
    sha_ok = bool(
        local_sha16 and len(dev_sha) >= 8 and (
            local_sha16.lower().startswith(dev_sha) or dev_sha.startswith(local_sha16.lower())
        )
    )
    sha_status = (
        "MATCH" if sha_ok
        else ("MISMATCH" if local_sha16 and dev_sha else "n/a")
    )
    print(f"  OTA:      running={ota.get('running')} state={ota.get('state')} "
          f"boot={ota.get('boot')} last_invalid={ota.get('last_invalid')}")
    if fw:
        print(f"  Firmware: elf_sha={dev_sha} ({sha_status})  built={fw.get('built', '-')}")
    if boot_ms:
        print(f"  Boot ms:  core={boot_ms.get('core', '-')} audio={boot_ms.get('audio', '-')} "
              f"wifi={boot_ms.get('wifi', '-')} ready={boot_ms.get('ready', '-')}")
    print(f"  Health:   verdict={final_health.get('verdict')} fps={disp.get('fps')} "
          f"flush_errors={disp.get('flush_errors')} "
          f"free_psram={mem.get('free_psram')}B largest_internal={mem.get('largest_internal')}B")

    _SYS_TASKS = {"ipc0", "ipc1", "IDLE0", "IDLE1", "sys_evt", "Tmr Svc"}
    try:
        tasks_doc = get_json("/api/diag/tasks", timeout=8)
        all_tasks = tasks_doc.get("tasks") or []
        app_tasks = sorted(
            [t for t in all_tasks if str(t.get("name", "")) not in _SYS_TASKS],
            key=lambda t: int(t.get("stack_free", 999999)),
        )
        print("  Tightest 5 App Task Stacks:")
        for t in app_tasks[:5]:
            sf = int(t.get("stack_free", 0))
            flag = "WARN" if sf < 1024 else "ok  "
            print(f"    [{flag}] {str(t.get('name', '?')):<16} stack_free={sf:>5} B  prio={t.get('prio')}")
    except Exception as exc:  # noqa: BLE001
        print(f"  Tasks:    (unavailable: {exc})")

    try:
        crash_doc = get_json("/api/diag/crash", timeout=8)
        csum = crash_doc.get("summary")
        if isinstance(csum, dict):
            print(f"  Crash:    reset={crash_doc.get('reset')} DUMP PRESENT "
                  f"task={csum.get('task')} pc={csum.get('exc_pc')} reason={csum.get('panic_reason')}")
        else:
            print(f"  Crash:    reset={crash_doc.get('reset')} dump_present={crash_doc.get('dump_present')}")
    except Exception as exc:  # noqa: BLE001
        print(f"  Crash:    (unavailable: {exc})")

    try:
        log_txt = api("/api/logs?tail=32768", timeout=15).decode("utf-8", "replace")
        ansi_re = re.compile(r"\x1b\[[0-9;]*m")
        ew = [
            ln for ln in log_txt.splitlines()
            if ansi_re.sub("", ln).lstrip().startswith(("E (", "W ("))
        ]
        print(f"  Boot Logs: {len(ew)} E/W line(s)")
        for ln in ew[-15:]:
            print(f"    {ln}")
    except Exception as exc:  # noqa: BLE001
        print(f"  Boot Logs: (unavailable: {exc})")

    if args.screens and not rolled_back:
        print("\n==> Capturing post-OTA screen sheet")
        subprocess.call([sys.executable, os.path.join(HERE, "screens.py"), "--host", host()])

    return 1 if rolled_back else 0


def _sample_tasks_cpu(interval: float = 2.0) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, float]]:
    t1 = get_json("/api/diag/tasks", timeout=8)
    time.sleep(max(0.2, interval))
    t2 = get_json("/api/diag/tasks", timeout=8)
    tot1 = int(t1.get("total_runtime", 0))
    tot2 = int(t2.get("total_runtime", 0))
    dt = (tot2 - tot1) & 0xFFFFFFFF
    run1 = {str(t.get("name")): int(t.get("run", 0)) for t in (t1.get("tasks") or [])}
    enriched: list[dict[str, Any]] = []
    idle: dict[str, float] = {}
    for t in (t2.get("tasks") or []):
        name = str(t.get("name", "?"))
        r2 = int(t.get("run", 0))
        r1 = run1.get(name, r2)
        drun = (r2 - r1) & 0xFFFFFFFF
        pct = min(100.0, (drun / dt) * 100.0) if dt > 0 else 0.0
        row = dict(t)
        row["cpu_pct"] = round(pct, 1)
        enriched.append(row)
        if name in ("IDLE0", "IDLE1"):
            idle[name] = round(pct, 1)
    return t2, enriched, idle


def cmd_bench(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl bench",
        description="Sweep core screens (and optionally 10 watch dials) and report render/CPU metrics.",
    )
    ap.add_argument("--dials", action="store_true", help="also sweep all 10 watch faces")
    ap.add_argument("--split", action="store_true", help="sample /api/display?split=1 HUD breakdown")
    ap.add_argument("--settle", type=float, default=1.4, help="seconds to settle per screen (default 1.4)")
    args = ap.parse_args(argv)

    orig_space = 0
    orig_style: str | None = None
    try:
        d0 = get_json("/api/display")
        orig_space = int(d0.get("space", 0))
    except Exception:  # noqa: BLE001
        pass
    try:
        c0 = get_json("/api/cockpit")
        orig_style = (c0.get("display") or {}).get("watch_style")
    except Exception:  # noqa: BLE001
        pass

    results: list[dict[str, Any]] = []

    def _measure(label: str) -> None:
        time.sleep(0.35)
        d1 = get_json("/api/display")
        time.sleep(max(0.4, args.settle - 0.35))
        d = get_json("/api/display")
        l1 = (list(d1.get("layer_us") or []) + [0, 0, 0, 0, 0])[:5]
        l2 = (list(d.get("layer_us") or []) + [0, 0, 0, 0, 0])[:5]
        df_l = (int(d.get("render_frames", 0)) - int(d1.get("render_frames", 0))) & 0xFFFFFFFF
        if df_l > 0 and any(int(l2[i]) >= int(l1[i]) for i in range(5)):
            layers_ms = [((int(l2[i]) - int(l1[i])) & 0xFFFFFFFF) / df_l / 1000.0 for i in range(5)]
        else:
            layers_ms = [int(x) / 1000.0 for x in l2]
        row: dict[str, Any] = {
            "name": label,
            "fps": d.get("actual_fps", 0),
            "frame_ms": (d.get("render_frame_us") or 0) / 1000.0,
            "layers_ms": layers_ms,
            "flush_err": d.get("flush_errors", 0),
            "top_split": "",
        }
        if args.split:
            try:
                s1 = get_json("/api/display?split=1")
                time.sleep(1.0)
                s2 = get_json("/api/display?split=1")
                df = (int(s2.get("render_frames", 0)) - int(s1.get("render_frames", 0))) & 0xFFFFFFFF
                sp1, sp2 = s1.get("split_us") or {}, s2.get("split_us") or {}
                if df > 0 and sp2:
                    deltas = {
                        k: ((int(sp2[k]) - int(sp1.get(k, 0))) & 0xFFFFFFFF) / df / 1000.0
                        for k in sp2
                    }
                    top = sorted(deltas.items(), key=lambda kv: kv[1], reverse=True)[:3]
                    row["top_split"] = ", ".join(f"{k}:{v:.1f}ms" for k, v in top if v > 0.05)
            except Exception:  # noqa: BLE001
                pass
        results.append(row)

    try:
        for scr in CORE_SCREENS:
            try:
                api(f"/api/glass?screen={scr}", "POST", b"", timeout=8)
            except urllib.error.HTTPError as exc:
                print(f"  skipping screen={scr} (HTTP {exc.code})", file=sys.stderr)
                continue
            _measure(scr.upper())

        if args.dials:
            api("/api/glass?screen=watch", "POST", b"", timeout=8)
            for sty in WATCH_STYLES:
                try:
                    api(f"/api/glass?style={sty}", "POST", b"", timeout=8)
                except urllib.error.HTTPError as exc:
                    print(f"  skipping dial={sty} (HTTP {exc.code})", file=sys.stderr)
                    continue
                _measure(f"WATCH:{sty}")

        _, cpu_tasks, idle = _sample_tasks_cpu(2.0)
    finally:
        if orig_style:
            try:
                api(f"/api/glass?style={orig_style.lower()}", "POST", b"", timeout=6)
            except Exception:  # noqa: BLE001
                pass
        restore_scr = SPACE_INDEX_TO_SCREEN.get(orig_space, "home")
        try:
            api(f"/api/glass?screen={restore_scr}", "POST", b"", timeout=6)
        except Exception:  # noqa: BLE001
            pass

    hdr = (f"{'Screen/Dial':<18} {'FPS':>4} {'ms/f':>6}  "
           f"{'dial':>5} {'cnvs':>5} {'hud':>5} {'shll':>5} {'surf':>5}  {'err':>3}")
    if args.split:
        hdr += "  top HUD split"
    print(hdr)
    print("-" * len(hdr))
    for r in results:
        l = r["layers_ms"]
        line = (f"{r['name']:<18} {r['fps']:>4} {r['frame_ms']:>6.1f}  "
                f"{l[0]:>5.1f} {l[1]:>5.1f} {l[2]:>5.1f} {l[3]:>5.1f} {l[4]:>5.1f}  "
                f"{r['flush_err']:>3}")
        if args.split:
            line += f"  {r['top_split']}"
        print(line)

    top_cpu = sorted(
        [t for t in cpu_tasks if t.get("name") not in ("IDLE0", "IDLE1")],
        key=lambda t: t.get("cpu_pct", 0.0),
        reverse=True,
    )[:5]
    print(f"\nCPU Headroom (2.0s delta): IDLE0={idle.get('IDLE0', 0.0):.1f}%  "
          f"IDLE1={idle.get('IDLE1', 0.0):.1f}%")
    if top_cpu:
        summary = ", ".join(f"{t['name']}={t['cpu_pct']:.1f}%" for t in top_cpu)
        print(f"Top Consumers: {summary}")
    return 0


def _render_power_view(
    sensors: dict[str, Any],
    charge: dict[str, Any],
    cockpit: dict[str, Any],
    live_slope: str = "",
) -> str:
    bat = sensors.get("battery") or {}
    care = sensors.get("power_care") or cockpit.get("power_care") or {}
    ana = sensors.get("battery_analytics") or {}
    dec = charge.get("decoded") or {}
    regs = charge.get("registers") or []
    cp_pow = cockpit.get("power") or {}
    cp_net = cockpit.get("network") or {}

    pct = bat.get("percent", cp_pow.get("percent", -1))
    mv = bat.get("millivolts", cp_pow.get("mv", 0))
    usb = bat.get("usb_present", cp_pow.get("usb"))
    charging = bat.get("charging", cp_pow.get("charging"))
    vbus = sensors.get("vbus_mv")
    if vbus is None:
        vbus = bat.get("vbus_mv")
    vsys = sensors.get("vsys_mv")
    if vsys is None:
        vsys = bat.get("vsys_mv")
    pmic_c = sensors.get("pmic_die_c")
    if pmic_c is None:
        pmic_c = bat.get("pmic_die_c")
    if pmic_c is None:
        pmic_c = care.get("pmic_die_c")
    if isinstance(pmic_c, (int, float)) and pmic_c <= -100:
        pmic_c = None
    chip_c = cp_pow.get("chip_c")
    imu = sensors.get("imu") or {}
    pmic_i2c_err = sensors.get("pmic_i2c_err", bat.get("i2c_err", 0))
    imu_i2c_err = sensors.get("imu_i2c_err", imu.get("i2c_err", 0))

    dir_val = dec.get("direction")
    dir_str = CHARGE_DIRECTIONS.get(dir_val, str(dir_val)) if dir_val is not None else "-"
    ph_val = dec.get("charge_phase")
    ph_str = CHARGE_PHASES.get(ph_val, str(ph_val)) if ph_val is not None else "-"
    reg_errs = sum(1 for r in regs if isinstance(r, dict) and r.get("attempted") and not r.get("read_ok"))

    care_en = care.get("care_enabled", care.get("enabled"))
    care_hold = care.get("holding")
    rate_valid = ana.get("rate_valid")
    eta_min = (
        ana.get("eta_full_min") if charging else ana.get("eta_empty_min")
    )
    if eta_min is None:
        eta_min = care.get("eta_min")
    rate_pct = ana.get("rate_pct_hr", care.get("rate_pct_hr"))
    mv_hr = ana.get("mv_per_hr")
    rate_disp = "warming" if rate_valid is False else (f"{rate_pct} %/hr" if rate_pct is not None else "-")
    mv_disp = "warming" if rate_valid is False else (f"{mv_hr} mV/hr" if mv_hr is not None else "-")

    lines = [
        f"POWER & AXP2101 TELEMETRY{'  (' + live_slope + ')' if live_slope else ''}",
        f"  Battery:   {pct}%  {mv} mV  charging={charging}  usb={usb}  "
        f"VBUS={vbus if vbus is not None else '-'} mV  VSYS={vsys if vsys is not None else '-'} mV",
        f"  Thermal:   PMIC die={pmic_c if pmic_c is not None else '-'} C  "
        f"Chip={chip_c if chip_c is not None else '-'} C  "
        f"max(chip={ana.get('chip_c_max', '-')}, pmic={ana.get('pmic_c_max', '-')})  "
        f"throttle={care.get('thermal_throttle', False)}  pause={care.get('thermal_pause', False)}",
        f"  Desk Care: enabled={care_en}  holding={care_hold}  "
        f"CV={care.get('cv_mv', dec.get('cv_millivolts', '-'))} mV  "
        f"ICC={care.get('icc_ma', dec.get('constant_current_milliamps', '-'))} mA",
        f"  Analytics: rate={rate_disp}  "
        f"slope={mv_disp}  "
        f"ETA={_fmt_duration_min(eta_min)}  sag={ana.get('sag_mv', '-')} mV  "
        f"on_bat={_fmt_duration_sec(ana.get('time_on_bat_s'))}  "
        f"on_usb={_fmt_duration_sec(ana.get('time_on_usb_s'))}",
        f"  Charger:   dir={dir_str}  phase={ph_str}  enabled={dec.get('charge_enabled')}  "
        f"IIN={dec.get('input_limit_milliamps', '-')} mA  "
        f"CC={dec.get('constant_current_milliamps', '-')} mA  "
        f"CV={dec.get('cv_millivolts', '-')} mV  "
        f"VINDPM={dec.get('vindpm_active')}({dec.get('vindpm_millivolts', '-')}mV)  "
        f"treg={dec.get('thermal_regulation')}  ilim={dec.get('current_limited')}  "
        f"reg_errs={reg_errs}/{len(regs)}",
        f"  System:    CPU={cp_pow.get('cpu_mhz', '-')} MHz  "
        f"Wi-Fi RSSI={cp_net.get('rssi', '-')} dBm ch={cp_net.get('channel', '-')} "
        f"PS={cp_net.get('ps_level', '-')} disc={cp_net.get('disconnects', '-')}  "
        f"I2C_err(pmic={pmic_i2c_err}, imu={imu_i2c_err})",
    ]
    return "\n".join(lines)


def cmd_power(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl power",
        description="Inspect AXP2101 power/battery telemetry or toggle 80% Desk Battery Care.",
    )
    ap.add_argument("--care", choices=("on", "off"), default=None,
                    help="toggle 80%% Desk Battery Care mode (POST /api/power/care)")
    ap.add_argument("--watch", nargs="?", const=2.0, default=None, type=float,
                    metavar="SEC", help="refresh telemetry every SEC seconds with live slope")
    ap.add_argument("--json", action="store_true", help="print combined raw JSON")
    args = ap.parse_args(argv)

    if args.care is not None:
        enable = 1 if args.care == "on" else 0
        resp = json.loads(api(f"/api/power/care?enable={enable}", "POST", b"").decode())
        if args.json:
            print(json.dumps(resp, indent=2))
        else:
            print(f"BATTERY CARE: enabled={resp.get('care_enabled')} holding={resp.get('holding')} "
                  f"cv={resp.get('cv_mv')}mV icc={resp.get('icc_ma')}mA "
                  f"thermal_throttle={resp.get('thermal_throttle')} thermal_pause={resp.get('thermal_pause')}")
        return 0

    def _fetch_all() -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
        sensors = get_json("/api/sensors", timeout=8)
        try:
            charge = get_json("/api/power/charge", timeout=8)
        except Exception:  # noqa: BLE001
            charge = {}
        try:
            cockpit = get_json("/api/cockpit", timeout=8)
        except Exception:  # noqa: BLE001
            cockpit = {}
        return sensors, charge, cockpit

    if args.watch is None:
        sensors, charge, cockpit = _fetch_all()
        if args.json:
            print(json.dumps({"sensors": sensors, "charge": charge, "cockpit": cockpit}, indent=2))
        else:
            print(_render_power_view(sensors, charge, cockpit))
        return 0

    interval = max(0.5, args.watch)
    first_t: float | None = None
    first_mv: int | None = None
    first_pct: int | None = None
    try:
        while True:
            sensors, charge, cockpit = _fetch_all()
            now = time.monotonic()
            bat = sensors.get("battery") or {}
            mv = bat.get("millivolts")
            pct = bat.get("percent")
            slope_txt = "warming up"
            if first_t is None and isinstance(mv, int) and isinstance(pct, int):
                first_t, first_mv, first_pct = now, mv, pct
            elif first_t is not None and (now - first_t) >= 5.0:
                dt_hr = (now - first_t) / 3600.0
                parts = []
                if isinstance(mv, int) and first_mv is not None:
                    parts.append(f"{(mv - first_mv) / (dt_hr * 60.0):+.1f} mV/min")
                if isinstance(pct, int) and first_pct is not None:
                    parts.append(f"{(pct - first_pct) / dt_hr:+.1f} %/hr")
                slope_txt = "live " + ", ".join(parts)
            clear_terminal()
            print(_render_power_view(sensors, charge, cockpit, live_slope=slope_txt))
            print(f"\nrefresh={interval:.1f}s  ctrl-c to exit")
            time.sleep(interval)
    except KeyboardInterrupt:
        return 0


def _overlap_new_suffix(prev: str, cur: str) -> str:
    """Overlap-dedupe fallback for older firmware without X-Jarvis-Log-Cursor."""
    if not prev:
        return cur
    if cur == prev:
        return ""
    max_k = min(len(prev), len(cur))
    # Fast anchor on last 256 bytes of prev
    anchor_len = min(256, len(prev))
    anchor = prev[-anchor_len:]
    pos = cur.rfind(anchor)
    if pos >= 0:
        return cur[pos + anchor_len:]
    for k in range(max_k, 0, -1):
        if prev.endswith(cur[:k]):
            return cur[k:]
    return cur


def cmd_tail(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl tail",
        description="Stream /api/logs using cursor streaming (with overlap-dedupe fallback).",
    )
    ap.add_argument("--grep", default=None, metavar="PAT",
                    help="case-insensitive regex filter (with ANSI highlight on TTY)")
    ap.add_argument("--interval", type=float, default=0.8,
                    help="poll interval in seconds (default 0.8)")
    ap.add_argument("--lines", type=int, default=40,
                    help="initial lines to display (default 40)")
    args = ap.parse_args(argv)

    pat = re.compile(args.grep, re.IGNORECASE) if args.grep else None
    use_color = sys.stdout.isatty()
    interval = max(0.2, args.interval)

    def _emit_lines(lines: Sequence[str]) -> None:
        for ln in lines:
            if pat is not None:
                if not pat.search(ln):
                    continue
                if use_color:
                    ln = pat.sub(lambda m: f"\x1b[1;33m{m.group(0)}\x1b[0m", ln)
            print(ln, flush=True)

    _, hdrs, raw = api_with_headers("/api/logs?tail=16384", timeout=15.0)
    text = raw.decode("utf-8", "replace")
    cursor = _header_ci(hdrs, "X-Jarvis-Log-Cursor")
    init_lines = text.splitlines()
    if args.lines > 0:
        init_lines = init_lines[-args.lines:]
    _emit_lines(init_lines)

    prev_text = text
    partial = ""
    try:
        while True:
            time.sleep(interval)
            try:
                if cursor is not None:
                    _, hdrs, chunk_raw = api_with_headers(
                        f"/api/logs?since={urllib.parse.quote(cursor)}", timeout=15.0
                    )
                    dropped = _header_ci(hdrs, "X-Jarvis-Log-Dropped")
                    if dropped and dropped not in ("0", "false"):
                        print(f"--- [log ring wrapped: dropped {dropped} bytes] ---",
                              file=sys.stderr, flush=True)
                    new_cursor = _header_ci(hdrs, "X-Jarvis-Log-Cursor")
                    if new_cursor is not None:
                        cursor = new_cursor
                    delta = chunk_raw.decode("utf-8", "replace")
                else:
                    _, hdrs, full_raw = api_with_headers("/api/logs?tail=16384", timeout=15.0)
                    new_cursor = _header_ci(hdrs, "X-Jarvis-Log-Cursor")
                    full_text = full_raw.decode("utf-8", "replace")
                    if new_cursor is not None:
                        cursor = new_cursor
                    delta = _overlap_new_suffix(prev_text, full_text)
                    prev_text = full_text
            except (urllib.error.URLError, OSError):
                continue

            if not delta:
                continue
            buf = partial + delta
            parts = buf.splitlines(keepends=True)
            if parts and not parts[-1].endswith(("\n", "\r")):
                partial = parts.pop()
            else:
                partial = ""
            _emit_lines([p.rstrip("\r\n") for p in parts])
    except KeyboardInterrupt:
        if partial:
            _emit_lines([partial])
        return 0


def cmd_tasks(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl tasks",
        description="Inspect FreeRTOS task stack watermarks and optional 2-sample CPU %%.",
    )
    ap.add_argument("--cpu", action="store_true",
                    help="take 2 samples to compute per-task CPU %% and IDLE0/IDLE1 headroom")
    ap.add_argument("--interval", type=float, default=2.0,
                    help="seconds between CPU samples (default 2.0)")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    args = ap.parse_args(argv)

    if args.cpu:
        doc, tasks, idle = _sample_tasks_cpu(args.interval)
        tasks_sorted = sorted(tasks, key=lambda t: (-t.get("cpu_pct", 0.0), int(t.get("stack_free", 0))))
    else:
        doc = get_json("/api/diag/tasks", timeout=10)
        tasks_sorted = sorted(doc.get("tasks") or [], key=lambda t: int(t.get("stack_free", 999999)))
        idle = {}

    if args.json:
        out = dict(doc)
        out["tasks"] = tasks_sorted
        if args.cpu:
            out["idle_headroom"] = idle
        print(json.dumps(out, indent=2))
        return 0

    print(f"TASKS: cpu={doc.get('cpu_mhz')}MHz  free_internal={doc.get('free_internal')}B  "
          f"largest_internal={doc.get('largest_internal_block')}B  count={len(tasks_sorted)}")
    _sys_t = {"ipc0", "ipc1", "IDLE0", "IDLE1", "sys_evt", "Tmr Svc"}
    if args.cpu:
        print(f"  CPU Headroom: IDLE0={idle.get('IDLE0', 0.0):.1f}%  IDLE1={idle.get('IDLE1', 0.0):.1f}%")
        print(f"  {'TASK':<18} {'PRIO':>4} {'CPU%':>6} {'STACK_FREE':>11}  STATUS")
        print(f"  {'-' * 52}")
        for t in tasks_sorted:
            tname = str(t.get("name", "?"))
            sf = int(t.get("stack_free", 0))
            lim = 256 if tname in _sys_t else 1024
            flag = f"WARN (<{lim}B)" if sf < lim else "ok"
            print(f"  {tname:<18} {int(t.get('prio', 0)):>4} "
                  f"{t.get('cpu_pct', 0.0):>5.1f}% {sf:>9} B  {flag}")
    else:
        print(f"  {'TASK':<18} {'PRIO':>4} {'STACK_FREE':>11}  STATUS")
        print(f"  {'-' * 44}")
        for t in tasks_sorted:
            tname = str(t.get("name", "?"))
            sf = int(t.get("stack_free", 0))
            lim = 256 if tname in _sys_t else 1024
            flag = f"WARN (<{lim}B)" if sf < lim else "ok"
            print(f"  {tname:<18} {int(t.get('prio', 0)):>4} "
                  f"{sf:>9} B  {flag}")
    return 0


def cmd_board(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="jarvisctl board",
        description="Fetch /api/diag/board and print hardware probe summary.",
    )
    ap.add_argument("--watch", choices=("pwr", "touch"), default=None,
                    help="watch PWR key (3s) or raw touch (4s)")
    ap.add_argument("--gyro", action="store_true", help="enable IMU gyro briefly and sample")
    ap.add_argument("--json", action="store_true", help="print raw JSON")
    args = ap.parse_args(argv)

    query: list[str] = []
    if args.watch:
        query.append("watch=" + args.watch)
    if args.gyro:
        query.append("gyro=1")
    path = "/api/diag/board" + ("?" + "&".join(query) if query else "")
    doc = get_json(path, timeout=20)
    if args.json:
        print(json.dumps(doc, indent=2))
        return 0

    spi = doc.get("spi") or {}
    pk = doc.get("pwr_key") or {}
    imu = doc.get("imu") or {}
    pmic = doc.get("pmic") or {}
    touch = doc.get("touch") or {}
    te = doc.get("te") or {}

    spi_mhz = (spi.get("hz", 0) / 1e6) if isinstance(spi.get("hz"), (int, float)) else 0
    pk_lvl = pk.get("level")
    pk_state = "pressed(1)" if pk_lvl == 1 else (f"idle({pk_lvl})" if pk_lvl is not None else "-")
    who = f"0x{int(imu['who_am_i']):02x}" if isinstance(imu.get("who_am_i"), int) else "-"

    print("BOARD HARDWARE SUMMARY:")
    print(f"  spi:     {'ok' if spi.get('ok') else 'FAIL'}  {spi_mhz:g} MHz  ({spi.get('source', '-')})")
    print(f"  pwr_key: {'ok' if pk.get('ok') else 'FAIL'}  level={pk_state}"
          + (f" transitions={pk.get('transitions')}" if pk.get("watched") else ""))
    print(f"  imu:     {'ok' if imu.get('ok') else 'FAIL'}  who_am_i={who}  "
          f"temp={imu.get('temp_c', '-')} C  wom={imu.get('wom', False)}")
    print(f"  pmic:    {'ok' if pmic.get('ok') else 'FAIL'}  die={pmic.get('die_c', '-')} C  "
          f"vbus={pmic.get('vbus_mv', '-')} mV  vsys={pmic.get('vsys_mv', '-')} mV  "
          f"vbat={pmic.get('vbat_mv', '-')} mV  charge_cc={pmic.get('charge_cc_ma', '-')} mA")
    print(f"  touch:   {'ok' if touch.get('ok') else 'FAIL'}"
          + (f"  reads={touch.get('reads')} max_fingers={touch.get('max_fingers')}"
             if touch.get("watched") else "  ready"))
    print(f"  te:      {'ok' if te.get('ok') else 'FAIL'}  edges={te.get('edges', '-')}  "
          f"hz={te.get('hz', '-')}")
    return 0


def main() -> int:
    ensure_utf8_console()
    # Strip --host before any `--` separator; host() re-reads sys.argv when needed.
    _, args = _extract_host_flag(sys.argv[1:])
    if not args:
        print(__doc__)
        return 2
    if args[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd, rest = args[0], args[1:]

    if cmd == "status":
        return cmd_status()
    if cmd == "listen":
        return cmd_listen()
    if cmd == "mute":
        return cmd_mute()
    if cmd == "say":
        return cmd_say(" ".join(rest))
    if cmd == "demo":
        if rest:
            print(__doc__)
            return 2
        return cmd_demo()
    if cmd == "screen":
        return cmd_screen(*rest[:1])
    if cmd == "canvas":
        return cmd_canvas(rest)
    if cmd == "tune":
        return cmd_tune(rest)
    if cmd == "taps":
        return cmd_taps(*rest[:1])
    if cmd == "vadlog":
        return cmd_vadlog(*rest[:1])
    if cmd == "voiceperf":
        return cmd_voiceperf(rest)
    if cmd == "crash":
        return cmd_crash(rest)
    if cmd == "logs":
        tail = rest[0] if rest else "16384"
        sys.stdout.write(api(f"/api/logs?tail={tail}", timeout=20).decode(
            "utf-8", "replace"))
        return 0
    if cmd == "gestures":
        return cmd_gestures(*rest[:1])
    if cmd == "reboot":
        return cmd_reboot()
    if cmd == "update":
        return cmd_update()
    if cmd == "ota":
        img = os.path.join(HERE, "..", "build", "jarvisrobot_v5.bin")
        return cmd_desk(["ota", "--image", img])
    if cmd == "art":
        img = os.path.join(HERE, "..", "build", "emote_assets.bin")
        return cmd_desk(["ota", "--assets", "--image", img])
    if cmd == "input":
        return cmd_desk(["input", *rest])
    if cmd == "takeover":
        ttl = rest[0] if rest else "300"
        return cmd_desk(["takeover", "--ttl", ttl])
    if cmd == "normal":
        return cmd_desk(["normal"])
    if cmd == "mode":
        return cmd_mode(rest)
    if cmd == "doctor":
        return cmd_desk(["doctor", *rest])
    if cmd == "volume":
        if not rest:
            print("volume requires 10..100", file=sys.stderr)
            return 2
        return cmd_desk(["levels", "--volume", rest[0]])
    if cmd == "brightness":
        if not rest:
            print("brightness requires 10..100", file=sys.stderr)
            return 2
        return cmd_desk(["levels", "--brightness", rest[0]])
    if cmd == "desk":
        return cmd_desk(rest)
    # Codex Mode 2.0
    if cmd == "ask":
        return cmd_ask(rest)
    if cmd == "run":
        return cmd_run(rest)
    if cmd == "agent":
        return cmd_agent(rest)
    if cmd == "inbox":
        return cmd_inbox(rest)
    # Dev Workbench 2.0
    if cmd == "ship":
        return cmd_ship(rest)
    if cmd == "bench":
        return cmd_bench(rest)
    if cmd == "power":
        return cmd_power(rest)
    if cmd == "tail":
        return cmd_tail(rest)
    if cmd == "tasks":
        return cmd_tasks(rest)
    if cmd == "board":
        return cmd_board(rest)

    print(__doc__)
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            print("401: this host is not paired with the device. Hold BOOT "
                  "1.5-5 s, then: python3 scripts/jarvis-desk.py --host "
                  f"{host()} pair --code NNNNNN", file=sys.stderr)
            raise SystemExit(3) from None
        if exc.code in (409, 422, 503):
            detail = _http_error_detail(exc)
            print(f"HTTP {exc.code}: {detail or exc.reason}", file=sys.stderr)
            raise SystemExit(1) from None
        raise
