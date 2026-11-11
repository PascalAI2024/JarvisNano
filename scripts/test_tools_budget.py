#!/usr/bin/env python3
"""The execute_tool byte budget's build chain, and the probe that mirrors it.

    python3 -m unittest scripts/test_tools_budget.py

What is pinned here: the generated JR_TOOLS_EXEC_JS is not stale against
components/jr_tools/js/exec_tool.js, the program fits JR_TOOLS_CODE_CAP beside
the largest path and arguments, the budget number follows from
JR_TOOLS_RESPONSE_CAP, the wording that sends "the board" to delegated_tasks,
and scripts/tools-template-probe.py (request bodies, verdicts, and that it
never prints the URL, the key or the body).

NOT covered here: running the JavaScript. That needs Node, which the host-tests
container does not have; components/jr_tools/host/test_exec_budget.mjs does it
and runs in CI and by hand.
"""
import importlib.util
import io
import json
import os
import re
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.dont_write_bytecode = True


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = load("gen_exec_js", "gen-exec-js.py")
probe = load("tools_template_probe", "tools-template-probe.py")
TEMPLATES_C = (ROOT / "components/jr_tools/src/jr_tools_templates.c").read_text(encoding="utf-8")
MAIN_C = (ROOT / "main/main.c").read_text(encoding="utf-8")


def declaration(name):
    """The description text of one function declaration in main.c."""
    m = re.search(r'\.name = "%s",\s*\.description =\s*((?:"(?:[^"\\]|\\.)*"\s*)+)' % name, MAIN_C)
    assert m, name
    return probe.text_of(probe.tokens(m.group(1)))


class GeneratedProgram(unittest.TestCase):
    def test_c_file_is_not_stale(self):
        current = (ROOT / "components/jr_tools/src/jr_tools_templates.c").read_text(encoding="utf-8")
        start, stop = current.index(gen.BEGIN), current.index(gen.END)
        self.assertEqual(current[start:stop + len(gen.END)], gen.block(),
                         "run scripts/gen-exec-js.py")

    def test_program_leaves_room_for_the_largest_path_and_args(self):
        response_cap, args_cap, code_cap = gen.caps()
        program = gen.program()
        self.assertEqual(program.count("%s"), 2)
        worst = len(program) - 4 + gen.PATH_MAX + args_cap + 1
        self.assertLessEqual(worst, code_cap)
        self.assertGreater(len(program), 1000)

    def test_a_program_that_cannot_fit_is_refused(self):
        response_cap, args_cap, _ = gen.caps()
        with mock.patch.object(gen, "caps", return_value=(response_cap, args_cap, 3000)):
            with self.assertRaises(SystemExit):
                gen.program()

    def test_budget_follows_from_the_response_slot(self):
        response_cap, _, _ = gen.caps()
        self.assertEqual(gen.budget(), response_cap - len('{"result":') - 1 - 1 - gen.SLACK)
        self.assertIn("const MAX=%d;" % gen.budget(), gen.program())
        # What the device prints, {"result":<value>}, fits when value <= budget.
        self.assertLess(len('{"result":') + gen.budget() + 1 + 1, response_cap)

    def test_the_program_carries_the_budget_logic(self):
        program = gen.program()
        for needle in ("truncated:true", "total:r.length", "total_chars", "TIERS",
                       "JSON.stringify", "hint:HINT"):
            self.assertIn(needle, program)


class Wording(unittest.TestCase):
    def test_board_questions_go_to_delegated_tasks(self):
        execute = declaration("execute_tool")
        self.assertIn("delegated_tasks", execute)
        self.assertNotIn("createWorkItem", execute)
        self.assertNotIn("coordination.", execute)
        tasks = declaration("delegated_tasks")
        for phrase in ("work board", "what is on the board", "the tasks",
                       "what is queued, running or done", "never search_tools or execute_tool"):
            self.assertIn(phrase, tasks)


