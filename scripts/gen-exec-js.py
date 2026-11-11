#!/usr/bin/env python3
"""Generate JR_TOOLS_EXEC_JS in components/jr_tools/src/jr_tools_templates.c.

The source is components/jr_tools/js/exec_tool.js. This script strips its
full-line comments and indentation, joins the lines, turns __PATH__ and
__ARGS__ into the two %s of the snprintf in jr_tools_build_code, turns
__BUDGET__ into the byte budget derived below, and rewrites the block between
the BEGIN/END GENERATED markers. Never hand-edit the escaped C line.

    python3 scripts/gen-exec-js.py            rewrite the C file
    python3 scripts/gen-exec-js.py --check    exit 1 when the C file is stale
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "components/jr_tools/js/exec_tool.js"
DEST = ROOT / "components/jr_tools/src/jr_tools_templates.c"
HEADER = ROOT / "components/jr_tools/include/jr_tools/jr_tools.h"
TOOLS_C = ROOT / "components/jr_tools/src/jr_tools.c"

BEGIN = "/* BEGIN GENERATED: scripts/gen-exec-js.py from components/jr_tools/js/exec_tool.js */"
END = "/* END GENERATED */"

# jr_tools.c wraps the program's return value as {"result":<value>} and prints
# it into jr_tool_result_t.response_json (JR_TOOLS_RESPONSE_CAP, NUL included).
ENVELOPE = len('{"result":') + len("}") + 1
# cJSON_PrintPreallocated asks for 26 bytes of room before it prints a number,
# even a one-digit one, so a number at the very end needs that much slack; the
# rest covers the gateway wrapping the value differently than expected.
SLACK = 64
# Longest path array ("a","b",... with one-character segments under the
# 48-byte name cap) the template can be handed.
PATH_MAX = 100


def define(text, name):
    m = re.search(r"#define\s+%s\s+(\d+)U?" % name, text)
    if not m:
        sys.exit("gen-exec-js: %s not found" % name)
    return int(m.group(1))


def caps():
    header = HEADER.read_text(encoding="utf-8")
    code_cap = define(TOOLS_C.read_text(encoding="utf-8"), "JR_TOOLS_CODE_CAP")
    return (define(header, "JR_TOOLS_RESPONSE_CAP"),
            define(header, "JR_TOOLS_ARGS_CAP"), code_cap)


def budget():
    response_cap, _, _ = caps()
    return response_cap - ENVELOPE - SLACK


def program():
    """The program text with two %s left in it, exactly what snprintf gets."""
    lines = []
    for raw in SRC.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("//"):
            lines.append(line)
    text = "".join(lines)
    if "//" in text:
        sys.exit("gen-exec-js: '//' left in the program (a trailing comment?)")
    text = text.replace("__BUDGET__", str(budget()))
    for token in ("__PATH__", "__ARGS__"):
        if text.count(token) != 1:
            sys.exit("gen-exec-js: %s must appear exactly once" % token)
        text = text.replace(token, "%s")
    if text.count("%") != 2 or "__" in text.replace("__proto__", ""):
        sys.exit("gen-exec-js: stray % or __ token left in the program")
    _, args_cap, code_cap = caps()
    worst = len(text) - 4 + PATH_MAX + args_cap + 1
    if worst > code_cap:
        sys.exit("gen-exec-js: program %d B + largest path and args = %d B, "
                 "over JR_TOOLS_CODE_CAP %d" % (len(text), worst, code_cap))
    return text


def block():
    escaped = program().replace("\\", "\\\\").replace('"', '\\"')
    return "%s\n#define JR_TOOLS_EXEC_JS \\\n    \"%s\"\n%s" % (BEGIN, escaped, END)


def main():
    current = DEST.read_text(encoding="utf-8")
    start, stop = current.find(BEGIN), current.find(END)
    if start < 0 or stop < start:
        sys.exit("gen-exec-js: BEGIN/END GENERATED markers not found in %s" % DEST)
    updated = current[:start] + block() + current[stop + len(END):]
    if "--check" in sys.argv[1:]:
        if updated != current:
            print("gen-exec-js: jr_tools_templates.c is stale; run scripts/gen-exec-js.py")
            return 1
        print("gen-exec-js: up to date (program %d B, budget %d B)"
              % (len(program()), budget()))
        return 0
    if updated != current:
        DEST.write_text(updated, encoding="utf-8", newline="\n")
    print("gen-exec-js: program %d B, budget %d B%s"
          % (len(program()), budget(), "" if updated != current else " (unchanged)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
