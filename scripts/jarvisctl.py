#!/usr/bin/env python3
"""jarvisctl — the JarvisNano operator tool.

One command for everything an operator (human or AI session) does with the
device over LAN. Host comes from --host or $JARVIS_DEVICE_HOST.

    jarvisctl status                     # one-line health verdict + key state
    jarvisctl listen | mute              # arm / privacy-pause the voice
    jarvisctl say "text"                 # speak a turn through Gemini
    jarvisctl screen [out.png]           # capture the glass (PNG via sips/PIL)
    jarvisctl canvas image.png [--ttl S] # push an image to the glass
    jarvisctl canvas --clear
    jarvisctl tune [pbgain=250] [speakmic=21] [mic=24] [vol=90] [barge=1]
    jarvisctl taps [outdir]              # WAV taps + metrics
    jarvisctl vadlog [out.csv]           # barge/VAD decision log
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

HERE = os.path.dirname(os.path.abspath(__file__))


def host() -> str:
    h = None
    argv = sys.argv[1:]
    if "--host" in argv:
        h = argv[argv.index("--host") + 1]
    h = h or os.environ.get("JARVIS_DEVICE_HOST")
    if not h:
        raise SystemExit("--host or JARVIS_DEVICE_HOST required")
    return h


def api(path: str, method: str = "GET", body: bytes | None = None,
        timeout: int = 10):
    req = urllib.request.Request(f"http://{host()}{path}", data=body,
                                 method=method)
    req.add_header("X-JarvisNano-Control", "1")
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
    if d.get("flush_errors", 0):
        problems.append(f"display flush errors={d['flush_errors']}")
    verdict = "HEALTHY" if not problems else "ATTENTION: " + ", ".join(problems)
    print(f"{verdict}\n"
          f"  phase={g.get('phase')} mic_rms={g.get('mic_rms')} "
          f"deaths={g.get('deaths')}\n"
          f"  internal={g.get('free_internal_heap')}B "
          f"psram={g.get('free_psram')}B "
          f"display={d.get('actual_fps')}fps")
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
    if cmd == "reboot":
        return cmd_reboot()
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
