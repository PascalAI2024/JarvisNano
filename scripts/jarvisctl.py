#!/usr/bin/env python3
"""jarvisctl — the JarvisNano operator tool.

One command for everything an operator (human or AI session) does with the
device over LAN. Host comes from --host or $JARVIS_DEVICE_HOST.

    jarvisctl status                     # one-line health verdict + key state
    jarvisctl listen | mute              # arm / privacy-pause the voice
    jarvisctl say "text"                 # speak a turn through Gemini
    jarvisctl demo                       # queue the 27 s on-glass showcase reel
    jarvisctl screen [out.png]           # capture the glass (PNG via sips/PIL)
    jarvisctl canvas image.png [--ttl S] # push an image to the glass
    jarvisctl canvas --clear
    jarvisctl tune [pbgain=250] [speakmic=21] [mic=24] [vol=90] [barge=1]
    jarvisctl tune preroll=600 refill=1000  # jitter buffer, ms (live)
    jarvisctl taps [outdir]              # WAV taps + metrics
    jarvisctl vadlog [out.csv]           # barge/VAD decision log
    jarvisctl logs [tail_bytes]          # device log ring — read AFTER, no monitor
    jarvisctl gestures [lines]           # recent physical inputs + resolved actions
    jarvisctl input tap|double|long|swipe [left|right|up|down] [edge]
    jarvisctl takeover [ttl_s]           # Codex owns glass; double-tap exits
    jarvisctl normal                     # release Codex mode explicitly
    jarvisctl mode                       # read current glass owner
    jarvisctl desk present ...           # bounded interactive Desk surface
    jarvisctl doctor [--repair]           # diagnose logs/counters safely
    jarvisctl volume 10..100              # paired persistent speaker level
    jarvisctl brightness 10..100          # paired persistent mood ceiling
    jarvisctl ota                        # upload built firmware over Wi-Fi
    jarvisctl art                        # upload the built face clips (emote_assets.bin)
    jarvisctl update                     # flash built firmware over USB
    jarvisctl reboot                     # watchdog reset via esptool (USB)
Exit code 0 = healthy/ok. `status` exits 1 when the device is deaf/muted so
scripts and agents can gate on it.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import urllib.request
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
KEYCHAIN_SERVICE = "com.ingeniousdigital.jarvisnano.desk"


def host() -> str:
    value = None
    argv = sys.argv[1:]
    if "--host" in argv:
        value = argv[argv.index("--host") + 1]
    value = value or os.environ.get("JARVIS_DEVICE_HOST")
    if not value:
        raise SystemExit("--host or JARVIS_DEVICE_HOST required")
    return value


def base_url() -> str:
    value = host().rstrip("/")
    return value if value.startswith(("http://", "https://")) else "http://" + value


def pairing_token() -> str | None:
    """Load this host's pairing token without exposing it in argv or output."""
    account = "jarvis-desk@" + urllib.parse.urlsplit(base_url()).netloc
    try:
        result = subprocess.run(
            ["security", "find-generic-password", "-a", account,
             "-s", KEYCHAIN_SERVICE, "-w"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return None
    if result.returncode != 0:
        return None
    token = result.stdout.rstrip("\r\n")
    encoded = token.encode("utf-8")
    if not (32 <= len(encoded) <= 64) or any(
            byte <= 0x20 or byte == 0x7F for byte in encoded):
        return None
    return token


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


def get_json(path: str) -> dict:
    return json.loads(api(path))


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
    # Playback holes are the one thing the transport counters cannot see:
    # every byte arrived and the speaker still stuttered. Show them here so
    # "choppy" is a number before it is a debugging session.
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


def cmd_screen(out: str = "glass.png") -> int:
    ppm = out + ".ppm"
    with open(ppm, "wb") as f:
        f.write(api("/api/display/snapshot.ppm", timeout=20))
    if subprocess.run(["sips", "-s", "format", "png", ppm, "--out", out],
                      capture_output=True).returncode != 0:
        from PIL import Image
        Image.open(ppm).save(out)
    os.unlink(ppm)
    print(out)
    return 0


def cmd_canvas(argv: list[str]) -> int:
    return subprocess.call([sys.executable,
                            os.path.join(HERE, "send-canvas.py"),
                            *argv, "--host", host()])


def cmd_tune(argv: list[str]) -> int:
    q = "&".join(a.replace("=", "=") for a in argv if "=" in a)
    print(api(f"/api/debug/gain?{q}", "POST", b"").decode())
    return 0


def cmd_taps(outdir: str = "taps-out") -> int:
    return subprocess.call([sys.executable,
                            os.path.join(HERE, "live-device.py"),
                            "audio-taps", "--host", host(), "--out", outdir])


def cmd_vadlog(out: str = "vadlog.csv") -> int:
    with open(out, "wb") as f:
        f.write(api("/api/diag/vadlog", timeout=20))
    print(out)
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
    """Courteous flash: claim the operator lease so the glass announces
    "JARVIS AT WORK" BEFORE the write stalls rendering, then flash the
    already-built image over USB and let boot release the lease naturally.
    Never flash a device the owner is using without this."""
    try:
        if cmd_desk(["takeover", "--ttl", "180"]) != 0:
            raise RuntimeError("paired takeover failed")
        print("Codex mode claimed — glass announces the update")
    except Exception as exc:  # noqa: BLE001 - device may be wedged; flash anyway
        print(f"takeover skipped ({exc}) — flashing regardless")
    import time
    time.sleep(1.5)   # let the caption land before the stall
    env = dict(os.environ, NO_BUILD="1")
    return subprocess.call(
        ["bash", os.path.join(HERE, "flash-v5.sh")], env=env)


def cmd_reboot() -> int:
    esptool = os.path.join(HERE, "..", ".build_tools", "esptool", "bin",
                           "python")
    return subprocess.call([esptool, "-m", "esptool", "--after",
                            "watchdog-reset", "flash-id"],
                           stdout=subprocess.DEVNULL)


def main() -> int:
    # strip the --host pair; host() re-reads it from sys.argv when needed
    args = sys.argv[1:]
    if "--host" in args:
        i = args.index("--host")
        args = args[:i] + args[i + 2:]
    if not args:
        print(__doc__)
        return 2
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
        # The faces live in their own partition and do not ride with the app.
        img = os.path.join(HERE, "..", "build", "emote_assets.bin")
        return cmd_desk(["ota", "--assets", "--image", img])
    if cmd == "input":
        # Paired synthetic input is intentionally non-physical authority.
        return cmd_desk(["input", *rest])
    if cmd == "takeover":
        ttl = rest[0] if rest else "300"
        return cmd_desk(["takeover", "--ttl", ttl])
    if cmd == "normal":
        return cmd_desk(["normal"])
    if cmd == "mode":
        print(api("/api/operator/lease").decode())
        return 0
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
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
