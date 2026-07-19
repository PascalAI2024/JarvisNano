#!/usr/bin/env python3
"""Present bounded Codex Desk surfaces on a paired JarvisNano."""

from __future__ import annotations

import argparse
import getpass
import hashlib
import ipaddress
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable, Mapping, Sequence, TextIO


DEFAULT_DEVICE_HOST = "192.168.50.98"
DEFAULT_TIMEOUT_SECONDS = 5.0
DEFAULT_WATCH_INTERVAL_SECONDS = 0.75
KEYCHAIN_SERVICE = "com.ingeniousdigital.jarvisnano.desk"
MAX_RESPONSE_BYTES = 128 * 1024
MAX_TOKEN_BYTES = 64  # JR_CFG_PAIRING_TOKEN_CAP includes the trailing NUL
MAX_CURSOR = 0x7FFFFFFF  # firmware query_int() parses a signed int

MAX_SESSION_BYTES = 32
MAX_ID_BYTES = 32
MAX_TITLE_BYTES = 24
MAX_BODY_BYTES = 48
MAX_ACTION_ID_BYTES = 16
MAX_ACTION_LABEL_BYTES = 12
MAX_ACTIONS = 3
MIN_TTL_SECONDS = 1
MAX_TTL_SECONDS = 600
SURFACE_KINDS = ("notice", "progress", "result", "choice", "consent")

SAFE_IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]*$")
SENSITIVE_FIELD_PARTS = (
    "authorization",
    "api_key",
    "mcp_key",
    "password",
    "secret",
    "token",
)


