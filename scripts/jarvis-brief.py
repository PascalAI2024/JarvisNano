#!/usr/bin/env python3
"""jarvis-brief — Jarvis speaks first.

Composes a short spoken brief (time, weather, device battery) and delivers
it through the device: the glass shows a brief card via the caption path,
and the voice speaks it as a Gemini turn. The proactive layer's first
breath — run it by hand, from a cron, or from any agent session:

    python3 scripts/jarvis-brief.py                 # speak the brief now
    python3 scripts/jarvis-brief.py --dry           # print what it would say

Respects the owner absolutely: if the device is privacy-muted or leased,
the brief is SKIPPED (exit 3) — Jarvis never speaks first into a room that
asked for quiet.
"""
from __future__ import annotations

import datetime
import json
import os
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


def host() -> str:
    h = os.environ.get("JARVIS_DEVICE_HOST")
    if "--host" in sys.argv:
        h = sys.argv[sys.argv.index("--host") + 1]
    if not h:
        raise SystemExit("--host or JARVIS_DEVICE_HOST required")
    return h


def fetch(url: str, timeout: int = 8) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "jarvisnano"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def compose() -> str:
    now = datetime.datetime.now()
    parts = [f"Good {'morning' if now.hour < 12 else 'afternoon' if now.hour < 18 else 'evening'}, Sir."]
    parts.append(f"It is {now.strftime('%-I:%M %p')} on {now.strftime('%A')}.")
    try:
        wx = fetch("https://wttr.in/?format=%C,+%t").strip()
        if wx and "Unknown" not in wx:
            parts.append(f"Outside: {wx}.")
    except Exception:  # noqa: BLE001 - weather is a garnish, never a blocker
        pass
    try:
        g = json.loads(fetch(f"http://{host()}/api/cockpit"))
        # battery rides /api/cockpit's power block when present
        power = g.get("power") or {}
        pct = power.get("percent")
        if isinstance(pct, int) and pct <= 100:
            parts.append(f"My battery is at {pct} percent"
                         + (" and charging." if power.get("charging") else "."))
    except Exception:  # noqa: BLE001
        pass
    parts.append("Ready when you are.")
    return " ".join(parts)


def main() -> int:
    text = compose()
    if "--dry" in sys.argv:
        print(text)
        return 0
    # Owner's quiet is sacred: skip if muted or leased.
    try:
        g = json.loads(fetch(f"http://{host()}/api/gemini/live"))
        if g.get("privacy_paused"):
            print("skipped: device is privacy-muted")
            return 3
    except Exception as exc:  # noqa: BLE001
        print(f"skipped: device unreachable ({exc})")
        return 3
    print(f"briefing: {text}")
    return subprocess.call([sys.executable, os.path.join(HERE, "jarvisctl.py"),
                            "say", f"Deliver this brief verbatim, warmly: {text}",
                            "--host", host()])


if __name__ == "__main__":
    raise SystemExit(main())
