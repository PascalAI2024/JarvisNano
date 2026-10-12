#!/usr/bin/env python3
"""Display-only Deck -> Nano bridge. No task replies or operator lease writes.

    python scripts/deck-nano.py --fixture --preview   # offline, no credentials
    python scripts/deck-nano.py --needs-owner-fixture --once  # real workers, fixture pulse
    python scripts/deck-nano.py --host HOST --once    # live, fenced publication
    python scripts/deck-nano.py                      # host from JARVIS_DEVICE_HOST

deckd is always read at 127.0.0.1:7717. Firmware must advertise the atomic
Deck publish fence before this program sends anything. Ctrl-C stops refresh;
the bridge's last frame expires naturally. It never clears another task.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

DECK_ORIGIN = "http://127.0.0.1:7717"
DECK_PATHS = ("/api/status", "/api/agents", "/api/tasks")
TASK_ID = "deck-face"
PUBLISH_FENCE = "no_operator_or_foreign_producer"
TIMEOUT_SECONDS = 2.0
MAX_JSON_BYTES = 1024 * 1024
MAX_POST_BYTES = 1536
MAX_SOURCE_ITEMS = 256
MAX_WORKERS = 4
POLL_SECONDS = 5.0
HEARTBEAT_SECONDS = 10.0
TTL_SECONDS = 30
WORKER_STATES = frozenset(("working", "starting", "needs-you", "stalled"))


class BridgeError(Exception):
    """A bounded diagnostic: never include server bodies, URLs or credentials."""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise BridgeError("redirect refused")


def device_origin(value: str) -> str:
    value = value.strip().rstrip("/")
    if not value.startswith(("http://", "https://")):
        value = "http://" + value
    try:
        parts = urllib.parse.urlsplit(value)
        port = parts.port
    except ValueError:
        raise BridgeError("invalid device host") from None
    if (parts.scheme not in ("http", "https") or not parts.hostname
            or parts.username is not None or parts.password is not None
            or parts.path or parts.query or parts.fragment
            or any(ord(ch) <= 0x20 or ord(ch) == 0x7F for ch in value)
            or (port is not None and not 1 <= port <= 65535)):
        raise BridgeError("invalid device host")
    return value


def _json_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate field")
        result[key] = value
    return result


def _bad_number(value):
    raise ValueError("invalid JSON number")


class JsonHTTP:
    """An origin-bound, method/path allowlist with no proxies or redirects."""

    def __init__(self, origin: str, routes: set[tuple[str, str]], token=None):
        self.origin = origin
        self.routes = frozenset(routes)
        self.token = token
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), NoRedirect())

    def request(self, method: str, path: str, payload=None):
        if (method, path) not in self.routes:
            raise BridgeError("route refused")
        headers = {"Accept": "application/json"}
        if self.token:
            headers["X-JarvisNano-Token"] = self.token
        data = None
        if payload is not None:
            data = json.dumps(payload, ensure_ascii=True, allow_nan=False,
                              separators=(",", ":")).encode("ascii")
            if len(data) > MAX_POST_BYTES:
                raise BridgeError("publish body too large")
            headers["Content-Type"] = "application/json"
        if method == "POST":
            headers["X-JarvisNano-Control"] = "1"
        req = urllib.request.Request(self.origin + path, data=data,
                                     headers=headers, method=method)
        try:
            with self.opener.open(req, timeout=TIMEOUT_SECONDS) as response:
                if not 200 <= response.status < 300:
                    raise BridgeError("HTTP status refused")
                raw = response.read(MAX_JSON_BYTES + 1)
        except urllib.error.HTTPError as exc:
            exc.close()
            raise BridgeError(f"HTTP {exc.code}") from None
        except (OSError, urllib.error.URLError):
            raise BridgeError("connection unavailable") from None
        if len(raw) > MAX_JSON_BYTES:
            raise BridgeError("response too large")
        try:
            return json.loads(raw.decode("utf-8"), object_pairs_hook=_json_pairs,
                              parse_constant=_bad_number)
        except (ValueError, RecursionError):
            raise BridgeError("invalid JSON response") from None


class DeckSource:
    def __init__(self):
        self.http = JsonHTTP(DECK_ORIGIN, {("GET", path) for path in DECK_PATHS})

    def sample(self):
        status = self.http.request("GET", "/api/status")
        if not isinstance(status, dict) or status.get("ok") is not True:
            raise BridgeError("deckd unavailable")
        agents = self.http.request("GET", "/api/agents")
        tasks = self.http.request("GET", "/api/tasks")
        return project(agents, tasks, source="live")


class FixtureSource:
    """Three synthetic workers and an attention task, never called live proof."""

    def sample(self):
        agents = [{"id": str(n), "alive": True, "state": "working"}
                  for n in range(1, 4)]
        tasks = [{"id": "fixture-owner", "state": "waiting-owner",
                  "question": "Synthetic fixture attention"}]
        return project(agents, tasks, source="fixture")


class AttentionFixtureSource:
    """Keep the live workers; label the synthetic attention overlay explicitly."""

    def __init__(self, source):
        self.source = source

    def sample(self):
        deck = self.source.sample()
        deck["source"] = "fixture"
        deck["needs_owner"] = True
        deck["owner_count"] = max(1, deck["owner_count"])
        return deck


def _source_list(value):
    if (not isinstance(value, list) or len(value) > MAX_SOURCE_ITEMS
            or any(not isinstance(item, dict) for item in value)):
        raise BridgeError("invalid deckd list")
    return value


def project(agents, tasks, *, source: str):
    if source not in ("live", "fixture"):
        raise BridgeError("invalid source")
    agents, tasks = _source_list(agents), _source_list(tasks)
    attention = set()
    for task in tasks:
        if task.get("state") == "waiting-owner":
            key, question = task.get("id"), task.get("question")
            if (not isinstance(key, str) or not key or len(key) > 256
                    or not isinstance(question, str) or not question.strip()):
                raise BridgeError("invalid owner task")
            attention.add(("task", key))
    workers = {}
    for agent in agents:
        if agent.get("alive") is not True or agent.get("role") == "orchestrator":
            continue
        state, key = agent.get("state"), agent.get("id")
        if not isinstance(state, str) or state not in WORKER_STATES:
            continue  # Idle/done/failed/exited/unknown never become running nodes.
        if not isinstance(key, str) or not key or len(key) > 256 or key in workers:
            raise BridgeError("invalid worker identity")
        because = agent.get("because")
        if (isinstance(because, dict) and because.get("stale") is True
                and state in ("working", "starting")):
            state = "stalled"
        workers[key] = state
        if state == "needs-you":
            task_id = agent.get("taskId")
            if isinstance(task_id, str) and task_id and len(task_id) <= 256:
                attention.add(("task", task_id))
            else:
                attention.add(("agent", key))
    states = [workers[key] for key in sorted(workers)]
    displayed = [{"id": f"w{n}", "label": f"WORKER {n}", "state": state}
                 for n, state in enumerate(states[:MAX_WORKERS], 1)]
    return {"source": source, "workers": displayed, "live": len(states),
            "running": states.count("working"),
            "remaining": max(0, len(states) - MAX_WORKERS),
            "needs_owner": bool(attention), "owner_count": len(attention)}


def _integer(value, lower, upper):
    return type(value) is int and lower <= value <= upper


def validate_projection(deck):
    keys = {"source", "workers", "live", "running", "remaining",
            "needs_owner", "owner_count"}
    if (not isinstance(deck, dict) or set(deck) != keys
            or deck["source"] not in ("live", "fixture")
            or not _integer(deck["live"], 0, MAX_SOURCE_ITEMS)
            or not _integer(deck["running"], 0, deck["live"])
            or not _integer(deck["owner_count"], 0, MAX_SOURCE_ITEMS * 2)
            or type(deck["needs_owner"]) is not bool
            or deck["needs_owner"] != (deck["owner_count"] > 0)
            or not isinstance(deck["workers"], list)
            or len(deck["workers"]) != min(deck["live"], MAX_WORKERS)
            or not _integer(deck["remaining"], 0, MAX_SOURCE_ITEMS)
            or deck["remaining"] != max(0, deck["live"] - MAX_WORKERS)):
        raise BridgeError("invalid projection")
    for n, worker in enumerate(deck["workers"], 1):
        if (not isinstance(worker, dict) or set(worker) != {"id", "label", "state"}
                or worker["id"] != f"w{n}" or worker["label"] != f"WORKER {n}"
                or not isinstance(worker["state"], str)
                or worker["state"] not in WORKER_STATES):
            raise BridgeError("invalid worker projection")
    shown_running = sum(w["state"] == "working" for w in deck["workers"])
    if not shown_running <= deck["running"] <= shown_running + deck["remaining"]:
        raise BridgeError("invalid running count")


def make_payload(deck, revision):
    validate_projection(deck)
    if not _integer(revision, 1, 0xFFFFFFFE):
        raise BridgeError("invalid next revision")
    return {"task_id": TASK_ID, "revision": revision,
            "state": "waiting" if deck["needs_owner"] else "working",
            "progress": 0,
            "title": "DECK FIXTURE" if deck["source"] == "fixture" else "DECK",
            "summary": ("NEEDS YOU - CHECK DECK" if deck["needs_owner"]
                        else "WORKER STATES"),
            "evidence": [], "ttl_s": TTL_SECONDS, "deck": deck}


class DeviceClient:
    def __init__(self, origin, token):
        if not token:
            raise BridgeError("pairing credential required")
        self.http = JsonHTTP(device_origin(origin), {
            ("GET", "/api/operator/lease"), ("GET", "/api/agent/link"),
            ("POST", "/api/agent/link")}, token)

    def next_revision(self):
        lease = self.http.request("GET", "/api/operator/lease")
        if not isinstance(lease, dict) or lease.get("active") is not False:
            raise BridgeError("operator owns device")
        state = self.http.request("GET", "/api/agent/link")
        if (not isinstance(state, dict) or state.get("deck_supported") is not True
                or state.get("publish_fence") != PUBLISH_FENCE):
            raise BridgeError("Deck publish fence unavailable")
        if type(state.get("active")) is not bool:
            raise BridgeError("invalid task ownership")
        if state["active"] and state.get("task_id") != TASK_ID:
            raise BridgeError("another task owns DESK")
        revision = state.get("next_revision")
        if not _integer(revision, 1, 0xFFFFFFFE):
            raise BridgeError("invalid next revision")
        return revision

    def publish(self, deck, revision):
        payload = make_payload(deck, revision)
        result = self.http.request("POST", "/api/agent/link", payload)
        if (not isinstance(result, dict) or result.get("ok") is not True
                or result.get("accepted") is not True):
            raise BridgeError("publication not accepted")


class Bridge:
    def __init__(self, source, device, clock=time.monotonic):
        self.source, self.device, self.clock = source, device, clock
        self.last_projection = None
        self.last_success = float("-inf")

    def step(self):
        deck = self.source.sample()  # A failed fresh read never refreshes old claims.
        validate_projection(deck)
        if not deck["live"] and not deck["needs_owner"]:
            self.last_projection = None
            return "idle; previous frame expires"
        signature = json.dumps(deck, sort_keys=True, separators=(",", ":"))
        now = self.clock()
        if (signature == self.last_projection
                and now - self.last_success < HEARTBEAT_SECONDS):
            return "unchanged"
        revision = self.device.next_revision()
        self.device.publish(deck, revision)  # Server fence must recheck atomically.
        self.last_projection, self.last_success = signature, self.clock()
        return "published fixture" if deck["source"] == "fixture" else "published live"


def load_credentials(value):
    """Use jarvisctl's host-bound keychain/registry/environment lookup unchanged."""
    origin = device_origin(value)
    spec = importlib.util.spec_from_file_location(
        "deck_nano_jarvisctl", Path(__file__).with_name("jarvisctl.py"))
    if spec is None or spec.loader is None:
        raise BridgeError("pairing lookup unavailable")
    ctl = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ctl)
    previous = sys.argv
    try:
        sys.argv = ["deck-nano", "--host", origin]
        token = ctl.pairing_token()
    finally:
        sys.argv = previous
    if not token:
        raise BridgeError("pairing credential required")
    return origin, token