class DeskError(Exception):
    """Expected CLI failure safe to serialize without credentials."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        http_status: int | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.http_status = http_status

    def as_payload(self) -> dict[str, Any]:
        error: dict[str, Any] = {"code": self.code, "message": self.message}
        if self.http_status is not None:
            error["http_status"] = self.http_status
        return {"ok": False, "error": error}


class JsonArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise DeskError("usage", message)


def _utf8_len(value: str) -> int:
    return len(value.encode("utf-8"))


def bounded_text(
    value: str,
    field: str,
    max_bytes: int,
    *,
    allow_empty: bool = False,
) -> str:
    if not isinstance(value, str):
        raise DeskError("invalid_input", f"{field} must be text")
    if not allow_empty and not value:
        raise DeskError("invalid_input", f"{field} must not be empty")
    if any(ord(character) < 0x20 or ord(character) == 0x7F for character in value):
        raise DeskError("invalid_input", f"{field} must not contain control characters")
    if any(character in value for character in ('<', '>', '"', '\\')):
        raise DeskError("invalid_input", f"{field} contains a character rejected by firmware")
    lowered = value.lower()
    if "http://" in lowered or "https://" in lowered or "<script" in lowered:
        raise DeskError("invalid_input", f"{field} must not contain a URL or script markup")
    if _utf8_len(value) > max_bytes:
        raise DeskError(
            "invalid_input",
            f"{field} must be at most {max_bytes} UTF-8 bytes",
        )
    return value


def bounded_identifier(value: str, field: str, max_bytes: int) -> str:
    bounded_text(value, field, max_bytes)
    if SAFE_IDENTIFIER.fullmatch(value) is None:
        raise DeskError(
            "invalid_input",
            f"{field} may contain only letters, digits, dot, underscore, colon, and dash",
        )
    return value


def bounded_nonnegative(value: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if number < 0 or number > MAX_CURSOR:
        raise argparse.ArgumentTypeError(f"must be between 0 and {MAX_CURSOR}")
    return number


def bounded_ttl(value: str) -> int:
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer number of seconds") from exc
    if number < MIN_TTL_SECONDS or number > MAX_TTL_SECONDS:
        raise argparse.ArgumentTypeError(
            f"must be between {MIN_TTL_SECONDS} and {MAX_TTL_SECONDS} seconds"
        )
    return number


def bounded_timeout(value: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if number < 0.25 or number > 30.0:
        raise argparse.ArgumentTypeError("must be between 0.25 and 30 seconds")
    return number


def bounded_interval(value: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if number < 0.1 or number > 60.0:
        raise argparse.ArgumentTypeError("must be between 0.1 and 60 seconds")
    return number


def normalize_base_url(host: str) -> str:
    value = host.strip()
    if not value:
        raise DeskError("invalid_host", "device host must not be empty")
    if "://" not in value:
        value = "http://" + value
    parsed = urllib.parse.urlsplit(value)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        raise DeskError("invalid_host", "device host must be an HTTP or HTTPS URL")
    if parsed.username is not None or parsed.password is not None:
        raise DeskError("invalid_host", "device host must not contain credentials")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise DeskError("invalid_host", "device host must not contain a path, query, or fragment")
    try:
        port = parsed.port
    except ValueError as exc:
        raise DeskError("invalid_host", "device host contains an invalid port") from exc
    hostname = parsed.hostname
    if parsed.scheme == "http":
        trusted_cleartext = hostname in ("localhost",) or hostname.endswith((".local", ".local."))
        try:
            address = ipaddress.ip_address(hostname.rstrip("."))
            trusted_cleartext = trusted_cleartext or address.is_private or address.is_loopback or address.is_link_local
        except ValueError:
            pass
        if not trusted_cleartext:
            raise DeskError(
                "insecure_host",
                "plain HTTP is limited to private, loopback, and .local device hosts",
            )
    if ":" in hostname and not hostname.startswith("["):
        hostname = f"[{hostname}]"
    authority = hostname if port is None else f"{hostname}:{port}"
    return f"{parsed.scheme}://{authority}"


def keychain_account(base_url: str) -> str:
    return "jarvis-desk@" + urllib.parse.urlsplit(base_url).netloc


def stable_session_id(base_url: str) -> str:
    seed = f"{getpass.getuser()}|{urllib.parse.urlsplit(base_url).netloc}"
    digest = hashlib.sha256(seed.encode("utf-8")).hexdigest()[:24]
    return f"desk-{digest}"


def sanitize(value: Any, secrets: Sequence[str] = ()) -> Any:
    """Remove credentials from untrusted device responses before printing."""
    if isinstance(value, dict):
        cleaned: dict[str, Any] = {}
        for key, item in value.items():
            name = str(key)
            lowered = name.lower()
            if any(part in lowered for part in SENSITIVE_FIELD_PARTS):
                cleaned[name] = "[redacted]"
            else:
                cleaned[name] = sanitize(item, secrets)
        return cleaned
    if isinstance(value, list):
        return [sanitize(item, secrets) for item in value]
    if isinstance(value, str):
        cleaned = value
        for secret in secrets:
            if secret:
                cleaned = cleaned.replace(secret, "[redacted]")
        return cleaned
    return value


def emit_json(value: Any, stream: TextIO = sys.stdout) -> None:
    stream.write(json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True))
    stream.write("\n")
    stream.flush()


@dataclass(frozen=True)
class HttpResult:
    status: int
    payload: dict[str, Any]


class DeviceClient:
    def __init__(
        self,
        base_url: str,
        timeout: float,
        *,
        opener: Callable[..., Any] = urllib.request.urlopen,
    ) -> None:
        self.base_url = normalize_base_url(base_url)
        self.timeout = timeout
        self._opener = opener

    @staticmethod
    def _read_json(response: Any, route: str) -> dict[str, Any]:
        raw = response.read(MAX_RESPONSE_BYTES + 1)
        if len(raw) > MAX_RESPONSE_BYTES:
            raise DeskError("response_too_large", f"device response exceeded {MAX_RESPONSE_BYTES} bytes")
        try:
            payload = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise DeskError("invalid_response", f"device returned invalid JSON for {route}") from exc
        if not isinstance(payload, dict):
            raise DeskError("invalid_response", f"device returned a non-object JSON response for {route}")
        return payload

    def request(
        self,
        method: str,
        path: str,
        *,
        body: Mapping[str, Any] | None = None,
        token: str | None = None,
    ) -> HttpResult:
        headers = {
            "Accept": "application/json",
            "User-Agent": "JarvisDesk/1",
        }
        if token:
            headers["X-JarvisNano-Token"] = token
        data: bytes | None = None
        if method != "GET":
            headers["X-JarvisNano-Control"] = "1"
            headers["Content-Type"] = "application/json"
            data = json.dumps(body if body is not None else {}, separators=(",", ":")).encode("utf-8")

        request = urllib.request.Request(
            self.base_url + path,
            data=data,
            headers=headers,
            method=method,
        )
        try:
            with self._opener(request, timeout=self.timeout) as response:
                payload = self._read_json(response, path)
                return HttpResult(status=int(response.status), payload=payload)
        except urllib.error.HTTPError as exc:
            # Do not echo a response body here. Pairing errors and proxies have
            # been known to return request material; status and route suffice.
            try:
                exc.read(MAX_RESPONSE_BYTES + 1)
            except Exception:
                pass
            raise DeskError(
                "http_error",
                f"device rejected {method} {path}",
                http_status=exc.code,
            ) from None
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            reason = "timed out" if isinstance(exc, TimeoutError) else "failed"
            raise DeskError("network_error", f"device request {reason}: {method} {path}") from None

    def get(self, path: str, *, token: str | None = None) -> dict[str, Any]:
        return self.request("GET", path, token=token).payload

    def post(
        self,
        path: str,
        body: Mapping[str, Any] | None,
        *,
        token: str | None,
    ) -> dict[str, Any]:
        return self.request("POST", path, body=body, token=token).payload


class MacOSKeychain:
    def __init__(
        self,
        service: str = KEYCHAIN_SERVICE,
        *,
        runner: Callable[..., Any] = subprocess.run,
    ) -> None:
        self.service = service
        self._runner = runner

    def load(self, account: str) -> str | None:
        try:
            result = self._runner(
                [
                    "security",
                    "find-generic-password",
                    "-a",
                    account,
                    "-s",
                    self.service,
                    "-w",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=5,
                check=False,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            raise DeskError("keychain_error", "macOS Keychain is unavailable") from None
        if result.returncode == 44:  # errSecItemNotFound from `security`
            return None
        if result.returncode != 0:
            raise DeskError("keychain_error", "macOS Keychain credential lookup failed")
        value = result.stdout.rstrip("\r\n")
        return value or None

    def store(self, account: str, token: str) -> None:
        # `-w` as the final option prompts on stdin. Supplying the token through
        # the pipe keeps it out of argv, terminal output, and process listings.
        try:
            result = self._runner(
                [
                    "security",
                    "add-generic-password",
                    "-U",
                    "-a",
                    account,
                    "-s",
                    self.service,
                    "-w",
                ],
                input=token + "\n",
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=10,
                check=False,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
            raise DeskError("keychain_error", "macOS Keychain is unavailable") from None
        if result.returncode != 0:
            raise DeskError("keychain_error", "macOS Keychain rejected the credential update")


def validate_token(token: str) -> str:
    if (
        not token
        or _utf8_len(token) > MAX_TOKEN_BYTES
        or any(character.isspace() or ord(character) < 0x21 or ord(character) == 0x7F
               for character in token)
    ):
        raise DeskError("invalid_token", "stored Desk token is invalid")
    return token


def resolve_token(
    keychain: MacOSKeychain,
    account: str,
    env: Mapping[str, str],
    base_url: str,
) -> str:
    token = keychain.load(account)
    if token is None:
        env_token = env.get("JARVIS_DESK_TOKEN", "")
        configured_base = normalize_base_url(
            env.get("JARVIS_DEVICE_HOST") or DEFAULT_DEVICE_HOST
        )
        if env_token and base_url != configured_base:
            raise DeskError(
                "token_host_mismatch",
                "environment Desk token is bound to JARVIS_DEVICE_HOST or the default host",
            )
        token = env_token
    if not token:
        raise DeskError(
            "not_paired",
            "no Desk token found; physically hold the panel and run pair",
        )
    return validate_token(token)


def parse_action(value: str) -> dict[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("action must use id=label")
    action_id, label = value.split("=", 1)
    try:
        bounded_identifier(action_id, "action id", MAX_ACTION_ID_BYTES)
        bounded_text(label, "action label", MAX_ACTION_LABEL_BYTES)
    except DeskError as exc:
        raise argparse.ArgumentTypeError(exc.message) from exc
    return {"id": action_id, "label": label}


def outbox_path(after: int) -> str:
    return "/api/brain/outbox?" + urllib.parse.urlencode({"after": after})


def validate_outbox(
    payload: Any,
    *,
    require_inbox_seq: bool,
    requested_after: int | None = None,
    detect_gap: bool = False,
) -> dict[str, Any]:
    if not isinstance(payload, dict) or not isinstance(payload.get("events"), list):
        raise DeskError("invalid_response", "brain outbox response is missing events")
    next_after = payload.get("next_after")
    if isinstance(next_after, bool) or not isinstance(next_after, int):
        raise DeskError("invalid_response", "brain outbox response is missing next_after")
    if next_after < 0 or next_after > MAX_CURSOR:
        raise DeskError("invalid_response", "brain outbox next_after is out of range")
    next_inbox_seq = payload.get("next_inbox_seq")
    if require_inbox_seq:
        if isinstance(next_inbox_seq, bool) or not isinstance(next_inbox_seq, int):
            raise DeskError("invalid_response", "brain outbox response is missing next_inbox_seq")
        if next_inbox_seq < 1 or next_inbox_seq > MAX_CURSOR:
            raise DeskError("invalid_response", "brain outbox next_inbox_seq is out of range")
    event_sequences: list[int] = []
    for event in payload["events"]:
        if not isinstance(event, dict) or event.get("v") != 1 or event.get("type") != "surface.action":
            raise DeskError("invalid_response", "brain outbox contains an invalid event envelope")
        event_seq = event.get("seq")
        event_payload = event.get("payload")
        if (
            isinstance(event_seq, bool)
            or not isinstance(event_seq, int)
            or event_seq < 1
            or event_seq > MAX_CURSOR
            or not isinstance(event.get("session"), str)
            or not isinstance(event.get("id"), str)
            or not isinstance(event_payload, dict)
            or not isinstance(event_payload.get("action_id"), str)
            or isinstance(event_payload.get("ts_ms"), bool)
            or not isinstance(event_payload.get("ts_ms"), int)
        ):
            raise DeskError("invalid_response", "brain outbox contains an invalid action event")
        try:
            bounded_identifier(event["session"], "event session", MAX_SESSION_BYTES)
            bounded_identifier(event["id"], "event id", MAX_ID_BYTES)
            bounded_identifier(event_payload["action_id"], "event action id", MAX_ACTION_ID_BYTES)
        except DeskError as exc:
            raise DeskError("invalid_response", exc.message) from None
        if event_sequences and event_seq <= event_sequences[-1]:
            raise DeskError("invalid_response", "brain outbox event sequence is not strictly increasing")
        event_sequences.append(event_seq)
    if event_sequences and event_sequences[-1] != next_after:
        raise DeskError("invalid_response", "brain outbox checkpoint does not match its last event")
    if detect_gap and requested_after is not None:
        missing = (
            bool(event_sequences) and event_sequences[0] > requested_after + 1
        ) or (
            not event_sequences and next_after > requested_after
        )
        if missing:
            raise DeskError(
                "event_gap",
                "brain outbox history overran; one or more physical actions were dropped",
            )
    return payload


def current_brain_state(client: DeviceClient, token: str) -> dict[str, Any]:
    return validate_outbox(
        client.get(outbox_path(0), token=token),
        require_inbox_seq=True,
    )


def next_inbox_sequence(client: DeviceClient, token: str) -> int:
    return int(current_brain_state(client, token)["next_inbox_seq"])


def command_pair(
    client: DeviceClient,
    keychain: MacOSKeychain,
    account: str,
) -> dict[str, Any]:
    try:
        response = client.post("/api/pairing/claim?rotate=1", {}, token=None)
    except DeskError as exc:
        if exc.code == "http_error" and exc.http_status == 403:
            raise DeskError(
                "physical_pairing_required",
                "hold the idle panel for 1.2 seconds, then run pair again",
                http_status=403,
            ) from None
        raise
    token = response.get("token")
    if not isinstance(token, str):
        raise DeskError("invalid_response", "pairing response did not contain a token")
    validate_token(token)
    keychain.store(account, token)
    return {
        "ok": True,
        "command": "pair",
        "paired": True,
        "storage": "macos-keychain",
    }


def command_status(
    client: DeviceClient,
    keychain: MacOSKeychain,
    account: str,
    env: Mapping[str, str],
) -> dict[str, Any]:
    token = resolve_token(keychain, account, env, client.base_url)
    outbox = validate_outbox(
        client.get(outbox_path(0), token=token),
        require_inbox_seq=False,
    )
    cockpit: Any
    try:
        cockpit = client.get("/api/cockpit")
    except DeskError as exc:
        if exc.code == "http_error" and exc.http_status in (404, 405, 410, 501):
            cockpit = {"available": False, "http_status": exc.http_status}
        else:
            raise
    return {
        "ok": True,
        "command": "status",
        "brain": sanitize(outbox, (token,)),
        "cockpit": sanitize(cockpit, (token,)),
    }


def post_surface(
    client: DeviceClient,
    token: str,
    envelope: dict[str, Any],
    *,
    retry_guard: Callable[[dict[str, Any]], None] | None = None,
) -> tuple[dict[str, Any], int]:
    for attempt in range(2):
        try:
            response = client.post("/api/brain/inbox", envelope, token=token)
            return sanitize(response, (token,)), int(envelope["seq"])
        except DeskError as exc:
            if attempt != 0 or exc.code != "http_error" or exc.http_status != 409:
                raise
            state = current_brain_state(client, token)
            if retry_guard is not None:
                retry_guard(state)
            envelope["seq"] = int(state["next_inbox_seq"])
    raise DeskError("sequence_conflict", "brain inbox sequence conflict")


def require_matching_surface(
    state: dict[str, Any],
    session: str,
    surface_id: str,
) -> None:
    surface = state.get("surface")
    if not isinstance(surface, dict) or surface.get("active") is not True:
        raise DeskError("surface_not_active", "there is no active Desk surface to dismiss")
    if surface.get("session") != session or surface.get("id") != surface_id:
        raise DeskError(
            "stale_dismissal",
            "active Desk surface does not match this session and id",
        )


def command_present(
    args: argparse.Namespace,
    client: DeviceClient,
    keychain: MacOSKeychain,
    account: str,
    env: Mapping[str, str],
    session: str,
) -> dict[str, Any]:
    surface_id = bounded_identifier(args.id, "id", MAX_ID_BYTES)
    title = bounded_text(args.title, "title", MAX_TITLE_BYTES)
    body = bounded_text(args.body, "body", MAX_BODY_BYTES)
    if len(args.action) > MAX_ACTIONS:
        raise DeskError("invalid_input", f"at most {MAX_ACTIONS} actions are allowed")
    action_ids = [action["id"] for action in args.action]
    if len(action_ids) != len(set(action_ids)):
        raise DeskError("invalid_input", "action ids must be unique")
    token = resolve_token(keychain, account, env, client.base_url)
    seq = next_inbox_sequence(client, token)
    envelope = {
        "v": 1,
        "type": "surface.present",
        "seq": seq,
        "session": session,
        "id": surface_id,
        "ttl_ms": args.ttl * 1000,
        "payload": {
            "kind": args.kind,
            "title": title,
            "body": body,
            "actions": args.action,
        },
    }
    response, accepted_seq = post_surface(client, token, envelope)
    return {
        "ok": True,
        "command": "present",
        "id": surface_id,
        "seq": accepted_seq,
        "session": session,
        "response": response,
    }


def command_dismiss(
    args: argparse.Namespace,
    client: DeviceClient,
    keychain: MacOSKeychain,
    account: str,
    env: Mapping[str, str],
    session: str,
) -> dict[str, Any]:
    surface_id = bounded_identifier(args.id, "id", MAX_ID_BYTES)
    token = resolve_token(keychain, account, env, client.base_url)
    state = current_brain_state(client, token)
    require_matching_surface(state, session, surface_id)
    seq = int(state["next_inbox_seq"])
    envelope = {
        "v": 1,
        "type": "surface.dismiss",
        "seq": seq,
        "session": session,
        "id": surface_id,
        # ttl_ms is ignored for dismissals, but the envelope keeps the same
        # bounded schema as a presentation and therefore uses its legal floor.
        "ttl_ms": MIN_TTL_SECONDS * 1000,
        "payload": {},
    }
    response, accepted_seq = post_surface(
        client,
        token,
        envelope,
        retry_guard=lambda refreshed: require_matching_surface(
            refreshed, session, surface_id
        ),
    )
    return {
        "ok": True,
        "command": "dismiss",
        "id": surface_id,
        "seq": accepted_seq,
        "session": session,
        "response": response,
    }


def command_events(
    args: argparse.Namespace,
    client: DeviceClient,
    keychain: MacOSKeychain,
    account: str,
    env: Mapping[str, str],
) -> dict[str, Any]:
    token = resolve_token(keychain, account, env, client.base_url)
    outbox = validate_outbox(
        client.get(outbox_path(args.after), token=token),
        require_inbox_seq=False,
        requested_after=args.after,
        detect_gap=True,
    )
    return {
        **sanitize(outbox, (token,)),
        "ok": True,
        "command": "events",
        "after": args.after,
    }


def watch_events(
    args: argparse.Namespace,
    client: DeviceClient,
    *,
    token: str,
    stream: TextIO = sys.stdout,
    sleeper: Callable[[float], None] = time.sleep,
    max_polls: int | None = None,
) -> int:
    cursor = args.after
    polls = 0
    emit_json(
        {"ok": True, "command": "watch", "state": "started", "after": cursor},
        stream,
    )
    try:
        while max_polls is None or polls < max_polls:
            outbox = validate_outbox(
                client.get(outbox_path(cursor), token=token),
                require_inbox_seq=False,
                requested_after=cursor,
                detect_gap=True,
            )
            next_after = int(outbox["next_after"])
            for event in outbox["events"]:
                emit_json(
                    {
                        "ok": True,
                        "command": "watch",
                        "event": sanitize(event, (token,)),
                        "next_after": event["seq"],
                    },
                    stream,
                )
            cursor = next_after
            emit_json(
                {
                    "ok": True,
                    "command": "watch",
                    "state": "caught_up",
                    "next_after": cursor,
                },
                stream,
            )
            polls += 1
            if max_polls is None or polls < max_polls:
                sleeper(args.interval)
    except KeyboardInterrupt:
        emit_json(
            {"ok": True, "command": "watch", "state": "stopped", "next_after": cursor},
            stream,
        )
    return 0


def build_parser(env: Mapping[str, str] | None = None) -> JsonArgumentParser:
    environment = os.environ if env is None else env
    parser = JsonArgumentParser(description=__doc__)
    parser.add_argument(
        "--host",
        default=environment.get("JARVIS_DEVICE_HOST") or DEFAULT_DEVICE_HOST,
        help="device host or base URL",
    )
    parser.add_argument(
        "--timeout",
        type=bounded_timeout,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="HTTP timeout in seconds (0.25-30)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("pair", help="claim after a physical panel hold and store in Keychain")
    subparsers.add_parser("status", help="read Desk outbox and device cockpit status")

    present = subparsers.add_parser("present", help="present a bounded surface")
    present.add_argument("--id", required=True)
    present.add_argument("--kind", required=True, choices=SURFACE_KINDS)
    present.add_argument("--title", required=True)
    present.add_argument("--body", required=True)
    present.add_argument("--action", action="append", default=[], type=parse_action, metavar="ID=LABEL")
    present.add_argument("--ttl", type=bounded_ttl, default=300, help="lifetime in seconds (1-600)")

    dismiss = subparsers.add_parser("dismiss", help="dismiss one surface")
    dismiss.add_argument("--id", required=True)

    events = subparsers.add_parser("events", help="read action events once")
    events.add_argument("--after", type=bounded_nonnegative, default=0)

    watch = subparsers.add_parser("watch", help="stream action events as JSON Lines")
    watch.add_argument("--after", type=bounded_nonnegative, default=0)
    watch.add_argument("--interval", type=bounded_interval, default=DEFAULT_WATCH_INTERVAL_SECONDS)
    return parser


def execute_command(
    args: argparse.Namespace,
    client: DeviceClient,
    keychain: MacOSKeychain,
    env: Mapping[str, str],
) -> dict[str, Any]:
    account = keychain_account(client.base_url)
    session = bounded_identifier(stable_session_id(client.base_url), "session", MAX_SESSION_BYTES)
    if args.command == "pair":
        return command_pair(client, keychain, account)
    if args.command == "status":
        return command_status(client, keychain, account, env)
    if args.command == "present":
        return command_present(args, client, keychain, account, env, session)
    if args.command == "dismiss":
        return command_dismiss(args, client, keychain, account, env, session)
    if args.command == "events":
        return command_events(args, client, keychain, account, env)
    raise DeskError("usage", f"unsupported command: {args.command}")


def main(argv: Sequence[str] | None = None) -> int:
    env = os.environ
    try:
        args = build_parser(env).parse_args(argv)
        base_url = normalize_base_url(args.host)
        client = DeviceClient(base_url, args.timeout)
        keychain = MacOSKeychain()
        if args.command == "watch":
            account = keychain_account(client.base_url)
            token = resolve_token(keychain, account, env, client.base_url)
            return watch_events(args, client, token=token)
        result = execute_command(args, client, keychain, env)
        emit_json(sanitize(result))
        return 0
    except DeskError as exc:
        emit_json(exc.as_payload(), sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
