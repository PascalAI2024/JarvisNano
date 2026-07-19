#!/usr/bin/env python3
"""Unit tests for the host-only Jarvis Desk CLI."""

from __future__ import annotations

import argparse
import importlib.util
import io
import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest.mock import patch


MODULE_PATH = Path(__file__).with_name("jarvis-desk.py")
SPEC = importlib.util.spec_from_file_location("jarvis_desk", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
desk = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = desk
SPEC.loader.exec_module(desk)


class FakeKeychain:
    def __init__(self, value: str | None = "keychain-token") -> None:
        self.value = value
        self.loads: list[str] = []
        self.stores: list[tuple[str, str]] = []

    def load(self, account: str) -> str | None:
        self.loads.append(account)
        return self.value

    def store(self, account: str, token: str) -> None:
        self.stores.append((account, token))


class FakeClient:
    def __init__(self) -> None:
        self.base_url = "http://192.168.50.98"
        self.gets: list[tuple[str, str | None]] = []
        self.posts: list[tuple[str, dict[str, Any], str | None]] = []
        self.next_inbox_seq = 7
        self.next_after = 4
        self.events: list[dict[str, Any]] = []
        self.surface: dict[str, Any] = {
            "active": True,
            "session": "desk-session",
            "id": "build-7",
        }

    def get(self, path: str, *, token: str | None = None) -> dict[str, Any]:
        self.gets.append((path, token))
        if path.startswith("/api/brain/outbox?"):
            return {
                "events": list(self.events),
                "next_after": self.next_after,
                "next_inbox_seq": self.next_inbox_seq,
                "surface": dict(self.surface),
            }
        if path == "/api/cockpit":
            return {"voice": {"phase": "Listening"}}
        raise AssertionError(f"unexpected GET {path}")

    def post(
        self,
        path: str,
        body: dict[str, Any] | None,
        *,
        token: str | None,
    ) -> dict[str, Any]:
        self.posts.append((path, dict(body or {}), token))
        if path.startswith("/api/pairing/claim"):
            return {"ok": True, "token": "claimed-secret-token"}
        return {"ok": True, "accepted": True}


class FakeResponse:
    def __init__(self, payload: dict[str, Any], status: int = 200) -> None:
        self.status = status
        self._raw = json.dumps(payload).encode()

    def __enter__(self) -> "FakeResponse":
        return self

    def __exit__(self, *_args: Any) -> None:
        return None

    def read(self, _size: int = -1) -> bytes:
        return self._raw


def action_event(seq: int, action_id: str = "open") -> dict[str, Any]:
    return {
        "v": 1,
        "seq": seq,
        "type": "surface.action",
        "session": "desk-session",
        "id": "build-7",
        "payload": {"action_id": action_id, "ts_ms": 1234 + seq},
    }


class DeskCliTests(unittest.TestCase):
    def parse(self, *argv: str) -> argparse.Namespace:
        return desk.build_parser({}).parse_args(list(argv))

    def test_normalize_base_url_and_reject_credentials(self) -> None:
        self.assertEqual(desk.normalize_base_url("192.168.50.98"), "http://192.168.50.98")
        self.assertEqual(desk.normalize_base_url("https://device.test:8443/"), "https://device.test:8443")
        with self.assertRaises(desk.DeskError):
            desk.normalize_base_url("http://user:password@device.test")
        with self.assertRaisesRegex(desk.DeskError, "plain HTTP is limited"):
            desk.normalize_base_url("http://public.example.com")

    def test_keychain_token_precedes_environment_fallback(self) -> None:
        keychain = FakeKeychain("from-keychain")
        token = desk.resolve_token(
            keychain,
            "account",
            {"JARVIS_DESK_TOKEN": "from-env"},
            "http://192.168.50.98",
        )
        self.assertEqual(token, "from-keychain")

    def test_environment_token_is_fallback(self) -> None:
        keychain = FakeKeychain(None)
        token = desk.resolve_token(
            keychain,
            "account",
            {"JARVIS_DESK_TOKEN": "from-env"},
            "http://192.168.50.98",
        )
        self.assertEqual(token, "from-env")

    def test_environment_token_cannot_be_redirected_to_another_host(self) -> None:
        with self.assertRaisesRegex(desk.DeskError, "environment Desk token is bound"):
            desk.resolve_token(
                FakeKeychain(None),
                "account",
                {"JARVIS_DESK_TOKEN": "from-env"},
                "https://other.example.com",
            )

    def test_keychain_store_uses_stdin_not_argv(self) -> None:
        calls: list[tuple[list[str], dict[str, Any]]] = []

        def runner(command: list[str], **kwargs: Any) -> SimpleNamespace:
            calls.append((command, kwargs))
            return SimpleNamespace(returncode=0, stdout="")

        keychain = desk.MacOSKeychain(runner=runner)
        keychain.store("desk-account", "never-in-argv")
        command, kwargs = calls[0]
        self.assertNotIn("never-in-argv", command)
        self.assertEqual(command[-1], "-w")
        self.assertEqual(kwargs["input"], "never-in-argv\n")

    def test_pair_stores_claim_without_returning_secret(self) -> None:
        client = FakeClient()
        keychain = FakeKeychain(None)
        result = desk.command_pair(client, keychain, "account")
        self.assertEqual(keychain.stores, [("account", "claimed-secret-token")])
        self.assertNotIn("claimed-secret-token", json.dumps(result))
        self.assertEqual(client.posts[0][2], None)

    def test_pair_turns_forbidden_into_physical_hold_instruction(self) -> None:
        class ForbiddenClient(FakeClient):
            def post(self, *_args: Any, **_kwargs: Any) -> dict[str, Any]:
                raise desk.DeskError("http_error", "rejected", http_status=403)

        with self.assertRaisesRegex(desk.DeskError, "hold the idle panel") as raised:
            desk.command_pair(ForbiddenClient(), FakeKeychain(None), "account")
        self.assertEqual(raised.exception.code, "physical_pairing_required")

    def test_present_posts_canonical_envelope_with_keychain_token(self) -> None:
        client = FakeClient()
        keychain = FakeKeychain("keychain-token")
        args = self.parse(
            "present",
            "--id", "build-7",
            "--kind", "progress",
            "--title", "Firmware build",
            "--body", "Verifying the device",
            "--action", "open=Open",
            "--action", "stop=Stop",
            "--ttl", "45",
        )
        with patch.object(desk, "stable_session_id", return_value="desk-session"):
            result = desk.execute_command(args, client, keychain, {"JARVIS_DESK_TOKEN": "env-token"})
        path, envelope, token = client.posts[0]
        self.assertEqual(path, "/api/brain/inbox")
        self.assertEqual(token, "keychain-token")
        self.assertEqual(envelope["v"], 1)
        self.assertEqual(envelope["type"], "surface.present")
        self.assertEqual(envelope["seq"], 7)
        self.assertEqual(envelope["session"], "desk-session")
        self.assertEqual(envelope["id"], "build-7")
        self.assertEqual(envelope["ttl_ms"], 45_000)
        self.assertEqual(
            envelope["payload"],
            {
                "kind": "progress",
                "title": "Firmware build",
                "body": "Verifying the device",
                "actions": [{"id": "open", "label": "Open"}, {"id": "stop", "label": "Stop"}],
            },
        )
        self.assertEqual(result["seq"], 7)

    def test_status_authenticates_brain_read_but_not_public_cockpit(self) -> None:
        client = FakeClient()
        result = desk.command_status(client, FakeKeychain("desk-token"), "account", {})
        self.assertTrue(result["ok"])
        self.assertEqual(
            client.gets,
            [
                ("/api/brain/outbox?after=0", "desk-token"),
                ("/api/cockpit", None),
            ],
        )

    def test_dismiss_fetches_sequence_and_posts_empty_payload(self) -> None:
        client = FakeClient()
        args = self.parse("dismiss", "--id", "build-7")
        with patch.object(desk, "stable_session_id", return_value="desk-session"):
            desk.execute_command(args, client, FakeKeychain(), {})
        _, envelope, _ = client.posts[0]
        self.assertEqual(envelope["type"], "surface.dismiss")
        self.assertEqual(envelope["seq"], 7)
        self.assertEqual(envelope["ttl_ms"], 1_000)
        self.assertEqual(envelope["payload"], {})

    def test_dismiss_refuses_to_remove_another_surface(self) -> None:
        client = FakeClient()
        client.surface["id"] = "newer-surface"
        args = self.parse("dismiss", "--id", "build-7")
        with patch.object(desk, "stable_session_id", return_value="desk-session"):
            with self.assertRaisesRegex(desk.DeskError, "does not match"):
                desk.execute_command(args, client, FakeKeychain(), {})
        self.assertEqual(client.posts, [])

    def test_present_retries_one_sequence_conflict(self) -> None:
        class ConflictOnceClient(FakeClient):
            def __init__(self) -> None:
                super().__init__()
                self.conflicted = False

            def post(
                self,
                path: str,
                body: dict[str, Any] | None,
                *,
                token: str | None,
            ) -> dict[str, Any]:
                if path == "/api/brain/inbox" and not self.conflicted:
                    self.conflicted = True
                    self.next_inbox_seq = 8
                    self.posts.append((path, dict(body or {}), token))
                    raise desk.DeskError("http_error", "conflict", http_status=409)
                return super().post(path, body, token=token)

        client = ConflictOnceClient()
        args = self.parse(
            "present",
            "--id", "build-7",
            "--kind", "progress",
            "--title", "Build",
            "--body", "Retrying sequence",
        )
        with patch.object(desk, "stable_session_id", return_value="desk-session"):
            result = desk.execute_command(args, client, FakeKeychain(), {})
        self.assertEqual(result["seq"], 8)
        self.assertEqual(len(client.posts), 2)
        self.assertEqual(client.posts[1][1]["seq"], 8)

    def test_present_rejects_more_than_three_actions(self) -> None:
        args = self.parse(
            "present",
            "--id", "surface",
            "--kind", "choice",
            "--title", "Choose",
            "--body", "Select one",
            "--action", "a=A",
            "--action", "b=B",
            "--action", "c=C",
            "--action", "d=D",
        )
        with self.assertRaisesRegex(desk.DeskError, "at most 3"):
            desk.command_present(args, FakeClient(), FakeKeychain(), "account", {}, "desk-session")

    def test_present_rejects_firmware_field_overflow(self) -> None:
        args = self.parse(
            "present",
            "--id", "surface",
            "--kind", "notice",
            "--title", "x" * 25,
            "--body", "short",
        )
        with self.assertRaisesRegex(desk.DeskError, "title must be at most 24"):
            desk.command_present(args, FakeClient(), FakeKeychain(), "account", {}, "desk-session")

    def test_present_rejects_text_the_firmware_rejects(self) -> None:
        args = self.parse(
            "present",
            "--id", "surface",
            "--kind", "notice",
            "--title", "Open https://x.test",
            "--body", "short",
        )
        with self.assertRaisesRegex(desk.DeskError, "must not contain a URL"):
            desk.command_present(args, FakeClient(), FakeKeychain(), "account", {}, "desk-session")

    def test_http_post_applies_control_and_token_headers(self) -> None:
        opened: list[tuple[Any, float]] = []

        def opener(request: Any, *, timeout: float) -> FakeResponse:
            opened.append((request, timeout))
            return FakeResponse({"ok": True})

        client = desk.DeviceClient("https://device.test", 3.5, opener=opener)
        client.post("/api/brain/inbox", {"v": 1}, token="desk-secret")
        request, timeout = opened[0]
        headers = {key.lower(): value for key, value in request.header_items()}
        self.assertEqual(timeout, 3.5)
        self.assertEqual(headers["x-jarvisnano-control"], "1")
        self.assertEqual(headers["x-jarvisnano-token"], "desk-secret")
        self.assertEqual(json.loads(request.data), {"v": 1})

    def test_sanitize_redacts_secret_fields_and_values(self) -> None:
        value = {
            "token": "top-secret",
            "message": "request contained top-secret",
            "nested": {"authorization": "Bearer top-secret"},
        }
        cleaned = desk.sanitize(value, ("top-secret",))
        self.assertEqual(cleaned["token"], "[redacted]")
        self.assertEqual(cleaned["message"], "request contained [redacted]")
        self.assertEqual(cleaned["nested"]["authorization"], "[redacted]")

    def test_events_use_requested_cursor(self) -> None:
        client = FakeClient()
        client.events = [action_event(4)]
        result = desk.command_events(
            self.parse("events", "--after", "3"),
            client,
            FakeKeychain("desk-token"),
            "account",
            {},
        )
        self.assertEqual(client.gets, [("/api/brain/outbox?after=3", "desk-token")])
        self.assertEqual(result["next_after"], 4)
        self.assertEqual(len(result["events"]), 1)

    def test_events_fail_loudly_when_ring_history_has_a_gap(self) -> None:
        client = FakeClient()
        client.next_after = 5
        client.events = [action_event(5)]
        with self.assertRaisesRegex(desk.DeskError, "physical actions were dropped") as raised:
            desk.command_events(
                self.parse("events", "--after", "3"),
                client,
                FakeKeychain("desk-token"),
                "account",
                {},
            )
        self.assertEqual(raised.exception.code, "event_gap")

    def test_watch_emits_json_lines_and_advances_cursor(self) -> None:
        client = FakeClient()
        client.next_after = 5
        client.events = [action_event(4), action_event(5, "stop")]
        output = io.StringIO()
        args = self.parse("watch", "--after", "3", "--interval", "0.1")
        desk.watch_events(
            args,
            client,
            token="desk-token",
            stream=output,
            sleeper=lambda _: None,
            max_polls=1,
        )
        lines = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(lines[0]["state"], "started")
        self.assertEqual(lines[1]["event"]["type"], "surface.action")
        self.assertEqual(lines[1]["next_after"], 4)
        self.assertEqual(lines[2]["next_after"], 5)
        self.assertEqual(lines[3]["state"], "caught_up")
        self.assertEqual(lines[3]["next_after"], 5)
        self.assertEqual(client.gets, [("/api/brain/outbox?after=3", "desk-token")])


if __name__ == "__main__":
    unittest.main()
