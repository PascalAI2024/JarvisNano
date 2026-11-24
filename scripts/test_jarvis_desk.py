#!/usr/bin/env python3
"""Unit tests for the host-only Jarvis Desk CLI."""

from __future__ import annotations

import argparse
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType, SimpleNamespace
from typing import Any
from unittest.mock import patch


MODULE_PATH = Path(__file__).with_name("jarvis-desk.py")
SPEC = importlib.util.spec_from_file_location("jarvis_desk", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
desk = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = desk
SPEC.loader.exec_module(desk)

CTL_PATH = Path(__file__).with_name("jarvisctl.py")
CTL_SPEC = importlib.util.spec_from_file_location("jarvisctl", CTL_PATH)
assert CTL_SPEC is not None and CTL_SPEC.loader is not None
ctl = importlib.util.module_from_spec(CTL_SPEC)
sys.modules[CTL_SPEC.name] = ctl
CTL_SPEC.loader.exec_module(ctl)

CANVAS_PATH = Path(__file__).with_name("send-canvas.py")
CANVAS_SPEC = importlib.util.spec_from_file_location("send_canvas", CANVAS_PATH)
assert CANVAS_SPEC is not None and CANVAS_SPEC.loader is not None
send_canvas = importlib.util.module_from_spec(CANVAS_SPEC)
sys.modules[CANVAS_SPEC.name] = send_canvas
CANVAS_SPEC.loader.exec_module(send_canvas)


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
        self.base_url = "http://192.0.2.10"
        self.gets: list[tuple[str, str | None]] = []
        self.posts: list[tuple[str, dict[str, Any], str | None]] = []
        self.pair_codes: list[str | None] = []
        self.next_inbox_seq = 7
        self.next_after = 4
        self.events: list[dict[str, Any]] = []
        self.binary_posts: list[tuple[str, int, str]] = []
        self.text_gets: list[tuple[str, str]] = []
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
        if path == "/api/operator/lease":
            return {"active": False, "mode": "normal", "ttl_ms": 0}
        if path == "/api/device/health":
            return {
                "verdict": "ok", "repairable": False,
                "privacy": False, "flip_muted": False, "operator": False,
            }
        if path == "/api/diag/tasks":
            return {"tasks": [], "largest_internal": 12000}
        if path == "/api/audio/taps":
            return {"available": True, "taps": {}}
        if path == "/api/sensors":
            return {"imu": {"available": True}, "battery": {"available": True}}
        raise AssertionError(f"unexpected GET {path}")

    def post(
        self,
        path: str,
        body: dict[str, Any] | None,
        *,
        token: str | None,
        pair_code: str | None = None,
    ) -> dict[str, Any]:
        self.posts.append((path, dict(body or {}), token))
        self.pair_codes.append(pair_code)
        if path.startswith("/api/pairing/claim"):
            return {"ok": True, "token": "claimed-secret-token"}
        return {"ok": True, "accepted": True}

    def post_binary(
        self,
        path: str,
        body: bytes,
        *,
        token: str,
        timeout: float = 300.0,
    ) -> dict[str, Any]:
        self.binary_posts.append((path, len(body), token))
        return {"ok": True, "rebooting": True}


    def get_text(
        self,
        path: str,
        *,
        token: str,
        max_bytes: int = 16384,
    ) -> str:
        self.text_gets.append((path, token))
        return "E transport_ws: Error transport_poll_write(0)\n"


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
        self.assertEqual(desk.normalize_base_url("192.0.2.10"), "http://192.0.2.10")
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
            {"JARVIS_DESK_TOKEN": "from-env", "JARVIS_DEVICE_HOST": "192.0.2.10"},
            "http://192.0.2.10",
        )
        self.assertEqual(token, "from-keychain")

    def test_environment_token_is_fallback(self) -> None:
        keychain = FakeKeychain(None)
        token = desk.resolve_token(
            keychain,
            "account",
            {"JARVIS_DESK_TOKEN": "from-env", "JARVIS_DEVICE_HOST": "192.0.2.10"},
            "http://192.0.2.10",
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

    def test_pairing_token_env_name_is_accepted_as_alias(self) -> None:
        token = desk.resolve_token(
            FakeKeychain(None),
            "account",
            {"JARVIS_PAIRING_TOKEN": "from-env", "JARVIS_DEVICE_HOST": "192.0.2.10"},
            "http://192.0.2.10",
        )
        self.assertEqual(token, "from-env")

    def test_missing_security_binary_reads_as_nothing_stored(self) -> None:
        # Windows/Linux: no `security` on PATH. That must be "no token here"
        # so resolve_token() reaches the env fallback, not a keychain_error.
        def runner(*_args: Any, **_kwargs: Any) -> SimpleNamespace:
            raise FileNotFoundError("security")

        self.assertIsNone(desk.MacOSKeychain(runner=runner).load("account"))
        token = desk.resolve_token(
            desk.MacOSKeychain(runner=runner),
            "account",
            {"JARVIS_PAIRING_TOKEN": "from-env", "JARVIS_DEVICE_HOST": "192.0.2.10"},
            "http://192.0.2.10",
        )
        self.assertEqual(token, "from-env")

    def test_native_keychain_is_absent_off_macos(self) -> None:
        with patch.object(desk.sys, "platform", "win32"):
            self.assertIsNone(desk.MacOSKeychain().load("account"))

    def test_keychain_store_uses_stdin_not_argv(self) -> None:
        calls: list[tuple[list[str], dict[str, Any]]] = []

        def runner(command: list[str], **kwargs: Any) -> SimpleNamespace:
            calls.append((command, kwargs))
            if "find-generic-password" in command:
                return SimpleNamespace(returncode=0,
                                       stdout=kwargs.get("input", "never-in-argv"))
            return SimpleNamespace(returncode=0, stdout="")

        keychain = desk.MacOSKeychain(runner=runner)
        keychain.store("desk-account", "never-in-argv")
        command, kwargs = calls[0]
        self.assertNotIn("never-in-argv", command)
        self.assertNotIn("-w", command)
        self.assertEqual(kwargs["input"], "never-in-argv\n")

    def test_pair_stores_claim_without_returning_secret(self) -> None:
        client = FakeClient()
        keychain = FakeKeychain(None)
        result = desk.command_pair(client, keychain, "account", "123456")
        self.assertEqual(keychain.stores, [("account", "claimed-secret-token")])
        self.assertNotIn("claimed-secret-token", json.dumps(result))
        self.assertEqual(client.posts[0][2], None)
        self.assertEqual(client.pair_codes[0], "123456")

    def test_pair_turns_forbidden_into_physical_claim_instruction(self) -> None:
        class ForbiddenClient(FakeClient):
            def post(self, *_args: Any, **_kwargs: Any) -> dict[str, Any]:
                raise desk.DeskError("http_error", "rejected", http_status=403)

        with self.assertRaisesRegex(desk.DeskError, "hold BOOT") as raised:
            desk.command_pair(
                ForbiddenClient(), FakeKeychain(None), "account", "123456"
            )
        self.assertEqual(raised.exception.code, "physical_pairing_required")
        self.assertIn("60 seconds", raised.exception.message)

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

    def test_status_authenticates_brain_and_cockpit_reads(self) -> None:
        # /api/cockpit answers 401 once JR_DEV_OPEN_DIAGNOSTICS is 0; the
        # public-cockpit assumption only held on a dev-open image.
        client = FakeClient()
        result = desk.command_status(client, FakeKeychain("desk-token"), "account", {})
        self.assertTrue(result["ok"])
        self.assertEqual(
            client.gets,
            [
                ("/api/brain/outbox?after=0", "desk-token"),
                ("/api/cockpit", "desk-token"),
            ],
        )

    def test_takeover_and_release_use_paired_token(self) -> None:
        client = FakeClient()
        keychain = FakeKeychain("desk-token")
        takeover = self.parse("takeover", "--ttl", "45")
        normal = self.parse("normal")
        desk.execute_command(takeover, client, keychain, {})
        desk.execute_command(normal, client, keychain, {})
        self.assertEqual(
            client.posts,
            [
                ("/api/operator/lease?ttl=45", {}, "desk-token"),
                ("/api/operator/lease?release=1", {}, "desk-token"),
            ],
        )

    def test_mode_is_public_read_only(self) -> None:
        client = FakeClient()
        mode = self.parse("mode")
        result = desk.execute_command(mode, client, FakeKeychain(), {})
        self.assertEqual(result["response"]["mode"], "normal")
        self.assertEqual(client.gets, [("/api/operator/lease", None)])

    def test_ota_uses_paired_binary_request(self) -> None:
        client = FakeClient()
        # A directory, not NamedTemporaryFile: Windows refuses a second open
        # of a file that is still held open, so the OTA path could never be
        # tested there (2026-09-05).
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "jarvisrobot_v5.bin"
            image.write_bytes(b"x" * desk.MIN_OTA_BYTES)
            args = self.parse("ota", "--image", str(image))
            result = desk.execute_command(
                args, client, FakeKeychain("desk-token"), {})
        self.assertEqual(result["bytes"], desk.MIN_OTA_BYTES)
        self.assertEqual(
            client.binary_posts,
            [("/api/ota/upload", desk.MIN_OTA_BYTES, "desk-token")],
        )


    def test_doctor_triages_logs_without_mutation(self) -> None:
        client = FakeClient()
        args = self.parse("doctor")
        result = desk.execute_command(
            args, client, FakeKeychain("desk-token"), {})
        self.assertTrue(result["ok"])
        self.assertEqual(result["verdict"], "ok")
        self.assertEqual(result["incidents"][0]["category"], "websocket")
        self.assertEqual(client.posts, [])

    def test_levels_posts_paired_bounded_values(self) -> None:
        client = FakeClient()
        args = self.parse(
            "levels", "--volume", "40", "--brightness", "60")
        desk.execute_command(args, client, FakeKeychain("desk-token"), {})
        self.assertEqual(
            client.posts,
            [("/api/device/levels?volume=40&brightness=60",
              {}, "desk-token")],
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
        client.post(
            "/api/brain/inbox",
            {"v": 1},
            token="desk-secret",
            pair_code="123456",
        )
        request, timeout = opened[0]
        headers = {key.lower(): value for key, value in request.header_items()}
        self.assertEqual(timeout, 3.5)
        self.assertEqual(headers["x-jarvisnano-control"], "1")
        self.assertEqual(headers["x-jarvisnano-token"], "desk-secret")
        self.assertEqual(headers["x-jarvisnano-pair-code"], "123456")
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

class FakeUrlResponse:
    def __enter__(self) -> "FakeUrlResponse":
        return self

    def __exit__(self, *_args: Any) -> None:
        return None

    def read(self) -> bytes:
        return b'{"ok":true}'


class JarvisCtlAuthTests(unittest.TestCase):
    def test_pairing_token_uses_host_bound_keychain_account(self) -> None:
        completed = SimpleNamespace(returncode=0, stdout="t" * 32)
        with patch.object(ctl, "host", return_value="device.local"), \
             patch.object(ctl.subprocess, "run", return_value=completed) as run:
            self.assertEqual(ctl.pairing_token(), "t" * 32)
        self.assertIn("jarvis-desk@device.local", run.call_args.args[0])

    def test_pairing_token_falls_back_to_host_bound_env_without_keychain(self) -> None:
        env = {"JARVIS_PAIRING_TOKEN": "t" * 32, "JARVIS_DEVICE_HOST": "device.local"}
        with patch.object(ctl, "host", return_value="device.local"), \
             patch.object(ctl.subprocess, "run", side_effect=FileNotFoundError("security")), \
             patch.dict(ctl.os.environ, env, clear=True):
            self.assertEqual(ctl.pairing_token(), "t" * 32)

    def test_env_token_never_rides_to_another_host(self) -> None:
        no_keychain = patch.object(ctl.subprocess, "run",
                                   side_effect=FileNotFoundError("security"))
        # --host names a different device than the one the token is bound to.
        env = {"JARVIS_PAIRING_TOKEN": "t" * 32, "JARVIS_DEVICE_HOST": "other.local"}
        with patch.object(ctl, "host", return_value="device.local"), no_keychain, \
             patch.dict(ctl.os.environ, env, clear=True):
            self.assertIsNone(ctl.pairing_token())
        # No binding host at all: the token stays home.
        with patch.object(ctl, "host", return_value="device.local"), no_keychain, \
             patch.dict(ctl.os.environ, {"JARVIS_PAIRING_TOKEN": "t" * 32}, clear=True):
            self.assertIsNone(ctl.pairing_token())
        # A malformed env token is not a token.
        env = {"JARVIS_PAIRING_TOKEN": "short", "JARVIS_DEVICE_HOST": "device.local"}
        with patch.object(ctl, "host", return_value="device.local"), no_keychain, \
             patch.dict(ctl.os.environ, env, clear=True):
            self.assertIsNone(ctl.pairing_token())

    def test_api_attaches_keychain_token_to_protected_requests(self) -> None:
        captured: list[Any] = []

        def open_request(request: Any, timeout: int) -> FakeUrlResponse:
            captured.append((request, timeout))
            return FakeUrlResponse()

        with patch.object(ctl, "host", return_value="https://device.local/"), \
             patch.object(ctl, "pairing_token", return_value="t" * 32), \
             patch.object(ctl.urllib.request, "urlopen", side_effect=open_request):
            self.assertEqual(
                ctl.api("/api/voice/control?armed=1", "POST", b""),
                b'{"ok":true}',
            )
        request = captured[0][0]
        self.assertEqual(request.get_header("X-jarvisnano-token"), "t" * 32)
        self.assertEqual(request.get_header("X-jarvisnano-control"), "1")
        self.assertEqual(
            request.full_url,
            "https://device.local/api/voice/control?armed=1",
        )


@unittest.skipUnless(sys.platform == "win32", "Windows credential store")
class WindowsStoreTests(unittest.TestCase):
    """pair rotates the device token, so a store that cannot persist loses the
    only valid credential. Round-trip through the real HKCU store that Desk
    writes and jarvisctl reads, under a throwaway key."""

    KEY = r"Software\JarvisNano\DeskTest"

    def tearDown(self) -> None:
        import winreg
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, self.KEY)
        except OSError:
            pass

    def test_desk_store_round_trips_and_jarvisctl_reads_it(self) -> None:
        account = "jarvis-desk@device.local"
        with patch.object(desk, "WINDOWS_STORE_KEY", self.KEY):
            keychain = desk.MacOSKeychain()
            self.assertIsNone(keychain.load(account))
            keychain.store(account, "t" * 32)
            self.assertEqual(keychain.load(account), "t" * 32)
            self.assertIsNone(keychain.load("jarvis-desk@other.local"))
        source = Path(ctl.__file__).read_text(encoding="utf-8")
        # jarvisctl must read the very key Desk writes.
        self.assertIn('r"' + desk.WINDOWS_STORE_KEY + '"', source)
        no_keychain = patch.object(ctl.subprocess, "run",
                                   side_effect=FileNotFoundError("security"))
        real_open = __import__("winreg").OpenKey

        def open_test_key(root: Any, _key: str, *args: Any) -> Any:
            return real_open(root, self.KEY, *args)

        with patch.object(ctl, "host", return_value="device.local"), no_keychain, \
             patch.dict(ctl.os.environ, {}, clear=True), \
             patch("winreg.OpenKey", side_effect=open_test_key):
            self.assertEqual(ctl.pairing_token(), "t" * 32)


TWO_BY_TWO_PPM = b"P6\n2 2\n255\n" + bytes(range(12))


class JarvisCtlPortabilityTests(unittest.TestCase):
    """Pins for the paths that broke on Windows (2026-09-05): a missing host
    tracebacked, `screen` called sips blind, `reboot` assumed bin/python."""

    def test_missing_host_is_one_line_and_exit_2(self) -> None:
        err = io.StringIO()
        with patch.object(ctl.sys, "argv", ["jarvisctl", "status"]), \
             patch.dict(ctl.os.environ, {}, clear=True), \
             patch.object(ctl.sys, "stderr", err):
            with self.assertRaises(SystemExit) as raised:
                ctl.host()
        self.assertEqual(raised.exception.code, 2)
        self.assertEqual(err.getvalue().count("\n"), 1)
        self.assertIn("JARVIS_DEVICE_HOST", err.getvalue())

    def _fake_pil(self, marker: bytes) -> ModuleType:
        pil = ModuleType("PIL")

        def open_image(path: str) -> SimpleNamespace:
            self.assertTrue(Path(path).read_bytes().startswith(b"P6"))
            return SimpleNamespace(save=lambda out: Path(out).write_bytes(marker))

        pil.Image = SimpleNamespace(open=open_image)  # type: ignore[attr-defined]
        return pil

    def test_screen_converts_with_pil_and_never_calls_sips(self) -> None:
        def never(*_args: Any, **_kwargs: Any) -> None:
            raise AssertionError("sips must not be run when PIL is present")

        with tempfile.TemporaryDirectory() as tmp:
            out = str(Path(tmp) / "glass.png")
            with patch.dict(sys.modules, {"PIL": self._fake_pil(b"PNG")}), \
                 patch.object(ctl, "api", return_value=TWO_BY_TWO_PPM), \
                 patch.object(ctl.shutil, "which", return_value=None), \
                 patch.object(ctl.subprocess, "run", side_effect=never), \
                 patch.object(ctl.sys, "stdout", io.StringIO()):
                self.assertEqual(ctl.cmd_screen(out), 0)
            self.assertEqual(Path(out).read_bytes(), b"PNG")
            self.assertFalse(Path(out + ".ppm").exists())

    def test_screen_without_any_converter_keeps_the_ppm(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = str(Path(tmp) / "glass.png")
            with patch.dict(sys.modules, {"PIL": None}), \
                 patch.object(ctl, "api", return_value=TWO_BY_TWO_PPM), \
                 patch.object(ctl.shutil, "which", return_value=None), \
                 patch.object(ctl.sys, "stderr", io.StringIO()):
                self.assertEqual(ctl.cmd_screen(out), 1)
            self.assertFalse(Path(out).exists())
            self.assertEqual(Path(out + ".ppm").read_bytes(), TWO_BY_TWO_PPM)

    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow not installed")
    def test_screen_real_pil_writes_a_png(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = str(Path(tmp) / "glass.png")
            with patch.object(ctl, "api", return_value=TWO_BY_TWO_PPM), \
                 patch.object(ctl.shutil, "which", return_value=None), \
                 patch.object(ctl.sys, "stdout", io.StringIO()):
                self.assertEqual(ctl.cmd_screen(out), 0)
            self.assertTrue(Path(out).read_bytes().startswith(b"\x89PNG"))

    def test_esptool_python_resolves_windows_venv_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            scripts_dir = Path(tmp) / "scripts"
            scripts_dir.mkdir()
            with patch.object(ctl, "HERE", str(scripts_dir)):
                self.assertEqual(ctl.esptool_python(), sys.executable)
                win = Path(tmp) / ".build_tools" / "esptool" / "Scripts"
                win.mkdir(parents=True)
                (win / "python.exe").write_bytes(b"")
                self.assertEqual(Path(ctl.esptool_python()).resolve(),
                                 (win / "python.exe").resolve())
                posix = Path(tmp) / ".build_tools" / "esptool" / "bin"
                posix.mkdir(parents=True)
                (posix / "python").write_bytes(b"")
                self.assertEqual(Path(ctl.esptool_python()).resolve(),
                                 (posix / "python").resolve())


class SendCanvasTests(unittest.TestCase):
    def test_clear_attaches_paired_token_and_never_leaks_it(self) -> None:
        secret = "s" * 32
        captured: list[tuple[Any, int]] = []

        def open_request(request: Any, timeout: int = 30) -> FakeUrlResponse:
            captured.append((request, timeout))
            return FakeUrlResponse()

        env = {"JARVIS_PAIRING_TOKEN": secret, "JARVIS_DEVICE_HOST": "device.local"}
        out = io.StringIO()
        err = io.StringIO()
        with patch.object(ctl.subprocess, "run", side_effect=FileNotFoundError("security")), \
             patch.dict(ctl.os.environ, env, clear=True), \
             patch.object(send_canvas.urllib.request, "urlopen", side_effect=open_request), \
             patch.object(sys, "stdout", out), \
             patch.object(sys, "stderr", err):
            rc = send_canvas.main(["--host", "device.local", "--clear"])
        self.assertEqual(rc, 0)
        self.assertEqual(len(captured), 1)
        req = captured[0][0]
        self.assertEqual(req.full_url, "http://device.local/api/display/canvas?clear=1")
        self.assertEqual(req.get_header("X-jarvisnano-control"), "1")
        self.assertEqual(req.get_header("X-jarvisnano-token"), secret)
        self.assertNotIn(secret, out.getvalue())
        self.assertNotIn(secret, err.getvalue())

    def test_env_token_never_rides_to_different_host(self) -> None:
        secret = "s" * 32
        captured: list[Any] = []

        def open_request(request: Any, timeout: int = 30) -> FakeUrlResponse:
            captured.append(request)
            return FakeUrlResponse()

        env = {"JARVIS_PAIRING_TOKEN": secret, "JARVIS_DEVICE_HOST": "other.local"}
        with patch.object(ctl.subprocess, "run", side_effect=FileNotFoundError("security")), \
             patch.dict(ctl.os.environ, env, clear=True), \
             patch.object(send_canvas.urllib.request, "urlopen", side_effect=open_request), \
             patch.object(sys, "stdout", io.StringIO()):
            rc = send_canvas.main(["--host", "device.local", "--clear"])
        self.assertEqual(rc, 0)
        self.assertIsNone(captured[0].get_header("X-jarvisnano-token"))

    def test_takeover_claims_codex_lease_before_banner_push(self) -> None:
        secret = "t" * 32
        frame = b"\x00\x00" * (send_canvas.SIZE * send_canvas.SIZE)
        captured: list[Any] = []

        def open_request(request: Any, timeout: int = 30) -> FakeUrlResponse:
            captured.append(request)
            return FakeUrlResponse()

        out = io.StringIO()
        with patch.object(send_canvas, "load_pairing_token", return_value=secret), \
             patch.object(send_canvas, "banner_to_rgb565", return_value=frame) as mk, \
             patch.object(send_canvas.urllib.request, "urlopen", side_effect=open_request), \
             patch.object(sys, "stdout", out):
            rc = send_canvas.main([
                "--host", "https://device.local/",
                "--banner", "DEEP FOCUS",
                "--sub", "Back at 3:00 PM",
                "--accent", "violet",
                "--ttl", "45",
                "--takeover",
            ])
        self.assertEqual(rc, 0)
        mk.assert_called_once_with("DEEP FOCUS", sub="Back at 3:00 PM", accent="violet")
        self.assertEqual(len(captured), 2)
        lease_req, canvas_req = captured
        self.assertEqual(
            lease_req.full_url,
            "https://device.local/api/operator/lease?ttl=45&mode=codex",
        )
        self.assertEqual(lease_req.get_header("X-jarvisnano-token"), secret)
        self.assertEqual(lease_req.get_header("X-jarvisnano-control"), "1")
        self.assertEqual(
            canvas_req.full_url,
            "https://device.local/api/display/canvas?ttl=45000",
        )
        self.assertEqual(canvas_req.get_header("X-jarvisnano-token"), secret)
        self.assertEqual(canvas_req.get_header("X-jarvisnano-control"), "1")
        self.assertEqual(canvas_req.get_header("Content-type"), "application/octet-stream")
        self.assertEqual(len(canvas_req.data), send_canvas.SIZE * send_canvas.SIZE * 2)
        self.assertNotIn(secret, out.getvalue())

    def test_401_exits_cleanly_without_leaking_token(self) -> None:
        secret = "u" * 32
        err_stream = io.StringIO()
        http_401 = send_canvas.urllib.error.HTTPError(
            "http://device.local/api/display/canvas?clear=1",
            401,
            "Unauthorized",
            hdrs=None,  # type: ignore[arg-type]
            fp=None,
        )
        with patch.object(send_canvas, "load_pairing_token", return_value=secret), \
             patch.object(send_canvas.urllib.request, "urlopen", side_effect=http_401), \
             patch.object(sys, "stderr", err_stream):
            rc = send_canvas.main(["--host", "device.local", "--clear"])
        self.assertEqual(rc, 3)
        self.assertIn("401", err_stream.getvalue())
        self.assertNotIn(secret, err_stream.getvalue())

    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow not installed")
    def test_banner_renders_valid_466_rgb565_for_all_accents(self) -> None:
        expected_len = send_canvas.SIZE * send_canvas.SIZE * 2
        for accent in ("cyan", "violet", "amber", "mint", "crimson"):
            raw = send_canvas.banner_to_rgb565(
                "IN A MEETING", sub="Available at 2:30 PM", accent=accent
            )
            self.assertEqual(len(raw), expected_len)
            # Corner pixel (0, 0) outside the circular OLED mask must be pure black
            self.assertEqual(raw[0:2], b"\x00\x00")
            # Frame must contain visible luminous pixels
            self.assertTrue(any(b != 0 for b in raw))


class DeskHttpErrorDetailTests(unittest.TestCase):
    def _http_error(self, code: int, body: bytes) -> desk.urllib.error.HTTPError:
        return desk.urllib.error.HTTPError(
            "http://192.0.2.10/api/ota/upload",
            code,
            "Err",
            hdrs=None,  # type: ignore[arg-type]
            fp=io.BytesIO(body),
        )

    def test_post_binary_preserves_sanitized_503_ota_preflight_reason(self) -> None:
        secret = "s" * 32
        reason = b"connect power or charge battery above 30 percent (tok " + secret.encode() + b")"
        err = self._http_error(503, reason)
        client = desk.DeviceClient("http://192.0.2.10", 5.0, opener=lambda *_a, **_k: (_ for _ in ()).throw(err))
        with self.assertRaises(desk.DeskError) as raised:
            client.post_binary("/api/ota/upload", b"bin", token=secret)
        exc = raised.exception
        self.assertEqual(exc.http_status, 503)
        self.assertIn("connect power or charge battery above 30 percent", exc.message)
        self.assertNotIn(secret, exc.message)
        payload = exc.as_payload()
        self.assertIn("connect power or charge battery above 30 percent", payload["error"]["detail"])
        self.assertNotIn(secret, json.dumps(payload))

    def test_409_422_preserve_json_error_and_401_suppresses_body(self) -> None:
        err422 = self._http_error(422, b'{"ok":false,"error":"invalid bounded brain envelope"}')
        c422 = desk.DeviceClient("http://192.0.2.10", 5.0, opener=lambda *_a, **_k: (_ for _ in ()).throw(err422))
        with self.assertRaises(desk.DeskError) as r422:
            c422.post("/api/brain/inbox", {}, token="t" * 32)
        self.assertIn("invalid bounded brain envelope", r422.exception.message)

        err401 = self._http_error(401, b"sensitive-proxy-echo")
        c401 = desk.DeviceClient("http://192.0.2.10", 5.0, opener=lambda *_a, **_k: (_ for _ in ()).throw(err401))
        with self.assertRaises(desk.DeskError) as r401:
            c401.get("/api/cockpit", token="t" * 32)
        self.assertNotIn("sensitive-proxy-echo", r401.exception.message)
        self.assertIsNone(r401.exception.detail)


def _load_script(name: str, filename: str) -> ModuleType:
    path = Path(__file__).with_name(filename)
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


class ToolingFixesTests(unittest.TestCase):
    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow not installed")
    def test_rgb565_to_pil_decodes_exact_channels(self) -> None:
        import struct
        raw = struct.pack(">4H", 0xF800, 0x07E0, 0x001F, 0x0000)
        img = ctl.rgb565_to_pil(raw, 2, 2, big_endian=True)
        self.assertEqual(img.getpixel((0, 0)), (255, 0, 0))
        self.assertEqual(img.getpixel((1, 0)), (0, 255, 0))
        self.assertEqual(img.getpixel((0, 1)), (0, 0, 255))
        self.assertEqual(img.getpixel((1, 1)), (0, 0, 0))

    def test_live_device_get_bytes_attaches_pairing_token(self) -> None:
        live = _load_script("live_device_test", "live-device.py")
        secret = "k" * 32
        captured: list[Any] = []

        class Resp:
            status = 200
            headers = {"Content-Type": "application/octet-stream"}

            def __enter__(self) -> "Resp":
                return self

            def __exit__(self, *_a: Any) -> None:
                return None

            def read(self) -> bytes:
                return b"DATA"

        def fake_open(req: Any, timeout: float = 4.0) -> Resp:
            captured.append(req)
            return Resp()

        with patch.object(live, "load_pairing_token", return_value=secret), \
             patch.object(live.urllib.request, "urlopen", side_effect=fake_open):
            dev = live.Device("device.local", 4.0)
            code, _, body = dev.get_bytes("/api/display/snapshot.rgb565")
        self.assertEqual(code, 200)
        self.assertEqual(body, b"DATA")
        self.assertEqual(captured[0].get_header("X-jarvisnano-token"), secret)

    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow not installed")
    def test_live_device_convert_ppm_and_rgb565_with_pil(self) -> None:
        import struct
        live = _load_script("live_device_pil_test", "live-device.py")
        with tempfile.TemporaryDirectory() as tmp:
            ppm = Path(tmp) / "frame.ppm"
            ppm.write_bytes(TWO_BY_TWO_PPM)
            with patch.object(live.shutil, "which", return_value=None):
                png = live.convert_ppm_to_png(str(ppm))
            self.assertIsNotNone(png)
            assert png is not None
            self.assertTrue(Path(png).read_bytes().startswith(b"\x89PNG"))

            rgb = Path(tmp) / "frame.rgb565"
            rgb.write_bytes(struct.pack(">4H", 0xF800, 0x07E0, 0x001F, 0xFFFF))
            with patch.object(live.shutil, "which", return_value=None):
                png2 = live.convert_ppm_to_png(str(rgb))
            self.assertIsNotNone(png2)
            assert png2 is not None
            self.assertTrue(Path(png2).read_bytes().startswith(b"\x89PNG"))

    def test_gesture_doctor_parses_faces_including_orrery_and_reactor(self) -> None:
        gd = _load_script("gesture_doctor_test", "gesture-doctor.py")
        self.assertGreaterEqual(len(gd.FACES), 16)
        self.assertEqual(gd.FACES[14], "DIAL_ORRERY")
        self.assertEqual(gd.FACES[15], "DIAL_REACTOR")
        # Fallback list also includes both dials if header is absent
        with tempfile.TemporaryDirectory() as tmp:
            fallback = gd.faces_from_header(tmp)
            self.assertEqual(fallback[14], "DIAL_ORRERY")
            self.assertEqual(fallback[15], "DIAL_REACTOR")

    def test_usb_monitor_windows_pyserial_detect_and_no_reset_open(self) -> None:
        usb = _load_script("usb_monitor_test", "usb-monitor.py")
        events: list[str] = []

        class FakeSerial:
            def __init__(self, **kwargs: Any) -> None:
                self.kwargs = kwargs
                self.port = kwargs.get("port")
                self._dtr = True
                self._rts = True

            @property
            def dtr(self) -> bool:
                return self._dtr

            @dtr.setter
            def dtr(self, val: bool) -> None:
                self._dtr = val
                events.append(f"dtr={val}")

            @property
            def rts(self) -> bool:
                return self._rts

            @rts.setter
            def rts(self, val: bool) -> None:
                self._rts = val
                events.append(f"rts={val}")

            def open(self) -> None:
                events.append(f"open:{self.port}")

        fake_serial = ModuleType("serial")
        fake_serial.Serial = FakeSerial  # type: ignore[attr-defined]
        fake_tools = ModuleType("serial.tools")
        fake_lp = ModuleType("serial.tools.list_ports")
        fake_lp.comports = lambda: [  # type: ignore[attr-defined]
            SimpleNamespace(device="COM3", vid=0x1234, description="Other"),
            SimpleNamespace(device="COM7", vid=0x303A, description="USB JTAG/serial debug unit"),
        ]
        fake_tools.list_ports = fake_lp  # type: ignore[attr-defined]
        fake_serial.tools = fake_tools  # type: ignore[attr-defined]

        with patch.dict(sys.modules, {
            "serial": fake_serial,
            "serial.tools": fake_tools,
            "serial.tools.list_ports": fake_lp,
        }):
            detected = usb.detect_port_pyserial()
            self.assertEqual(detected, "COM7")
            ser = usb.open_pyserial("COM7", 115200)
        self.assertFalse(ser.kwargs["dsrdtr"])
        self.assertFalse(ser.kwargs["rtscts"])
        # DTR and RTS must be set False BEFORE open()
        open_idx = events.index("open:COM7")
        self.assertIn("dtr=False", events[:open_idx])
        self.assertIn("rts=False", events[:open_idx])

    @unittest.skipUnless(importlib.util.find_spec("PIL"), "Pillow not installed")
    def test_screens_rgb565_grab_and_markdown_table(self) -> None:
        import struct
        sc = _load_script("screens_test", "screens.py")
        raw = struct.pack(">4H", 0xF800, 0x07E0, 0x001F, 0x0000)
        hdrs = {"X-Jarvis-Display-Width": "2", "X-Jarvis-Display-Height": "2"}
        with patch.object(sc, "_req", return_value=(hdrs, raw)):
            img = sc.grab("device.local", retries=1)
        self.assertIsNotNone(img)
        assert img is not None
        self.assertEqual(img.size, (2, 2))
        self.assertEqual(img.getpixel((0, 0)), (255, 0, 0))

        md = sc.format_markdown_table([{
            "name": "HOME",
            "metrics": {
                "space": 0, "fps": 22, "frame_ms": 18.4,
                "top_layer": "hud", "top_layer_ms": 11.2,
                "layer_us": [0, 0, 11200, 1100, 0],
            },
        }])
        self.assertIn("| HOME | 0 | 22 | 18.4 | hud (11.2ms) |", md)


class CodexAndWorkbenchTests(unittest.TestCase):
    def test_sanitize_glass_text_enforces_firmware_rules(self) -> None:
        s = ctl.sanitize_glass_text("Deploy https://evil.test <script> — v3.6!", 24)
        self.assertLessEqual(len(s), 24)
        self.assertNotIn("http", s.lower())
        self.assertNotIn("<", s)
        self.assertIn("Deploy", s)

    def test_ask_consent_wait_allow_deny_and_timeout(self) -> None:
        posted: list[dict[str, Any]] = []
        session_id = "desk-test-session"

        def fake_post(path: str, payload: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
            posted.append(payload)
            return {"ok": True}

        # 1. Allow -> exit 0
        polls = iter([
            {"next_after": 10, "next_inbox_seq": 5, "events": []},
            {
                "next_after": 11,
                "next_inbox_seq": 6,
                "events": [{
                    "v": 1, "seq": 11, "type": "surface.action",
                    "session": session_id, "id": "placeholder",
                    "payload": {"action_id": "allow", "ts_ms": 999},
                }],
            },
        ])

        def fake_get_allow(path: str, timeout: int = 10) -> dict[str, Any]:
            item = next(polls)
            if posted and item["events"]:
                item["events"][0]["id"] = posted[0]["id"]
            return item

        out = io.StringIO()
        with patch.object(ctl, "_stable_session_id", return_value=session_id), \
             patch.object(ctl, "get_json", side_effect=fake_get_allow), \
             patch.object(ctl, "post_json", side_effect=fake_post), \
             patch.object(ctl.time, "sleep", lambda _: None), \
             patch.object(sys, "stdout", out):
            rc = ctl.cmd_ask(["Ship release?", "--consent", "--wait", "--json"])
        self.assertEqual(rc, 0)
        self.assertEqual(posted[0]["payload"]["kind"], "consent")
        self.assertEqual([a["id"] for a in posted[0]["payload"]["actions"]], ["deny", "allow"])
        self.assertEqual(json.loads(out.getvalue())["action_id"], "allow")

        # 2. Deny -> exit 1
        posted.clear()
        polls_deny = iter([
            {"next_after": 11, "next_inbox_seq": 6, "events": []},
            {
                "next_after": 12,
                "next_inbox_seq": 7,
                "events": [{
                    "v": 1, "seq": 12, "type": "surface.action",
                    "session": session_id, "id": "placeholder",
                    "payload": {"action_id": "deny", "ts_ms": 1000},
                }],
            },
        ])

        def fake_get_deny(path: str, timeout: int = 10) -> dict[str, Any]:
            item = next(polls_deny)
            if posted and item["events"]:
                item["events"][0]["id"] = posted[0]["id"]
            return item

        with patch.object(ctl, "_stable_session_id", return_value=session_id), \
             patch.object(ctl, "get_json", side_effect=fake_get_deny), \
             patch.object(ctl, "post_json", side_effect=fake_post), \
             patch.object(ctl.time, "sleep", lambda _: None), \
             patch.object(sys, "stdout", io.StringIO()):
            rc = ctl.cmd_ask(["Ship release?", "--consent", "--wait"])
        self.assertEqual(rc, 1)

        # 3. Timeout -> dismiss + exit 124
        posted.clear()
        ticks = iter([0.0, 0.5, 5.0])
        with patch.object(ctl, "_stable_session_id", return_value=session_id), \
             patch.object(ctl, "get_json", return_value={"next_after": 12, "next_inbox_seq": 7, "events": []}), \
             patch.object(ctl, "post_json", side_effect=fake_post), \
             patch.object(ctl.time, "monotonic", side_effect=lambda: next(ticks, 99.0)), \
             patch.object(ctl.time, "sleep", lambda _: None), \
             patch.object(sys, "stderr", io.StringIO()):
            rc = ctl.cmd_ask(["Timeout test", "--ttl", "1", "--wait"])
        self.assertEqual(rc, 124)
        self.assertEqual(posted[-1]["type"], "surface.dismiss")

    def test_run_streams_updates_agent_link_and_posts_result(self) -> None:
        api_calls: list[str] = []
        json_posts: list[tuple[str, dict[str, Any]]] = []
        rev = [1]

        def fake_api(path: str, method: str = "GET", body: bytes | None = None, timeout: int = 10) -> bytes:
            api_calls.append(f"{method} {path}")
            return b'{"ok":true}'

        def fake_get(path: str, timeout: int = 10) -> dict[str, Any]:
            if path == "/api/agent/link":
                return {"active": True, "next_revision": rev[0]}
            return {"next_after": 1, "next_inbox_seq": 3, "events": []}

        def fake_post(path: str, payload: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
            json_posts.append((path, payload))
            if path == "/api/agent/link":
                rev[0] += 1
            return {"ok": True}

        out = io.StringIO()
        with patch.object(ctl, "api", side_effect=fake_api), \
             patch.object(ctl, "get_json", side_effect=fake_get), \
             patch.object(ctl, "post_json", side_effect=fake_post), \
             patch.object(ctl, "_stable_session_id", return_value="desk-test"), \
             patch.object(sys, "stdout", out):
            rc = ctl.cmd_run([
                "--title", "UnitRun",
                "--takeover",
                "--",
                sys.executable, "-c",
                "print('[1/2] Compile'); print('[2/2] Verify')",
            ])
        self.assertEqual(rc, 0)
        self.assertIn("[1/2] Compile", out.getvalue())
        self.assertIn("POST /api/glass?screen=desk", api_calls)
        self.assertIn("POST /api/operator/lease?ttl=120&mode=codex", api_calls)
        self.assertIn("POST /api/operator/lease?release=1", api_calls)
        link_posts = [p for path, p in json_posts if path == "/api/agent/link"]
        self.assertGreaterEqual(len(link_posts), 2)
        self.assertEqual(link_posts[-1]["state"], "succeeded")
        self.assertEqual(link_posts[-1]["progress"], 100)
        # Firmware requires evidence items with keys "label" and "state"
        for item in link_posts[-1].get("evidence", []):
            self.assertEqual(set(item.keys()), {"label", "state"})
        result_posts = [p for path, p in json_posts if path == "/api/brain/inbox"]
        self.assertEqual(len(result_posts), 1)
        self.assertEqual(result_posts[0]["payload"]["kind"], "result")
        self.assertTrue(result_posts[0]["payload"]["title"].startswith("PASS:"))

    def test_agent_post_maps_aliases_and_validates_steps(self) -> None:
        posted: list[dict[str, Any]] = []
        with patch.object(ctl, "get_json", return_value={"next_revision": 4}), \
             patch.object(ctl, "post_json", side_effect=lambda _p, body, **_k: (posted.append(body), {"ok": True})[1]), \
             patch.object(sys, "stdout", io.StringIO()):
            rc = ctl.cmd_agent([
                "post", "--title", "Deploy", "--state", "complete",
                "--summary", "All green", "--step", "Tests:pass", "--step", "Flash:working",
            ])
        self.assertEqual(rc, 0)
        self.assertEqual(posted[0]["state"], "succeeded")
        self.assertEqual(posted[0]["progress"], 100)
        self.assertEqual(posted[0]["revision"], 4)
        self.assertEqual(
            posted[0]["evidence"],
            [{"label": "Tests", "state": "pass"}, {"label": "Flash", "state": "working"}],
        )

    def test_mode_and_inbox_commands(self) -> None:
        out = io.StringIO()
        with patch.object(ctl, "get_json", return_value={"active": True, "mode": "codex", "owner": "codex", "ttl_ms": 112000}), \
             patch.object(sys, "stdout", out):
            self.assertEqual(ctl.cmd_mode([]), 0)
        self.assertIn("active", out.getvalue())
        self.assertIn("112.0s", out.getvalue())

        notes_doc = {
            "count": 2,
            "max": 6,
            "notes": [
                {"id": 1, "text": "Buy milk", "done": False, "tag": "life"},
                {"id": 2, "text": "Refactor OTA", "done": False, "tag": "work"},
                {"id": 3, "text": "Old task", "done": True, "tag": "work"},
            ],
        }
        out2 = io.StringIO()
        with patch.object(ctl, "get_json", return_value=notes_doc), \
             patch.object(sys, "stdout", out2):
            self.assertEqual(ctl.cmd_inbox(["--tag", "work"]), 0)
        self.assertIn("Refactor OTA", out2.getvalue())
        self.assertNotIn("Buy milk", out2.getvalue())

        # Marking already-done note #3 must be idempotent (no toggle POST)
        toggled: list[str] = []
        with patch.object(ctl, "get_json", return_value=notes_doc), \
             patch.object(ctl, "api", side_effect=lambda p, *_a, **_k: (toggled.append(p), b'{"ok":true}')[1]), \
             patch.object(sys, "stdout", io.StringIO()):
            self.assertEqual(ctl.cmd_inbox(["--done", "3"]), 0)
            self.assertEqual(toggled, [])
            self.assertEqual(ctl.cmd_inbox(["--done", "2"]), 0)
            self.assertEqual(toggled, ["/api/notes?toggle=2"])

    def test_power_care_and_telemetry_render(self) -> None:
        called: list[str] = []
        resp = b'{"ok":true,"care_enabled":true,"holding":true,"cv_mv":4100,"icc_ma":500,"thermal_throttle":false,"thermal_pause":false}'
        out = io.StringIO()
        with patch.object(ctl, "api", side_effect=lambda p, *_a, **_k: (called.append(p), resp)[1]), \
             patch.object(sys, "stdout", out):
            self.assertEqual(ctl.cmd_power(["--care", "on"]), 0)
        self.assertEqual(called, ["/api/power/care?enable=1"])
        self.assertIn("holding=True", out.getvalue())

        text = ctl._render_power_view(
            {
                "battery": {"percent": 80, "millivolts": 4085, "charging": False, "usb_present": True},
                "pmic_die_c": 37.5, "vbus_mv": 5020, "vsys_mv": 4120,
                "pmic_i2c_err": 0, "imu_i2c_err": 1,
                "power_care": {"care_enabled": True, "holding": True, "cv_mv": 4100, "icc_ma": 500},
                "battery_analytics": {
                    "rate_valid": True, "rate_pct_hr": -4.2, "mv_per_hr": -38,
                    "eta_empty_min": 190, "sag_mv": 22, "time_on_bat_s": 600, "time_on_usb_s": 3600,
                },
            },
            {"decoded": {"direction": 0, "charge_phase": 4, "constant_current_milliamps": 500}, "registers": []},
            {"power": {"chip_c": 44, "cpu_mhz": 160}, "network": {"rssi": -54, "channel": 6, "ps_level": 1}},
        )
        self.assertIn("80%", text)
        self.assertIn("PMIC die=37.5", text)
        self.assertIn("phase=DONE", text)
        self.assertIn("ETA=3h 10m", text)

    def test_tail_overlap_dedupe_and_tasks_and_board(self) -> None:
        self.assertEqual(ctl._overlap_new_suffix("line1\nline2\n", "line2\nline3\n"), "line3\n")
        self.assertEqual(ctl._overlap_new_suffix("same\n", "same\n"), "")

        samples = iter([
            {"cpu_mhz": 160, "free_internal": 90000, "largest_internal_block": 32000, "total_runtime": 1000,
             "tasks": [{"name": "IDLE0", "prio": 0, "stack_free": 1200, "run": 800},
                       {"name": "tight_task", "prio": 5, "stack_free": 768, "run": 100}]},
            {"cpu_mhz": 160, "free_internal": 90000, "largest_internal_block": 32000, "total_runtime": 2000,
             "tasks": [{"name": "IDLE0", "prio": 0, "stack_free": 1200, "run": 1600},
                       {"name": "tight_task", "prio": 5, "stack_free": 768, "run": 250}]},
        ])
        out = io.StringIO()
        with patch.object(ctl, "get_json", side_effect=lambda *_a, **_k: next(samples)), \
             patch.object(ctl.time, "sleep", lambda _: None), \
             patch.object(sys, "stdout", out):
            self.assertEqual(ctl.cmd_tasks(["--cpu", "--interval", "0.2"]), 0)
        self.assertIn("IDLE0=80.0%", out.getvalue())
        self.assertIn("WARN (<1024B)", out.getvalue())

        board_doc = {
            "spi": {"ok": True, "hz": 40000000, "source": "macro"},
            "pwr_key": {"ok": True, "level": 1},
            "imu": {"ok": True, "who_am_i": 0x05, "temp_c": 31.2, "wom": True},
            "pmic": {"ok": True, "die_c": 36.1, "vbus_mv": 5010, "vsys_mv": 4100, "vbat_mv": 4080, "charge_cc_ma": 500},
            "touch": {"ok": True},
            "te": {"ok": True, "edges": 28, "hz": 59.9},
        }
        out_b = io.StringIO()
        with patch.object(ctl, "get_json", return_value=board_doc), \
             patch.object(sys, "stdout", out_b):
            self.assertEqual(ctl.cmd_board([]), 0)
        self.assertIn("40 MHz", out_b.getvalue())
        self.assertIn("0x05", out_b.getvalue())
        self.assertIn("die=36.1 C", out_b.getvalue())

    def test_crash_decode_matches_archived_elf(self) -> None:
        import hashlib
        with tempfile.TemporaryDirectory() as tmp:
            elf_bytes = b"\x7fELFfake-jarvis-elf"
            sha16 = hashlib.sha256(elf_bytes).hexdigest()[:16]
            elf_dir = Path(tmp) / ".build_logs" / "elfs"
            elf_dir.mkdir(parents=True)
            elf_file = elf_dir / f"{sha16}-deadbee.elf"
            elf_file.write_bytes(elf_bytes)

            crash_doc = {
                "reset": "panic", "dump_present": True, "dump_valid": True,
                "summary": {
                    "task": "render_task", "exc_pc": "0x42001234", "exc_cause": 28,
                    "exc_vaddr": "0x00000000", "panic_reason": "LoadProhibited",
                    "elf_sha": sha16, "bt_corrupted": False, "bt_depth": 1,
                    "bt": ["0x42005678"],
                },
            }
            out = io.StringIO()
            with patch.object(ctl, "REPO_ROOT", tmp), \
                 patch.object(ctl, "get_json", return_value=crash_doc), \
                 patch.object(ctl, "_run_addr2line", return_value=(0, "0x42001234: draw_strip at hud.c:42\n0x42005678: flush at disp.c:10\n", "")) as a2l, \
                 patch.object(sys, "stdout", out):
                rc = ctl.cmd_crash(["decode"])
            self.assertEqual(rc, 0)
            self.assertEqual(Path(a2l.call_args.args[0]).resolve(), elf_file.resolve())
            self.assertIn("SHA MATCH", out.getvalue())
            self.assertIn("draw_strip at hud.c:42", out.getvalue())

    def test_ship_preflight_blocks_low_battery_without_usb(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp) / "build"
            bdir.mkdir()
            (bdir / "jarvisrobot_v5.bin").write_bytes(b"A" * (300 * 1024))

            def fake_get(path: str, timeout: int = 10) -> dict[str, Any]:
                if path == "/api/sensors":
                    return {"battery": {"usb_present": False, "percent": 18}}
                if path == "/api/device/health":
                    return {"verdict": "ok", "memory": {"largest_internal": 32768}, "ota": {"running": "ota_0"}}
                raise AssertionError(path)

            err = io.StringIO()
            with patch.object(ctl, "REPO_ROOT", tmp), \
                 patch.object(ctl, "get_json", side_effect=fake_get), \
                 patch.object(sys, "stdout", io.StringIO()), \
                 patch.object(sys, "stderr", err):
                rc = ctl.cmd_ship(["--skip-tests", "--skip-build"])
            self.assertEqual(rc, 1)
            self.assertIn("18%", err.getvalue())
            self.assertIn("USB-C", err.getvalue())


class Glass37CinemaAndGistTests(unittest.TestCase):
    def test_transition_reads_cockpit_text_and_json(self) -> None:
        cockpit = {
            "display": {
                "transition": "shutter",
                "glass3_mode": "auto",
                "glass3": True,
                "gist_active": False,
            }
        }
        out = io.StringIO()
        with patch.object(ctl, "get_json", return_value=cockpit) as get_mock, \
             patch.object(sys, "stdout", out):
            self.assertEqual(ctl.cmd_transition([]), 0)
        get_mock.assert_called_once_with("/api/cockpit")
        self.assertIn("shutter", out.getvalue())
        self.assertIn("glass3_mode=auto", out.getvalue())

        out_json = io.StringIO()
        with patch.object(ctl, "get_json", return_value=cockpit), \
             patch.object(sys, "stdout", out_json):
            self.assertEqual(ctl.cmd_transition(["--json"]), 0)
        parsed = json.loads(out_json.getvalue())
        self.assertEqual(parsed["transition"], "shutter")
        self.assertEqual(parsed["glass3_mode"], "auto")
        self.assertTrue(parsed["glass3"])
        self.assertFalse(parsed["gist_active"])

    def test_transition_sets_each_mode_with_headers_and_json(self) -> None:
        for mode in ("shutter", "iris", "glide", "classic"):
            called: list[tuple[str, str, bytes | None]] = []
            resp_payload = {
                "ok": True,
                "glass3_mode": "auto",
                "glass3": True,
                "transition": mode,
            }
            resp_bytes = json.dumps(resp_payload).encode("utf-8")

            def fake_api(
                path: str,
                method: str = "GET",
                body: bytes | None = None,
                timeout: int = 10,
                _resp: bytes = resp_bytes,
                _called: list[tuple[str, str, bytes | None]] = called,
            ) -> bytes:
                _called.append((path, method, body))
                return _resp

            out = io.StringIO()
            with patch.object(ctl, "api", side_effect=fake_api), \
                 patch.object(sys, "stdout", out):
                self.assertEqual(ctl.cmd_transition([mode.upper()]), 0)
            self.assertEqual(called, [(f"/api/display/glass3?transition={mode}", "POST", b"")])
            self.assertIn(mode, out.getvalue())
            self.assertIn("glass3_mode=auto", out.getvalue())

            out_json = io.StringIO()
            with patch.object(ctl, "api", side_effect=fake_api), \
                 patch.object(sys, "stdout", out_json):
                self.assertEqual(ctl.cmd_transition([mode, "--json"]), 0)
            self.assertEqual(json.loads(out_json.getvalue()), resp_payload)

        # Verify end-to-end Control + Token headers via urllib
        captured: list[Any] = []
        secret = "t" * 32

        def fake_open(req: Any, timeout: float = 10) -> FakeResponse:
            captured.append(req)
            return FakeResponse({"ok": True, "glass3_mode": "auto", "glass3": True, "transition": "iris"})

        with patch.object(ctl, "host", return_value="device.local"), \
             patch.object(ctl, "pairing_token", return_value=secret), \
             patch.object(ctl.urllib.request, "urlopen", side_effect=fake_open), \
             patch.object(sys, "stdout", io.StringIO()):
            self.assertEqual(ctl.cmd_transition(["iris"]), 0)
        self.assertEqual(captured[0].full_url, "http://device.local/api/display/glass3?transition=iris")
        self.assertEqual(captured[0].get_method(), "POST")
        self.assertEqual(captured[0].get_header("X-jarvisnano-control"), "1")
        self.assertEqual(captured[0].get_header("X-jarvisnano-token"), secret)

    def test_transition_rejects_invalid_mode(self) -> None:
        err = io.StringIO()
        with patch.object(ctl, "api", side_effect=AssertionError("must not call api")), \
             patch.object(sys, "stderr", err):
            with self.assertRaises(SystemExit) as raised:
                ctl.cmd_transition(["warp"])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("invalid choice", err.getvalue())

    def test_gist_present_sanitizes_bounds_clamps_ttl_and_posts_json(self) -> None:
        posted: list[tuple[str, dict[str, Any]]] = []

        def fake_post(path: str, payload: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
            posted.append((path, payload))
            return {"ok": True, "active": True}

        # Sanitization, length bounds (23 / 43), degree symbol, URL/script strip, low TTL clamp
        out = io.StringIO()
        with patch.object(ctl, "post_json", side_effect=fake_post), \
             patch.object(sys, "stdout", out):
            rc = ctl.cmd_gist([
                "--kind", "weather",
                "--eyebrow", "  TOMORROW FORECAST — EXTRA LONG EYEBROW  ",
                "--hero", "71°F https://evil.test <script>bad</script> — CLEAR SKIES ALL AFTERNOON LONG",
                "--detail", "RAIN FROM 3:30 PM " + ("X" * 40),
                "--detail2", "WIND 12 MPH " + ("Y" * 40),
                "--footer", "LOCAL SENSOR FOOTER EXTRA TEXT",
                "--ttl", "1",
            ])
        self.assertEqual(rc, 0)
        self.assertEqual(posted[0][0], "/api/display/gist")
        body = posted[0][1]
        self.assertEqual(body["kind"], "weather")
        self.assertLessEqual(len(body["eyebrow"]), 23)
        self.assertLessEqual(len(body["footer"]), 23)
        self.assertLessEqual(len(body["hero"]), 43)
        self.assertLessEqual(len(body["detail"]), 43)
        self.assertLessEqual(len(body["detail2"]), 43)
        self.assertIn("71*F", body["hero"])
        self.assertNotIn("http", body["hero"].lower())
        self.assertNotIn("script", body["hero"].lower())
        self.assertNotIn("bad", body["hero"].lower())
        self.assertEqual(body["ttl_ms"], 4000)
        self.assertIn("GIST [weather]:", out.getvalue())
        self.assertIn("ttl=4s", out.getvalue())

        # High TTL clamp (>60 -> 60000 ms), default kind='answer', empty optionals stay "", --json
        posted.clear()
        out_json = io.StringIO()
        with patch.object(ctl, "post_json", side_effect=fake_post), \
             patch.object(sys, "stdout", out_json):
            rc = ctl.cmd_gist([
                "--eyebrow", "PRICE",
                "--hero", "$96,420",
                "--ttl", "999",
                "--json",
            ])
        self.assertEqual(rc, 0)
        body2 = posted[0][1]
        self.assertEqual(body2["kind"], "answer")
        self.assertEqual(body2["hero"], "$96,420")
        self.assertEqual(body2["detail"], "")
        self.assertEqual(body2["detail2"], "")
        self.assertEqual(body2["footer"], "")
        self.assertEqual(body2["ttl_ms"], 60000)
        parsed = json.loads(out_json.getvalue())
        self.assertTrue(parsed["ok"])
        self.assertTrue(parsed["active"])
        self.assertEqual(parsed["hero"], "$96,420")
        self.assertEqual(parsed["ttl_ms"], 60000)

    def test_gist_clear_posts_clear_query_with_headers_and_json(self) -> None:
        captured: list[Any] = []
        secret = "g" * 32

        def fake_open(req: Any, timeout: float = 10) -> FakeResponse:
            captured.append(req)
            return FakeResponse({"ok": True, "active": False})

        out = io.StringIO()
        with patch.object(ctl, "host", return_value="device.local"), \
             patch.object(ctl, "pairing_token", return_value=secret), \
             patch.object(ctl.urllib.request, "urlopen", side_effect=fake_open), \
             patch.object(sys, "stdout", out):
            self.assertEqual(ctl.cmd_gist(["clear"]), 0)
        self.assertEqual(captured[0].full_url, "http://device.local/api/display/gist?clear=1")
        self.assertEqual(captured[0].get_method(), "POST")
        self.assertEqual(captured[0].get_header("X-jarvisnano-control"), "1")
        self.assertEqual(captured[0].get_header("X-jarvisnano-token"), secret)
        self.assertIn("cleared", out.getvalue())

        out_json = io.StringIO()
        with patch.object(ctl, "host", return_value="device.local"), \
             patch.object(ctl, "pairing_token", return_value=secret), \
             patch.object(ctl.urllib.request, "urlopen", side_effect=fake_open), \
             patch.object(sys, "stdout", out_json):
            self.assertEqual(ctl.cmd_gist(["clear", "--json"]), 0)
        self.assertEqual(json.loads(out_json.getvalue()), {"ok": True, "active": False})

    def test_gist_presets_and_field_overrides(self) -> None:
        expected_kinds = {
            "price": "number",
            "event": "event",
            "timer": "timer",
            "note": "note",
            "briefing": "briefing",
        }
        for preset, exp_kind in expected_kinds.items():
            posted: list[dict[str, Any]] = []
            with patch.object(ctl, "post_json", side_effect=lambda _p, body, **_k: (posted.append(body), {"ok": True})[1]), \
                 patch.object(sys, "stdout", io.StringIO()):
                self.assertEqual(ctl.cmd_gist(["--preset", preset]), 0)
            body = posted[0]
            self.assertEqual(body["kind"], exp_kind)
            self.assertTrue(1 <= len(body["eyebrow"]) <= 23)
            self.assertTrue(1 <= len(body["hero"]) <= 43)
            self.assertLessEqual(len(body["detail"]), 43)
            self.assertLessEqual(len(body["detail2"]), 43)
            self.assertLessEqual(len(body["footer"]), 23)
            self.assertEqual(body["ttl_ms"], 20000)

        # Override preset fields and TTL
        posted_ov: list[dict[str, Any]] = []
        out = io.StringIO()
        with patch.object(ctl, "post_json", side_effect=lambda _p, body, **_k: (posted_ov.append(body), {"ok": True})[1]), \
             patch.object(sys, "stdout", out):
            rc = ctl.cmd_gist(["--preset", "price", "--hero", "$102,500", "--ttl", "45", "--json"])
        self.assertEqual(rc, 0)
        self.assertEqual(posted_ov[0]["kind"], "number")
        self.assertEqual(posted_ov[0]["eyebrow"], "BITCOIN")
        self.assertEqual(posted_ov[0]["hero"], "$102,500")
        self.assertEqual(posted_ov[0]["ttl_ms"], 45000)
        self.assertEqual(json.loads(out.getvalue())["hero"], "$102,500")

    def test_gist_argument_validation_errors(self) -> None:
        bad_invocations = [
            [],
            ["--eyebrow", "ONLY_EYEBROW"],
            ["--hero", "ONLY_HERO"],
            ["--kind", "invalid", "--eyebrow", "A", "--hero", "B"],
            ["--preset", "unknown"],
            ["bogus"],
            ["clear", "--preset", "price"],
            ["clear", "--hero", "Nope"],
            ["clear", "--ttl", "10"],
            ["--eyebrow", "   ", "--hero", "Valid"],
            ["--eyebrow", "Valid", "--hero", "https://only-url.test"],
        ]
        for argv in bad_invocations:
            err = io.StringIO()
            with patch.object(ctl, "post_json", side_effect=AssertionError("must not post")), \
                 patch.object(ctl, "api", side_effect=AssertionError("must not call api")), \
                 patch.object(sys, "stderr", err):
                with self.assertRaises(SystemExit, msg=f"expected exit 2 for {argv}") as raised:
                    ctl.cmd_gist(argv)
            self.assertEqual(raised.exception.code, 2, msg=f"bad exit code for {argv}")

    def test_main_dispatch_and_ask_consent_help_text(self) -> None:
        with patch.object(ctl.sys, "argv", ["jarvisctl", "--host", "device.local", "transition", "glide"]), \
             patch.object(ctl, "cmd_transition", return_value=0) as m_tr:
            self.assertEqual(ctl.main(), 0)
        m_tr.assert_called_once_with(["glide"])

        with patch.object(ctl.sys, "argv", ["jarvisctl", "--host", "device.local", "gist", "clear"]), \
             patch.object(ctl, "cmd_gist", return_value=0) as m_gist:
            self.assertEqual(ctl.main(), 0)
        m_gist.assert_called_once_with(["clear"])

        help_out = io.StringIO()
        with patch.object(sys, "stdout", help_out):
            with self.assertRaises(SystemExit) as raised:
                ctl.cmd_ask(["--help"])
        self.assertEqual(raised.exception.code, 0)
        help_text = help_out.getvalue()
        self.assertIn("0.8 s", help_text)
        self.assertIn("leaf-green", help_text)
        self.assertIn("1-tap", help_text)


class NanoReefAquariumTests(unittest.TestCase):
    SAMPLE_TANK: dict[str, Any] = {
        "ok": True,
        "active": True,
        "biome": "reef",
        "coral_level": 2,
        "coral_xp": 45,
        "pearls": 4,
        "algae": 5,
        "total_feeds": 12,
        "fish": [
            {
                "idx": 0,
                "name": "NEMO",
                "species": "clownfish",
                "stage": "juvenile",
                "xp": 42,
                "hunger": 18,
                "happiness": 92,
                "age_hours": 6,
            },
            {
                "idx": 1,
                "name": "NOVA",
                "species": "neon_tetra",
                "stage": "elder",
                "xp": 265,
                "hunger": 10,
                "happiness": 100,
                "age_hours": 30,
            },
        ],
    }

    def test_aquarium_status_default_and_explicit_text_and_json(self) -> None:
        for argv in ([], ["status"], ["STATUS"]):
            out = io.StringIO()
            with patch.object(ctl, "get_json", return_value=self.SAMPLE_TANK) as get_mock, \
                 patch.object(sys, "stdout", out):
                self.assertEqual(ctl.cmd_aquarium(argv), 0)
            get_mock.assert_called_once_with("/api/aquarium")
            txt = out.getvalue()
            self.assertIn("AQUARIUM [ACTIVE]", txt)
            self.assertIn("biome=reef (Tropical Reef)", txt)
            self.assertIn("coral=Lv 2 (45 XP)", txt)
            self.assertIn("pearls=4", txt)
            self.assertIn("clarity=95%", txt)
            self.assertIn("feeds=12", txt)
            self.assertIn("fish=2/8", txt)
            for col in ("#", "NAME", "SPECIES", "STAGE", "XP", "HUNGER", "HAPPY", "AGE"):
                self.assertIn(col, txt)
            self.assertIn("NEMO", txt)
            self.assertIn("clownfish", txt)
            self.assertIn("JUVENILE", txt)
            self.assertIn("18%", txt)
            self.assertIn("92%", txt)
            self.assertIn("6h", txt)
            self.assertIn("NOVA", txt)
            self.assertIn("ELDER", txt)
            self.assertIn("1d 6h", txt)

        for argv in (["--json"], ["status", "--json"], ["--json", "status"]):
            out_json = io.StringIO()
            with patch.object(ctl, "get_json", return_value=self.SAMPLE_TANK), \
                 patch.object(sys, "stdout", out_json):
                self.assertEqual(ctl.cmd_aquarium(argv), 0)
            self.assertEqual(json.loads(out_json.getvalue()), self.SAMPLE_TANK)

        empty_tank = {
            "ok": True, "active": False, "biome": "koi",
            "coral_level": 1, "coral_xp": 0, "pearls": 0, "algae": 0,
            "total_feeds": 0, "fish": [],
        }
        out_empty = io.StringIO()
        with patch.object(ctl, "get_json", return_value=empty_tank), \
             patch.object(sys, "stdout", out_empty):
            self.assertEqual(ctl.cmd_aquarium([]), 0)
        self.assertIn("AQUARIUM [STANDBY]", out_empty.getvalue())
        self.assertIn("clarity=100%", out_empty.getvalue())
        self.assertIn("(tank empty)", out_empty.getvalue())
        self.assertEqual(ctl._fmt_age_hours(48), "2d")
        self.assertEqual(ctl._fmt_age_hours(None), "-")

    def test_aquarium_simple_actions_and_auth_headers(self) -> None:
        resp_bytes = json.dumps(self.SAMPLE_TANK).encode("utf-8")
        for action in ("open", "close", "feed", "clean", "shake"):
            called: list[tuple[str, str, bytes | None]] = []

            def fake_api(
                path: str,
                method: str = "GET",
                body: bytes | None = None,
                timeout: int = 10,
                _called: list[tuple[str, str, bytes | None]] = called,
            ) -> bytes:
                _called.append((path, method, body))
                return resp_bytes

            out = io.StringIO()
            with patch.object(ctl, "api", side_effect=fake_api), \
                 patch.object(sys, "stdout", out):
                self.assertEqual(ctl.cmd_aquarium([action.upper()]), 0)
            self.assertEqual(called, [(f"/api/aquarium?action={action}", "POST", b"")])
            self.assertIn("AQUARIUM [ACTIVE]", out.getvalue())

            out_json = io.StringIO()
            with patch.object(ctl, "api", side_effect=fake_api), \
                 patch.object(sys, "stdout", out_json):
                self.assertEqual(ctl.cmd_aquarium([action, "--json"]), 0)
            self.assertEqual(json.loads(out_json.getvalue()), self.SAMPLE_TANK)

        # End-to-end header check via urllib (Control + Token)
        captured: list[Any] = []
        secret = "q" * 32

        def fake_open(req: Any, timeout: float = 10) -> FakeResponse:
            captured.append(req)
            return FakeResponse(self.SAMPLE_TANK)

        with patch.object(ctl, "host", return_value="device.local"), \
             patch.object(ctl, "pairing_token", return_value=secret), \
             patch.object(ctl.urllib.request, "urlopen", side_effect=fake_open), \
             patch.object(sys, "stdout", io.StringIO()):
            self.assertEqual(ctl.cmd_aquarium(["feed"]), 0)
        self.assertEqual(captured[0].full_url, "http://device.local/api/aquarium?action=feed")
        self.assertEqual(captured[0].get_method(), "POST")
        self.assertEqual(captured[0].get_header("X-jarvisnano-control"), "1")
        self.assertEqual(captured[0].get_header("X-jarvisnano-token"), secret)

        # Non-ok response returns exit code 1
        with patch.object(ctl, "api", return_value=b'{"ok":false,"fish":[]}'), \
             patch.object(sys, "stdout", io.StringIO()):
            self.assertEqual(ctl.cmd_aquarium(["shake"]), 1)

    def test_aquarium_biome_valid_and_invalid(self) -> None:
        for biome in ("reef", "abyss", "koi"):
            called: list[tuple[str, str, bytes | None]] = []
            payload = {**self.SAMPLE_TANK, "biome": biome}
            resp_bytes = json.dumps(payload).encode("utf-8")

            def fake_api(
                path: str,
                method: str = "GET",
                body: bytes | None = None,
                timeout: int = 10,
                _called: list[tuple[str, str, bytes | None]] = called,
                _resp: bytes = resp_bytes,
            ) -> bytes:
                _called.append((path, method, body))
                return _resp

            out = io.StringIO()
            with patch.object(ctl, "api", side_effect=fake_api), \
                 patch.object(sys, "stdout", out):
                self.assertEqual(ctl.cmd_aquarium(["biome", biome.upper()]), 0)
            self.assertEqual(called, [(f"/api/aquarium?action=biome&biome={biome}", "POST", b"")])
            self.assertIn(f"biome={biome}", out.getvalue())

            out_json = io.StringIO()
            with patch.object(ctl, "api", side_effect=fake_api), \
                 patch.object(sys, "stdout", out_json):
                self.assertEqual(ctl.cmd_aquarium(["biome", biome, "--json"]), 0)
            self.assertEqual(json.loads(out_json.getvalue())["biome"], biome)

        err = io.StringIO()
        with patch.object(ctl, "api", side_effect=AssertionError("must not call api")), \
             patch.object(sys, "stderr", err):
            with self.assertRaises(SystemExit) as raised:
                ctl.cmd_aquarium(["biome", "swamp"])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("invalid choice", err.getvalue())

    def test_aquarium_add_all_species_and_url_encoding(self) -> None:
        resp_bytes = json.dumps(self.SAMPLE_TANK).encode("utf-8")
        all_species = (
            "clownfish", "blue_tang", "neon_tetra", "golden_koi",
            "mandarin", "jellyfish", "manta_ray", "seahorse",
        )
        for sp in all_species:
            called: list[tuple[str, str]] = []
            with patch.object(ctl, "api", side_effect=lambda p, m="GET", *_a, _c=called: (_c.append((p, m)), resp_bytes)[1]), \
                 patch.object(sys, "stdout", io.StringIO()):
                self.assertEqual(ctl.cmd_aquarium(["add", sp.upper()]), 0)
            self.assertEqual(called, [(f"/api/aquarium?action=add&species={sp}", "POST")])

        # With name, URL encoding, whitespace-only name, and action-keyword name
        cases = [
            (["add", "clownfish", "NOVA"], "/api/aquarium?action=add&species=clownfish&name=NOVA"),
            (["add", "blue_tang", "SIR FIN & CO"], "/api/aquarium?action=add&species=blue_tang&name=SIR%20FIN%20%26%20CO"),
            (["add", "seahorse", "   "], "/api/aquarium?action=add&species=seahorse"),
            (["add", "clownfish", "FEED", "--json"], "/api/aquarium?action=add&species=clownfish&name=FEED"),
        ]
        for argv, expected_path in cases:
            called = []
            out = io.StringIO()
            with patch.object(ctl, "api", side_effect=lambda p, m="GET", *_a, _c=called: (_c.append((p, m)), resp_bytes)[1]), \
                 patch.object(sys, "stdout", out):
                self.assertEqual(ctl.cmd_aquarium(argv), 0)
            self.assertEqual(called, [(expected_path, "POST")])
            if "--json" in argv:
                self.assertEqual(json.loads(out.getvalue()), self.SAMPLE_TANK)

        err = io.StringIO()
        with patch.object(ctl, "api", side_effect=AssertionError("must not call api")), \
             patch.object(sys, "stderr", err):
            with self.assertRaises(SystemExit) as raised:
                ctl.cmd_aquarium(["add", "shark", "JAWS"])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("invalid choice", err.getvalue())

    def test_aquarium_remove_valid_and_bounds(self) -> None:
        resp_bytes = json.dumps(self.SAMPLE_TANK).encode("utf-8")
        for idx in (0, 7):
            called: list[tuple[str, str]] = []
            out = io.StringIO()
            with patch.object(ctl, "api", side_effect=lambda p, m="GET", *_a, _c=called: (_c.append((p, m)), resp_bytes)[1]), \
                 patch.object(sys, "stdout", out):
                self.assertEqual(ctl.cmd_aquarium(["remove", str(idx), "--json"]), 0)
            self.assertEqual(called, [(f"/api/aquarium?action=remove&idx={idx}", "POST")])
            self.assertEqual(json.loads(out.getvalue()), self.SAMPLE_TANK)

        for bad in (["remove", "-1"], ["remove", "8"], ["remove", "abc"], ["bogus"]):
            err = io.StringIO()
            with patch.object(ctl, "api", side_effect=AssertionError("must not call api")), \
                 patch.object(sys, "stderr", err):
                with self.assertRaises(SystemExit, msg=f"expected exit 2 for {bad}") as raised:
                    ctl.cmd_aquarium(bad)
            self.assertEqual(raised.exception.code, 2)

    def test_aquarium_main_dispatch_and_help(self) -> None:
        with patch.object(ctl.sys, "argv", ["jarvisctl", "--host", "device.local", "aquarium", "feed"]), \
             patch.object(ctl, "cmd_aquarium", return_value=0) as m_aq:
            self.assertEqual(ctl.main(), 0)
        m_aq.assert_called_once_with(["feed"])

        help_out = io.StringIO()
        with patch.object(sys, "stdout", help_out):
            with self.assertRaises(SystemExit) as raised:
                ctl.cmd_aquarium(["--help"])
        self.assertEqual(raised.exception.code, 0)
        help_text = help_out.getvalue()
        for word in ("status", "open", "close", "feed", "clean", "shake", "biome", "add", "remove"):
            self.assertIn(word, help_text)
        self.assertNotIn("%%", help_text)

        clean_help = io.StringIO()
        with patch.object(sys, "stdout", clean_help):
            with self.assertRaises(SystemExit) as raised2:
                ctl.cmd_aquarium(["clean", "--help"])
        self.assertEqual(raised2.exception.code, 0)
        self.assertIn("100% clarity", clean_help.getvalue())


if __name__ == "__main__":
    unittest.main()