class Probe(unittest.TestCase):
    def test_reads_the_whole_template_table(self):
        templates, allow, deny, exec_js = probe.load_firmware()
        self.assertEqual(sorted(templates), sorted([
            "recall_memory", "remember", "current_time", "weather_glance",
            "search_tools", "delegate_task", "delegated_tasks", "board_poll"]))
        self.assertIn("websearch", allow)
        self.assertIn("delete", deny)
        self.assertTrue(templates["delegated_tasks"]["board"])
        self.assertTrue(exec_js.startswith("const P=%s;let A=%s;"))

    def test_named_templates_match_the_c_test_vectors(self):
        program, target, _ = probe.build_code("remember", '{"note":"Pascal\'s tea, (green & hot)"}')
        self.assertEqual(program, 'const b="Pascal\'s tea, (green & hot)";'
                         'return await jarvis.memory.capture({type:"note",'
                         'title:b.slice(0,60),body:b,'
                         'tags:["jarvisnano","voice"],source:"jarvisnano",'
                         'confidence:"confirmed"})')
        program, _, _ = probe.build_code("recall_memory", '{"query":"release plan"}')
        self.assertTrue(program.startswith(
            'const r=await jarvis.memory.search({query:"release plan",area:"all",limit:5});'))
        program, _, _ = probe.build_code("delegated_tasks")
        self.assertTrue(program.startswith('const PJ="jarvisnano-desk";'))
        program, _, _ = probe.build_code("current_time")
        self.assertIn('"America/New_York"', program)

    def test_execute_tool_matches_the_firmware_policy(self):
        args = json.dumps({"tool": "websearch", "args_json": '{"query":"x y"}'})
        program, target, nested = probe.build_code("execute_tool", args)
        self.assertTrue(program.startswith('const P=["websearch"];let A={"query":"x y"};'))
        self.assertEqual((target, nested), ("websearch", {"query": "x y"}))
        program, _, _ = probe.build_code("execute_tool", json.dumps(
            {"tool": "memory.search", "args_json": "{}"}))
        self.assertTrue(program.startswith('const P=["memory","search"];let A={};'))
        for refused in ("dokploy.project.all", "coordination.deleteWorkItem", "websearchx"):
            program, _, _ = probe.build_code("execute_tool", json.dumps(
                {"tool": refused, "args_json": "{}"}))
            self.assertEqual(program, 'return {error:"that tool is not permitted from the device"}',
                             refused)
        for bad in ('{"tool":"x"}', '{"tool":"a b","args_json":"{}"}',
                    '{"tool":"websearch","args_json":"[]"}', "not json"):
            with self.assertRaises(probe.Refused):
                probe.build_code("execute_tool", bad)
        with self.assertRaises(probe.Refused):
            probe.build_code("no_such_tool", "{}")

    def test_request_bodies(self):
        args = json.dumps({"tool": "weather", "args_json": '{"latitude":1,"longitude":2}'})
        legacy = json.loads(probe.build_request("execute_tool", args, "https://example.test/act"))
        self.assertEqual(list(legacy), ["code"])
        self.assertIn('["weather"]', legacy["code"])
        typed = json.loads(probe.build_request(
            "execute_tool", args, "https://example.test/device/v1/invoke"))
        self.assertEqual(typed, {"tool": "weather", "args": {"latitude": 1, "longitude": 2}})

    def test_judge_mirrors_the_device(self):
        parses, fits, keys = probe.judge(b'{"result":{"tools":[],"has_more":false}}')
        self.assertEqual((parses, fits, keys), (True, True, ["tools", "has_more"]))
        self.assertEqual(probe.judge(b"<html>")[:2], (False, False))
        big = json.dumps({"result": {"text": "x" * 3200}}).encode()
        self.assertEqual(probe.judge(big)[:2], (True, False))   # over the 3 KB slot
        huge = json.dumps({"result": {"text": "x" * 9000}}).encode()
        self.assertEqual(probe.judge(huge)[:2], (True, False))  # over the 8 KB body cap

    def run_main(self, argv, env, response=None, error=None):
        out = io.StringIO()

        class Resp:
            status = 200

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def read(self):
                return response

        def fake_urlopen(req, timeout=None):
            if error:
                raise error
            return Resp()

        with mock.patch.dict(os.environ, env, clear=False), \
                mock.patch.object(probe.urllib.request, "urlopen", fake_urlopen), \
                redirect_stdout(out):
            rc = probe.main(argv)
        return rc, out.getvalue()

    def test_never_prints_the_url_the_key_or_the_body(self):
        env = {"JARVIS_PROBE_URL": "https://secret-host.example.test/act",
               "JARVIS_PROBE_KEY": "key-0123456789"}
        body = b'{"result":{"headline":"SECRET-BODY-TEXT"}}'
        rc, text = self.run_main(["execute_tool", json.dumps(
            {"tool": "websearch", "args_json": '{"query":"x"}'})], env, response=body)
        self.assertEqual(rc, 0)
        self.assertIn("http_status: 200", text)
        self.assertIn("body_bytes: %d" % len(body), text)
        self.assertIn("body_parses: yes", text)
        self.assertIn("fits_device_caps: yes", text)
        self.assertIn("top_level_keys: headline", text)
        for secret in ("secret-host", "key-0123456789", "SECRET-BODY-TEXT", "https://"):
            self.assertNotIn(secret, text)
        rc, text = self.run_main(["delegated_tasks"], env,
                                 error=OSError("failed to reach https://secret-host.example.test"))
        self.assertEqual(rc, 1)
        self.assertEqual(text.strip(), "error: OSError")

    def test_needs_its_environment(self):
        with mock.patch.dict(os.environ, {"JARVIS_PROBE_URL": "", "JARVIS_PROBE_KEY": ""}):
            rc, text = self.run_main(["delegated_tasks"], {})
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
