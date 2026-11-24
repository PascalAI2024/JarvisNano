#!/usr/bin/env python3
"""Probe the tool gateway with the exact request the firmware would send.

    JARVIS_PROBE_URL=... JARVIS_PROBE_KEY=... \\
        scripts/tools-template-probe.py TOOL [ARGS_JSON]

TOOL is a name the model can call (recall_memory, delegated_tasks, ...) or
execute_tool, whose ARGS_JSON is what the model sends it:
'{"tool":"websearch","args_json":"{\\"query\\":\\"x\\"}"}'. The program text is
taken from components/jr_tools/src/jr_tools_templates.c itself (the template
table, the allowlist and the generated execute_tool program), so it cannot
drift from the firmware. An endpoint containing /device/v1/invoke gets the
typed body, anything else the legacy {"code":...} body, as in jr_tools.c.

It prints only: the HTTP status, the body size in bytes, whether the body
parses as JSON, whether the answer fits the device caps, and the top-level
keys. It never prints the URL, the key or the body, and an error prints the
exception class only. With --dry-run it builds the request and prints the
sizes, and needs no environment variables and no network.
"""
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATES_C = ROOT / "components/jr_tools/src/jr_tools_templates.c"
TOOLS_C = ROOT / "components/jr_tools/src/jr_tools.c"
HEADER = ROOT / "components/jr_tools/include/jr_tools/jr_tools.h"
NAME_CAP = 48
TIMEOUT_S = 35


class Refused(Exception):
    """The firmware would not have sent this call (its status name)."""


def define(path, name):
    m = re.search(r"#define\s+%s\s+(\d+)U?" % name, path.read_text(encoding="utf-8"))
    if not m:
        sys.exit("tools-template-probe: %s not found in %s" % (name, path.name))
    return int(m.group(1))


# ---- reading the firmware's own tables out of the C source -----------------

_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"', "'": "'", "0": "\0"}


def tokens(text):
    """Strings (decoded), words and punctuation of C text, comments dropped."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            i = text.index("*/", i) + 2
        elif text.startswith("//", i):
            i = text.index("\n", i) if "\n" in text[i:] else n
        elif c == '"':
            j, buf = i + 1, []
            while text[j] != '"':
                if text[j] == "\\":
                    buf.append(_ESCAPES[text[j + 1]])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            out.append(("str", "".join(buf)))
            i = j + 1
        elif c == "'":
            i = text.index("'", i + (3 if text[i + 1] == "\\" else 2)) + 1
            out.append(("word", "'"))
        elif c.isalnum() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            out.append(("word", text[i:j]))
            i = j
        elif c in "{},=;":
            out.append(("p", c))
            i += 1
        else:
            i += 1
    return out


def brace_body(toks, start):
    """Tokens inside the {...} that opens at toks[start]."""
    depth = 0
    for k in range(start, len(toks)):
        if toks[k] == ("p", "{"):
            depth += 1
        elif toks[k] == ("p", "}"):
            depth -= 1
            if depth == 0:
                return toks[start + 1:k]
    raise ValueError("unbalanced braces")


def groups(toks):
    """Split a brace body into its top-level {...} groups' token lists."""
    out, depth, cur = [], 0, []
    for t in toks:
        if t == ("p", "{"):
            depth += 1
            if depth == 1:
                cur = []
                continue
        if t == ("p", "}"):
            depth -= 1
            if depth == 0:
                out.append(cur)
                continue
        if depth >= 1:
            cur.append(t)
    return out


def fields(group):
    out, cur = [], []
    for t in group:
        if t == ("p", ","):
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        out.append(cur)
    return out


def text_of(field):
    return "".join(v for k, v in field if k == "str")


def load_firmware():
    src = TEMPLATES_C.read_text(encoding="utf-8")
    toks = tokens(src)

    templates = {}
    for g in groups(_templates_body(toks)):
        f = fields(g)
        templates[text_of(f[0])] = {
            "arg": text_of(f[1]),
            "max_len": int(re.sub(r"\D", "", f[2][0][1])),
            "optional": f[3][0][1] == "true",
            "prefix": text_of(f[4]),
            "suffix": text_of(f[5]) if len(f) > 5 else "",
            "board": f[6][0][1] == "true" if len(f) > 6 else False,
        }
    allow = [v for k, v in _array_values(toks, "allow")]
    deny = [v for k, v in _array_values(toks, "deny_prefix")]
    m = re.search(r"#define JR_TOOLS_EXEC_JS \\\r?\n\s*(\"(?:[^\"\\]|\\.)*\")", src)
    if not m:
        sys.exit("tools-template-probe: JR_TOOLS_EXEC_JS not found")
    exec_js = text_of(tokens(m.group(1)))
    return templates, allow, deny, exec_js


def _templates_body(toks):
    for k in range(len(toks) - 3):
        if toks[k][1] == "s_templates":
            while toks[k] != ("p", "{"):
                k += 1
            return brace_body(toks, k)
    sys.exit("tools-template-probe: s_templates not found")


def _array_values(toks, name):
    for k in range(len(toks)):
        if toks[k] == ("word", name):
            while toks[k] != ("p", "{"):
                k += 1
            return [t for t in brace_body(toks, k) if t[0] == "str"]
    sys.exit("tools-template-probe: %s not found" % name)


# ---- the firmware's jr_tools_build_code, in Python --------------------------

