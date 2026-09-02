#!/usr/bin/env python3
"""The other end of delegate_task: a claim loop for one machine.

JarvisNano puts a spoken job on the JarvisMCP coordination board as a work
item. This loop claims one, runs it through a coding agent, and writes the
result back, which the device announces on its next poll. Standard library
only; three environment variables; no state on disk.

    JARVISMCP_URL     the gateway's execute route (https://.../execute)
    JARVISMCP_TOKEN   a bearer for that gateway (never in the repo)
    WORKER_RUNTIME    claude (default) | codex

Optional: BOARD_PROJECT (default jarvisnano-desk), WORKER_HOST (default the
machine's hostname), WORKER_MAX_SECONDS (default 900), WORKER_IDLE_SECONDS
(default 20).

Honest limits: one job at a time; a job that outlives its lease is blocked
with that reason; the agent's final message, cut to 300 characters, is the
result the device speaks. Run it under launchd or systemd; it exits on
SIGTERM between jobs.
"""
from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

URL = os.environ.get("JARVISMCP_URL", "")
TOKEN = os.environ.get("JARVISMCP_TOKEN", "")
RUNTIME = os.environ.get("WORKER_RUNTIME", "claude")
PROJECT = os.environ.get("BOARD_PROJECT", "jarvisnano-desk")
HOST = os.environ.get("WORKER_HOST", socket.gethostname().split(".")[0])
MAX_SECONDS = int(os.environ.get("WORKER_MAX_SECONDS", "900"))
IDLE_SECONDS = int(os.environ.get("WORKER_IDLE_SECONDS", "20"))
SESSION = time.strftime("%Y%m%dT%H%M%S")
IDENTITY = {"runtime": "claude" if RUNTIME == "claude" else "codex",
            "agentId": "board-worker", "hostId": HOST, "sessionId": SESSION}

_stop = False


def _term(*_):
    global _stop
    _stop = True


def execute(code: str):
    """One JarvisMCP execute call. Returns the sandbox's return value."""
    body = json.dumps({"code": code}).encode()
    req = urllib.request.Request(
        URL, data=body, method="POST",
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {TOKEN}"})
    with urllib.request.urlopen(req, timeout=40) as resp:
        payload = json.loads(resp.read().decode())
    if isinstance(payload, dict) and "result" in payload:
        return payload["result"]
    return payload


def board(method: str, args: dict):
    args = dict(args)
    args.setdefault("projectId", PROJECT)
    args.setdefault("identity", IDENTITY)
    return execute(f"return await jarvis.coordination.{method}({json.dumps(args)})")


def claim_one():
    items = board("listWorkItems", {"detail": "full", "descriptionLimit": 4000})
    if isinstance(items, dict):
        items = items.get("items") or items.get("workItems") or []
    for w in items:
        status = str(w.get("status", "")).lower()
        if status not in ("open", "queued", "ready", "pending", "todo", "new"):
            continue
        if w.get("lease") and w["lease"].get("active"):
            continue
        try:
            claimed = board("claimWorkItem", {"workItemId": w["id"],
                                              "leaseSeconds": min(3600, MAX_SECONDS + 60)})
        except urllib.error.HTTPError:
            continue
        if claimed:
            return {**w, **(claimed if isinstance(claimed, dict) else {})}
    return None


def run_agent(task: str) -> tuple[bool, str]:
    if RUNTIME == "codex":
        cmd = ["codex", "exec", "--sandbox", "workspace-write", task]
    else:
        cmd = ["claude", "-p", task, "--output-format", "json"]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=MAX_SECONDS)
    except subprocess.TimeoutExpired:
        return False, f"agent exceeded {MAX_SECONDS}s"
    except FileNotFoundError:
        return False, f"{cmd[0]} is not installed on {HOST}"
    out = proc.stdout.strip()
    if RUNTIME == "claude":
        try:
            parsed = json.loads(out)
            out = parsed.get("result") or parsed.get("content") or out
            if isinstance(out, list):
                out = " ".join(str(x.get("text", x)) for x in out)
        except (ValueError, AttributeError):
            pass
    summary = " ".join(str(out).split())[:300] or "(no output)"
    return proc.returncode == 0, summary


def work(item: dict):
    wid = item["id"]
    task = item.get("description") or item.get("title") or ""
    print(f"[worker] claimed {wid}: {task[:80]!r}", flush=True)
    board("reportProgress", {"workItemId": wid, "summary": f"started on {HOST} ({RUNTIME})"})
    started = time.time()
    ok, summary = run_agent(task)
    elapsed = int(time.time() - started)
    if ok:
        # The board wants the outcome as one object; a bare resultSummary is
        # refused with "result must be an object" (seen 2026-09-02).
        board("completeWorkItem", {"workItemId": wid,
                                   "result": {"summary": summary,
                                              "evidence": [f"{RUNTIME} on {HOST}, {elapsed}s"]}})
        print(f"[worker] completed {wid} in {elapsed}s", flush=True)
    else:
        board("blockWorkItem", {"workItemId": wid, "reason": summary[:200],
                                "evidence": [f"{RUNTIME} on {HOST}, {elapsed}s"]})
        print(f"[worker] blocked {wid}: {summary[:80]}", flush=True)


def main() -> int:
    if not URL or not TOKEN:
        print("JARVISMCP_URL and JARVISMCP_TOKEN are required", file=sys.stderr)
        return 2
    signal.signal(signal.SIGTERM, _term)
    signal.signal(signal.SIGINT, _term)
    print(f"[worker] {HOST} polling {PROJECT} every {IDLE_SECONDS}s as {RUNTIME}", flush=True)
    while not _stop:
        try:
            item = claim_one()
            if item:
                work(item)
                continue
        except urllib.error.URLError as exc:
            print(f"[worker] gateway unreachable: {exc}", flush=True)
        except Exception as exc:  # one bad item must not end the loop
            print(f"[worker] error: {exc}", flush=True)
        for _ in range(IDLE_SECONDS):
            if _stop:
                break
            time.sleep(1)
    print("[worker] stopped", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
