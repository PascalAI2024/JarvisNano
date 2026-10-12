"""Offline production-path checks for the display-only Deck bridge."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import sys
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

SPEC = importlib.util.spec_from_file_location(
    "deck_nano", Path(__file__).with_name("deck-nano.py"))
assert SPEC and SPEC.loader
nano = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(nano)
REAL_OPEN = urllib.request.OpenerDirector.open


class Response:
    def __init__(self, value=None, *, raw=None, status=200):
        self.raw = json.dumps(value).encode() if raw is None else raw
        self.status = self.code = status
        self.msg = "OK"
        self.read_sizes = []

    def read(self, size):
        self.read_sizes.append(size)
        return self.raw[:size]

    def info(self):
        return {}

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False


class Opener:
    def __init__(self, *replies):
        self.replies = list(replies)
        self.calls = []

    def open(self, req, *, timeout):
        self.calls.append((req, timeout))
        if not self.replies:
            raise AssertionError("unexpected HTTP request")
        value = self.replies.pop(0)
        if isinstance(value, BaseException):
            raise value
        return value if isinstance(value, Response) else Response(value)


def worker(key="one", state="working", **extra):
    return {"id": key, "state": state, "alive": True, **extra}


def supported(**extra):
    return {"active": False, "next_revision": 8, "deck_supported": True,
            "publish_fence": nano.PUBLISH_FENCE, **extra}


def client(*replies):
    result = nano.DeviceClient("http://nano.invalid", "test-token")
    result.http.opener = Opener(*replies)
    return result


class OfflineCase(unittest.TestCase):
    def setUp(self):
        self.network = patch.object(urllib.request.OpenerDirector, "open",
                                    side_effect=AssertionError("live network forbidden"))
        self.network.start()
        self.addCleanup(self.network.stop)


class ProjectionTests(OfflineCase):
    def test_three_workers_and_waiting_owner(self):
        deck = nano.project([worker(str(n)) for n in range(3)], [
            {"id": "t", "state": "waiting-owner", "question": "fixture"}], source="live")
        self.assertEqual((deck["live"], deck["running"], len(deck["workers"])), (3, 3, 3))
        self.assertTrue(deck["needs_owner"])
        self.assertEqual(deck["owner_count"], 1)
        nano.validate_projection(deck)

    def test_raw_content_and_identity_never_forwarded(self):
        secret = "PRIVATE-PATH-QUESTION-REASON-SESSION"
        deck = nano.project([worker(secret, name=secret, title=secret, cwd=secret,
                                    needs={"detail": secret}, because={"reason": secret})],
                            [{"id": secret, "text": secret, "repo": secret,
                              "state": "waiting-owner", "question": secret}], source="live")
        raw = json.dumps(nano.make_payload(deck, 1))
        self.assertNotIn(secret, raw)
        self.assertEqual(deck["workers"][0]["label"], "WORKER 1")

    def test_dead_orchestrator_idle_finished_and_unknown_not_running(self):
        agents = [worker("dead", alive=False), worker("owner", role="orchestrator")]
        agents += [worker(s, s) for s in ("idle", "done", "failed", "exited", "novel")]
        deck = nano.project(agents, [], source="live")
        self.assertEqual(deck["live"], 0)
        self.assertEqual(deck["running"], 0)

    def test_only_literal_alive_true_counts(self):
        for alive in (False, None, 1, "true"):
            with self.subTest(alive=alive):
                self.assertEqual(nano.project([worker(alive=alive)], [], source="live")["live"], 0)

    def test_stale_work_is_stalled(self):
        deck = nano.project([worker(because={"stale": True})], [], source="live")
        self.assertEqual(deck["workers"][0]["state"], "stalled")
        self.assertEqual(deck["running"], 0)

    def test_attention_worker_joins_waiting_task_once(self):
        deck = nano.project([worker(state="needs-you", taskId="t")], [
            {"id": "t", "state": "waiting-owner", "question": "fixture"}], source="live")
        self.assertEqual(deck["owner_count"], 1)

    def test_worker_attention_without_task(self):
        deck = nano.project([worker(state="needs-you")], [], source="live")
        self.assertTrue(deck["needs_owner"])
        self.assertEqual(deck["running"], 0)

    def test_review_and_old_question_are_not_attention(self):
        deck = nano.project([], [{"id": "t", "state": "review", "question": "old"}], source="live")
        self.assertFalse(deck["needs_owner"])

    def test_invalid_waiting_owner_fails_closed(self):
        for question in (None, "", "  ", 42):
            with self.subTest(question=question), self.assertRaises(nano.BridgeError):
                nano.project([], [{"id": "t", "state": "waiting-owner",
                                   "question": question}], source="live")

    def test_four_node_cap_and_actual_counts(self):
        agents = [worker(str(n)) for n in range(7)]
        deck = nano.project(agents, [], source="live")
        self.assertEqual((len(deck["workers"]), deck["live"], deck["running"], deck["remaining"]),
                         (4, 7, 7, 3))
        nano.validate_projection(deck)

    def test_reordering_does_not_change_projection(self):
        agents = [worker("b", "stalled"), worker("a")]
        self.assertEqual(nano.project(agents, [], source="live"),
                         nano.project(agents[::-1], [], source="live"))

    def test_bad_root_duplicate_identity_and_oversize_fail_closed(self):
        for agents in ({}, [None], [worker(), worker()], [worker(str(n)) for n in range(257)]):
            with self.subTest(agents_type=type(agents)), self.assertRaises(nano.BridgeError):
                nano.project(agents, [], source="live")

    def test_fixture_is_explicit_and_has_three_workers(self):
        deck = nano.FixtureSource().sample()
        self.assertEqual((deck["source"], deck["live"], deck["running"]), ("fixture", 3, 3))
        self.assertTrue(deck["needs_owner"])
        self.assertEqual(nano.make_payload(deck, 1)["title"], "DECK FIXTURE")

    def test_attention_fixture_keeps_real_worker_states_and_counts(self):
        source = Mock(sample=Mock(return_value=nano.project(
            [worker(str(n)) for n in range(3)], [], source="live")))
        deck = nano.AttentionFixtureSource(source).sample()
        self.assertEqual((deck["source"], deck["live"], deck["running"]), ("fixture", 3, 3))
        self.assertTrue(deck["needs_owner"])
        self.assertEqual(deck["owner_count"], 1)
        nano.validate_projection(deck)
        source.sample.assert_called_once_with()

    def test_attention_fixture_does_not_mask_source_failure(self):
        source = Mock(sample=Mock(side_effect=nano.BridgeError("deckd unavailable")))
        with self.assertRaises(nano.BridgeError):
            nano.AttentionFixtureSource(source).sample()


class TransportTests(OfflineCase):
    def source(self, *replies):
        source = nano.DeckSource()
        source.http.opener = Opener(*replies)
        return source

    def test_only_fixed_loopback_three_get_routes(self):
        source = self.source({"ok": True}, [worker()], [])
        source.sample()
        requests = [req for req, _ in source.http.opener.calls]
        self.assertEqual([req.full_url for req in requests], [
            "http://127.0.0.1:7717/api/status", "http://127.0.0.1:7717/api/agents",
            "http://127.0.0.1:7717/api/tasks"])
        self.assertTrue(all(req.get_method() == "GET" and req.data is None for req in requests))
        self.assertTrue(all(req.get_header("X-jarvisnano-token") is None for req in requests))
        self.assertTrue(all(timeout == 2.0 for _, timeout in source.http.opener.calls))

    def test_proxy_environment_cannot_intercept_loopback(self):
        seen = []
        def http_open(handler, req):
            seen.append(req.full_url)
            return Response({"ok": True})
        with patch.dict(os.environ, {"http_proxy": "http://proxy.invalid:8888", "no_proxy": ""}), \
                patch.object(urllib.request.ProxyHandler, "proxy_open",
                             side_effect=AssertionError("proxy used")), \
                patch.object(urllib.request.HTTPHandler, "http_open", http_open):
            source = nano.DeckSource()
            source.http.opener.open = lambda req, timeout: REAL_OPEN(
                source.http.opener, req, timeout=timeout)
            self.assertEqual(source.http.request("GET", "/api/status"), {"ok": True})
        self.assertEqual(seen, ["http://127.0.0.1:7717/api/status"])

    def test_redirect_refused_before_following(self):
        req = urllib.request.Request("http://127.0.0.1:7717/api/status")
        with self.assertRaisesRegex(nano.BridgeError, "redirect refused"):
            nano.NoRedirect().redirect_request(req, None, 302, "", {}, "http://other.invalid/")

    def test_arbitrary_route_and_local_writes_refused(self):
        source = self.source()
        for method, path in (("POST", "/api/tasks"), ("GET", "/api/tasks/t"),
                             ("GET", "http://other.invalid/"), ("GET", "/api/needs")):
            with self.subTest(method=method, path=path), self.assertRaises(nano.BridgeError):
                source.http.request(method, path)
        self.assertFalse(source.http.opener.calls)

    def test_unhealthy_daemon_stops_before_content_reads(self):
        for status in ({"ok": False}, {}, {"ok": 1}, None):
            source = self.source(status)
            with self.assertRaises(nano.BridgeError):
                source.sample()
            self.assertEqual(len(source.http.opener.calls), 1)

    def test_response_cap_is_enforced(self):
        response = Response(raw=b" " * (nano.MAX_JSON_BYTES + 1))
        source = self.source(response)
        with self.assertRaisesRegex(nano.BridgeError, "response too large"):
            source.sample()
        self.assertEqual(response.read_sizes, [nano.MAX_JSON_BYTES + 1])

    def test_bad_json_duplicate_nan_and_utf8_fail_closed(self):
        for raw in (b"not-json", b'{"ok":true,"ok":false}', b'{"value":NaN}', b"\xff"):
            with self.subTest(raw=raw), self.assertRaises(nano.BridgeError):
                self.source(Response(raw=raw)).sample()

    def test_http_error_does_not_disclose_body_url_or_token(self):
        error = urllib.error.HTTPError("http://private.invalid/", 403, "PRIVATE", {}, io.BytesIO(b"SECRET"))
        with self.assertRaises(nano.BridgeError) as raised:
            self.source(error).sample()
        self.assertEqual(str(raised.exception), "HTTP 403")

    def test_timeout_is_bounded_diagnostic(self):
        with self.assertRaisesRegex(nano.BridgeError, "connection unavailable"):
            self.source(TimeoutError("PRIVATE-HOST")).sample()

    def test_device_host_cannot_contain_credentials_path_or_query(self):
        for host in ("", "http://u:p@nano.invalid", "http://nano.invalid/path",
                     "http://nano.invalid?q=1", "http://nano.invalid#frag", "http://nano.invalid\nX: bad"):
            with self.subTest(host=host), self.assertRaises(nano.BridgeError):
                nano.device_origin(host)

    def test_device_write_routes_cannot_change_privacy_lease_or_answer(self):
        device = client()
        for path in ("/api/operator/lease", "/api/voice/control", "/api/tasks/t/reply",
                     "/api/display/canvas", "/api/agent/link?clear=1"):
            with self.subTest(path=path), self.assertRaises(nano.BridgeError):
                device.http.request("POST", path, {})
        self.assertFalse(device.http.opener.calls)


class FenceTests(OfflineCase):
    def test_capability_and_exact_fence_required(self):
        for values in ({"deck_supported": False}, {"deck_supported": 1},
                       {"publish_fence": None}, {"publish_fence": "advisory"}):
            device = client({"active": False}, supported(**values))
            with self.subTest(values=values), self.assertRaises(nano.BridgeError):
                device.next_revision()
            self.assertTrue(all(req.get_method() == "GET" for req, _ in device.http.opener.calls))

    def test_old_firmware_without_capability_is_read_only(self):
        device = client({"active": False}, {"active": False, "next_revision": 1})
        with self.assertRaises(nano.BridgeError):
            device.next_revision()
        self.assertEqual(len(device.http.opener.calls), 2)

    def test_operator_lease_missing_or_nonliteral_false_blocks(self):
        for lease in ({}, None, {"active": True}, {"active": 0}, {"active": "false"}):
            device = client(lease)
            with self.subTest(lease=lease), self.assertRaises(nano.BridgeError):
                device.next_revision()
            self.assertEqual(len(device.http.opener.calls), 1)

    def test_foreign_task_blocks_publication(self):
        device = client({"active": False}, supported(active=True, task_id="owner-task"))
        with self.assertRaisesRegex(nano.BridgeError, "another task"):
            device.next_revision()

    def test_existing_bridge_task_may_refresh(self):
        device = client({"active": False}, supported(active=True, task_id="deck-face"))
        self.assertEqual(device.next_revision(), 8)

    def test_invalid_ownership_and_revision_fail_closed(self):
        for values in ({"active": 0}, {"next_revision": True}, {"next_revision": 0},
                       {"next_revision": -1}, {"next_revision": 0xFFFFFFFF}):
            device = client({"active": False}, supported(**values))
            with self.subTest(values=values), self.assertRaises(nano.BridgeError):
                device.next_revision()

    def test_actual_publish_uses_generic_payload_paired_control_headers(self):
        device = client({"ok": True, "accepted": True})
        device.publish(nano.FixtureSource().sample(), 8)
        req, _ = device.http.opener.calls[0]
        payload = json.loads(req.data)
        self.assertEqual((req.get_method(), req.full_url), ("POST", "http://nano.invalid/api/agent/link"))
        self.assertEqual(req.get_header("X-jarvisnano-token"), "test-token")
        self.assertEqual(req.get_header("X-jarvisnano-control"), "1")
        self.assertEqual(req.get_header("Content-type"), "application/json")
        self.assertEqual((payload["task_id"], payload["revision"], payload["ttl_s"]), ("deck-face", 8, 30))
        self.assertEqual(payload["progress"], 0)
        self.assertLess(len(req.data), nano.MAX_POST_BYTES)

    def test_unaccepted_response_cannot_count_as_success(self):
        for reply in ({"ok": True}, {"ok": False, "accepted": True}, {"ok": 1, "accepted": True}):
            with self.subTest(reply=reply), self.assertRaises(nano.BridgeError):
                client(reply).publish(nano.FixtureSource().sample(), 1)

    def test_unknown_raw_field_and_spoofed_label_cannot_be_published(self):
        for field, value in (("raw_question", "PRIVATE"), ("live", True)):
            deck = nano.FixtureSource().sample()
            deck[field] = value
            device = client()
            with self.subTest(field=field), self.assertRaises(nano.BridgeError):
                device.publish(deck, 1)
            self.assertFalse(device.http.opener.calls)
        deck = nano.FixtureSource().sample()
        deck["workers"][0]["label"] = "PRIVATE"
        with self.assertRaises(nano.BridgeError):
            client().publish(deck, 1)

    def test_inconsistent_counts_refused(self):
        for field, value in (("running", 0), ("remaining", 4), ("owner_count", 0)):
            deck = nano.FixtureSource().sample()
            deck[field] = value
            with self.subTest(field=field), self.assertRaises(nano.BridgeError):
                nano.make_payload(deck, 1)


class BridgeTests(OfflineCase):
    def bridge(self):
        self.now = 0.0
        source = Mock(sample=Mock(return_value=nano.FixtureSource().sample()))
        device = Mock(next_revision=Mock(return_value=8), publish=Mock())
        return nano.Bridge(source, device, clock=lambda: self.now)

    def test_unchanged_not_sent_until_freshness_heartbeat(self):
        bridge = self.bridge()
        self.assertEqual(bridge.step(), "published fixture")
        self.now = 9.9
        self.assertEqual(bridge.step(), "unchanged")
        self.now = 10
        self.assertEqual(bridge.step(), "published fixture")
        self.assertEqual(bridge.device.publish.call_count, 2)
        self.assertEqual(bridge.source.sample.call_count, 3)

    def test_changed_projection_sent_immediately(self):
        bridge = self.bridge()
        bridge.step()
        bridge.source.sample.return_value = nano.project([worker()], [], source="live")
        self.now = 1
        self.assertEqual(bridge.step(), "published live")
        self.assertEqual(bridge.device.publish.call_count, 2)

    def test_source_failure_never_refreshes_cached_workers(self):
        bridge = self.bridge()
        bridge.step()
        self.now = 100
        bridge.source.sample.side_effect = nano.BridgeError("deckd unavailable")
        with self.assertRaises(nano.BridgeError):
            bridge.step()
        self.assertEqual(bridge.device.publish.call_count, 1)
        self.assertEqual(bridge.last_success, 0)

    def test_idle_expires_without_clear_and_returning_work_publishes(self):
        bridge = self.bridge()
        bridge.step()
        bridge.source.sample.return_value = nano.project([], [], source="live")
        self.assertIn("expires", bridge.step())
        self.assertEqual(bridge.device.publish.call_count, 1)
        bridge.source.sample.return_value = nano.FixtureSource().sample()
        self.assertEqual(bridge.step(), "published fixture")
        self.assertEqual(bridge.device.publish.call_count, 2)

    def test_attention_without_workers_still_publishes(self):
        bridge = self.bridge()
        bridge.source.sample.return_value = nano.project([], [
            {"id": "t", "state": "waiting-owner", "question": "fixture"}], source="live")
        self.assertEqual(bridge.step(), "published live")

    def test_atomic_server_race_rejection_repreflights_next_attempt(self):
        error = urllib.error.HTTPError("http://nano.invalid/api/agent/link", 409, "race", {}, io.BytesIO())
        device = client({"active": False}, supported(), error,
                        {"active": False}, supported(next_revision=10), {"ok": True, "accepted": True})
        bridge = nano.Bridge(nano.FixtureSource(), device, clock=lambda: 0)
        with self.assertRaises(nano.BridgeError):
            bridge.step()
        self.assertIsNone(bridge.last_projection)
        self.assertEqual(bridge.step(), "published fixture")
        calls = [req for req, _ in device.http.opener.calls]
        self.assertEqual([req.get_method() for req in calls], ["GET", "GET", "POST"] * 2)
        self.assertEqual(json.loads(calls[-1].data)["revision"], 10)

    def test_backoff_is_bounded_and_never_clears(self):
        bridge = Mock(step=Mock(side_effect=[nano.BridgeError("offline")] * 6))
        delays = []
        def sleep(delay):
            delays.append(delay)
            if len(delays) == 6:
                raise KeyboardInterrupt
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(KeyboardInterrupt):
            nano.run(bridge, sleep=sleep)
        self.assertEqual(delays, [5, 10, 20, 40, 60, 60])
        self.assertEqual(bridge.mock_calls, [unittest.mock.call.step()] * 6)

    def test_single_failure_exits_without_retry(self):
        bridge = Mock(step=Mock(side_effect=nano.BridgeError("offline")))
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(nano.run(bridge, once=True, sleep=Mock()), 3)
        self.assertEqual(bridge.step.call_count, 1)

    def test_success_resets_backoff(self):
        bridge = Mock(step=Mock(side_effect=[nano.BridgeError("offline"), "published live",
                                            nano.BridgeError("offline")]))
        delays = []
        def sleep(delay):
            delays.append(delay)
            if len(delays) == 3:
                raise KeyboardInterrupt
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(KeyboardInterrupt):
            nano.run(bridge, sleep=sleep)
        self.assertEqual(delays, [5, 5, 5])


class CredentialAndCliTests(OfflineCase):
    def test_jarvisctl_host_bound_environment_lookup_reused(self):
        token = "x" * 40
        argv = sys.argv
        with patch.dict(os.environ, {"JARVIS_DEVICE_HOST": "nano.invalid", "JARVIS_PAIRING_TOKEN": token}, clear=True), \
                patch("subprocess.run", side_effect=FileNotFoundError), patch.object(sys, "platform", "linux"):
            self.assertEqual(nano.load_credentials("nano.invalid"), ("http://nano.invalid", token))
            with self.assertRaises(nano.BridgeError):
                nano.load_credentials("other.invalid")
        self.assertIs(sys.argv, argv)

    def test_fixture_preview_never_loads_credentials_or_opens_http(self):
        output = io.StringIO()
        with patch.object(nano, "load_credentials", side_effect=AssertionError("credential access")), \
                contextlib.redirect_stdout(output):
            self.assertEqual(nano.main(["--fixture", "--preview"]), 0)
        deck = json.loads(output.getvalue())
        self.assertEqual((deck["source"], deck["live"], deck["needs_owner"]), ("fixture", 3, True))

    def test_live_preview_has_no_device_calls(self):
        source = Mock(sample=Mock(return_value=nano.project([worker()], [], source="live")))
        with patch.object(nano, "DeckSource", return_value=source), \
                patch.object(nano, "load_credentials", side_effect=AssertionError("credential access")), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(nano.main(["--preview"]), 0)

    def test_attention_fixture_preview_uses_live_source_not_synthetic_workers(self):
        source = Mock(sample=Mock(return_value=nano.project([worker()], [], source="live")))
        with patch.object(nano, "DeckSource", return_value=source), \
                patch.object(nano, "FixtureSource", side_effect=AssertionError("synthetic workers")), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(nano.main(["--needs-owner-fixture", "--preview"]), 0)
        deck = json.loads(output.getvalue())
        self.assertEqual((deck["source"], deck["live"], deck["needs_owner"]), ("fixture", 1, True))

    def test_missing_host_is_bounded_error(self):
        with patch.dict(os.environ, {}, clear=True), contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(nano.main(["--fixture", "--once"]), 3)
        self.assertEqual(output.getvalue(), "deck-nano: --host or JARVIS_DEVICE_HOST required\n")

    def test_once_cli_connects_only_after_credentials(self):
        device = Mock(next_revision=Mock(return_value=1), publish=Mock())
        with patch.object(nano, "load_credentials", return_value=("http://nano.invalid", "test-token")) as lookup, \
                patch.object(nano, "DeviceClient", return_value=device) as constructor, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(nano.main(["--fixture", "--host", "nano.invalid", "--once"]), 0)
        lookup.assert_called_once_with("nano.invalid")
        constructor.assert_called_once_with("http://nano.invalid", "test-token")
        device.publish.assert_called_once()


if __name__ == "__main__":
    unittest.main()