def run(bridge, *, once=False, sleep=time.sleep):
    failures = 0
    while True:
        try:
            outcome = bridge.step()
            print("deck-nano: " + outcome)
            failures = 0
            if once:
                return 0
            delay = POLL_SECONDS
        except BridgeError as exc:
            print("deck-nano: paused (" + str(exc) + ")", file=sys.stderr)
            if once:
                return 3
            failures = min(failures + 1, 5)
            delay = min(60.0, POLL_SECONDS * 2 ** (failures - 1))
        sleep(delay)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("JARVIS_DEVICE_HOST", ""))
    fixtures = parser.add_mutually_exclusive_group()
    fixtures.add_argument("--fixture", action="store_true", help="explicit synthetic workers and attention")
    fixtures.add_argument("--needs-owner-fixture", action="store_true",
                          help="live workers with explicit synthetic owner attention")
    parser.add_argument("--preview", action="store_true", help="print projection; no device calls")
    parser.add_argument("--once", action="store_true", help="one fresh publish attempt")
    args = parser.parse_args(argv)
    try:
        source = FixtureSource() if args.fixture else DeckSource()
        if args.needs_owner_fixture:
            source = AttentionFixtureSource(source)
        if args.preview:
            print(json.dumps(source.sample(), sort_keys=True))
            return 0
        if not args.host:
            raise BridgeError("--host or JARVIS_DEVICE_HOST required")
        origin, token = load_credentials(args.host)
        return run(Bridge(source, DeviceClient(origin, token)), once=args.once)
    except BridgeError as exc:
        print("deck-nano: " + str(exc), file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        print("deck-nano: stopped; previous frame expires")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