def json_literal(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def build_code(tool, args_json="{}", project="jarvisnano-desk"):
    templates, allow, deny, exec_js = load_firmware()
    code_cap = define(TOOLS_C, "JR_TOOLS_CODE_CAP")
    args_cap = define(HEADER, "JR_TOOLS_ARGS_CAP")
    text = args_json if args_json else "{}"
    try:
        args = json.loads(text)
    except ValueError:
        raise Refused("invalid_args")
    if tool == "execute_tool":
        if (not isinstance(args, dict) or set(args) != {"tool", "args_json"}
                or not isinstance(args["tool"], str)
                or not isinstance(args["args_json"], str)):
            raise Refused("invalid_args")
        target = args["tool"]
        if (not 0 < len(target) < NAME_CAP
                or not re.fullmatch(r"[A-Za-z0-9._-]+", target)):
            raise Refused("invalid_args")
        try:
            nested = json.loads(args["args_json"])
        except ValueError:
            raise Refused("invalid_args")
        if not isinstance(nested, dict):
            raise Refused("invalid_args")
        ok = any(target == a or target.startswith(a + ".") for a in allow)
        last = target.rsplit(".", 1)[-1]
        if ok and any(last.lower().startswith(d) for d in deny):
            ok = False
        if not ok:
            return 'return {error:"that tool is not permitted from the device"}', target, nested
        program = exec_js.replace("%s", "\0", 2)
        program = (program.replace("\0", json_literal(target.split(".")), 1)
                   .replace("\0", json_literal(nested), 1))
        if len(program) >= code_cap:
            raise Refused("too_large")
        return program, target, nested
    t = templates.get(tool)
    if t is None:
        raise Refused("unknown_tool")
    if len(text) >= args_cap:
        raise Refused("too_large")
    if not isinstance(args, dict) or any(k != t["arg"] for k in args):
        raise Refused("invalid_args")
    value = args.get(t["arg"])
    if t["arg"] in args and not isinstance(value, str):
        raise Refused("invalid_args")
    prelude = 'const PJ="%s";' % project if t["board"] else ""
    if not value:
        if tool == "current_time":
            value = "America/New_York"
        elif t["optional"]:
            return prelude + t["prefix"] + t["suffix"], tool, None
        else:
            raise Refused("invalid_args")
    if len(value.encode("utf-8")) > t["max_len"]:
        raise Refused("invalid_args")
    program = prelude + t["prefix"] + json_literal(value) + t["suffix"]
    if len(program) >= code_cap:
        raise Refused("too_large")
    return program, tool, None


def build_request(tool, args_json, url, project="jarvisnano-desk"):
    """The POST body jr_tools.c would send to `url`."""
    program, target, nested = build_code(tool, args_json, project)
    if "/device/v1/invoke" in url:
        if tool == "execute_tool":
            body = {"tool": target, "args": nested}
        else:
            body = {"tool": tool, "args": json.loads(args_json or "{}")}
    else:
        body = {"code": program}
    return json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


# ---- judging the answer the way the device would ----------------------------

def judge(raw):
    """(parses, fits, keys) for a gateway body, mirroring response_from_mcp."""
    body_cap = define(TOOLS_C, "JR_TOOLS_HTTP_BODY_CAP")
    response_cap = define(HEADER, "JR_TOOLS_RESPONSE_CAP")
    try:
        root = json.loads(raw)
    except ValueError:
        return False, False, []
    upstream = root["result"] if isinstance(root, dict) and "result" in root else root
    printed = json.dumps({"result": upstream}, ensure_ascii=False, separators=(",", ":"))
    fits = (len(raw) <= body_cap and isinstance(root, dict)
            and len(printed.encode("utf-8")) + 1 <= response_cap)
    if isinstance(upstream, dict):
        keys = list(upstream)
    else:
        keys = ["(%s)" % type(upstream).__name__]
    return True, fits, keys


def main(argv):
    dry = "--dry-run" in argv
    argv = [a for a in argv if a != "--dry-run"]
    if not argv or len(argv) > 2:
        print(__doc__.split("\n\n")[0].strip())
        return 2
    tool = argv[0]
    args_json = argv[1] if len(argv) == 2 else "{}"
    url = os.environ.get("JARVIS_PROBE_URL", "")
    key = os.environ.get("JARVIS_PROBE_KEY", "")
    if not dry and not (url.startswith("https://") and key):
        print("error: set JARVIS_PROBE_URL (https) and JARVIS_PROBE_KEY")
        return 2
    try:
        request = build_request(tool, args_json, url)
    except Refused as why:
        print("refused: the firmware would answer %s" % why)
        return 1
    if dry:
        print("request_bytes: %d" % len(request))
        print("code_cap: %d" % define(TOOLS_C, "JR_TOOLS_CODE_CAP"))
        return 0
    req = urllib.request.Request(url, data=request, method="POST", headers={
        "Content-Type": "application/json", "Authorization": "Bearer " + key})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as resp:
            status, raw = resp.status, resp.read()
    except urllib.error.HTTPError as err:
        status, raw = err.code, err.read()
    except Exception as err:  # class name only: the message can carry the URL
        print("error: %s" % type(err).__name__)
        return 1
    parses, fits, keys = judge(raw)
    body_cap = define(TOOLS_C, "JR_TOOLS_HTTP_BODY_CAP")
    print("http_status: %d" % status)
    print("body_bytes: %d (device cap %d)" % (len(raw), body_cap))
    print("body_parses: %s" % ("yes" if parses else "no"))
    print("fits_device_caps: %s" % ("yes" if fits else "no"))
    print("top_level_keys: %s" % (",".join(keys) if keys else "-"))
    return 0 if status == 200 and parses and fits else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
