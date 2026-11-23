// The execute_tool program's byte budget, run for real. The program text is
// read back out of the generated JR_TOOLS_EXEC_JS in jr_tools_templates.c, so
// what is tested is the exact text the firmware sends to the gateway, run
// against mock `jarvis` services that return every awkward shape.
//
//   node components/jr_tools/host/test_exec_budget.mjs
//
// Node is not in the host-tests container (scripts/host-tests.sh), so this runs
// in CI next to scripts/check-dashboard-js.mjs and by hand.
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..", "..", "..");
const csrc = readFileSync(join(here, "..", "src", "jr_tools_templates.c"), "utf8");
const header = readFileSync(join(root, "components/jr_tools/include/jr_tools/jr_tools.h"), "utf8");

// The macro's string literal is JSON-compatible: only \" and \\ are escaped.
const m = csrc.match(/#define JR_TOOLS_EXEC_JS \\\r?\n\s*("(?:[^"\\]|\\.)*")/);
if (!m) throw new Error("JR_TOOLS_EXEC_JS not found");
const template = JSON.parse(m[1]);
const RESPONSE_CAP = Number(header.match(/JR_TOOLS_RESPONSE_CAP\s+(\d+)U/)[1]);
// {"result":<value>} plus the NUL, and the 64 B of cJSON number headroom.
const MAX = RESPONSE_CAP - 12 - 64;

let checks = 0;
function check(cond, what) {
  checks++;
  if (!cond) {
    console.log("FAIL: " + what);
    process.exit(1);
  }
}

const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

async function run(pathArr, args, services) {
  const parts = template.split("%s");
  check(parts.length === 3, "the template has exactly two %s");
  const code = parts[0] + JSON.stringify(pathArr) + parts[1] + JSON.stringify(args) + parts[2];
  return await new AsyncFunction("jarvis", code)(services);
}

const bytes = (x) => Buffer.byteLength(JSON.stringify(x), "utf8");
const svc = (value) => ({ test: { get: async () => (typeof value === "function" ? value() : value) } });
const call = (value) => run(["test", "get"], {}, svc(value));

// --- A result that fits is returned as before, unflagged.
{
  const out = await call({ results: [{ title: "a", snippet: "b" }] });
  check(JSON.stringify(out) === '{"results":[{"title":"a","snippet":"b"}]}', "a small result passes through unchanged");
  check(out.truncated === undefined, "a small result is not flagged");
  check((await call("hello")) === "hello", "a short string passes through");
  check((await call(42)) === 42, "a number passes through");
  check((await call(null)) === null, "null passes through");
  check((await call(undefined)) === undefined, "undefined passes through without throwing");
}

// --- The old trimming still applies to what fits: six items, 320 characters.
{
  const out = await call({ rows: Array.from({ length: 9 }, (_, i) => ({ i, t: "x".repeat(100) })) });
  check(out.rows.length === 6, "six items are kept");
  check(out.rows_total === 9, "a cut nested array says how long it was");
  check(out.truncated === undefined, "a result that fit the budget is not flagged");
  const s = await call({ t: "x".repeat(400) });
  check(s.t.length === 321 && s.truncated === undefined, "a long string is cut at 320 plus the ellipsis, unflagged when it fit");
}

// --- Shapes that used to outgrow the slot.
{
  const rows = Array.from({ length: 500 }, (_, i) => {
    const o = {};
    for (let k = 0; k < 20; k++) o["field" + k] = "value-" + i + "-" + "y".repeat(300);
    return o;
  });
  const out = await call(rows);
  check(bytes(out) <= MAX, "a 500-row list of fat objects fits: " + bytes(out));
  check(out.truncated === true && out.total === 500 && Array.isArray(out.items), "a list keeps items, says total, and is flagged");
  check(typeof out.hint === "string" && out.hint.length > 0, "the flag carries a hint to narrow the request");
  check(out.items.length >= 1, "at least one item survives");
}
{
  const out = await call("w".repeat(20000));
  check(bytes(out) <= MAX, "a 20000-character string fits");
  check(out.truncated === true && out.total_chars === 20000 && out.text.length > 0, "a long string becomes a flagged head");
}
{
  let deep = { leaf: "z".repeat(5000) };
  for (let i = 0; i < 12; i++) deep = { ["level" + i]: deep, pad: "p".repeat(400), more: Array.from({ length: 30 }, () => "q".repeat(100)) };
  const out = await call(deep);
  check(bytes(out) <= MAX, "a deep wide object fits: " + bytes(out));
  check(out.truncated === true, "a deep wide object is flagged");
}
{
  const wide = {};
  for (let k = 0; k < 600; k++) wide["key" + k] = "v".repeat(300);
  const out = await call(wide);
  check(bytes(out) <= MAX, "a 600-key object fits: " + bytes(out));
  check(out.truncated === true && Object.keys(out).length > 2, "a wide object keeps keys and is flagged");
}
{
  const out = await call({ title: "€".repeat(1500), more: "😀".repeat(700) });
  check(bytes(out) <= MAX, "three-byte and four-byte characters are counted as bytes: " + bytes(out));
}
{
  const a = { name: "cycle" };
  a.self = a;
  const out = await call(a);
  check(bytes(out) <= MAX, "a cyclic value still returns something that serialises");
  check(out.truncated === true, "a cyclic value is flagged");
}
{
  const out = await call({ id: 10n ** 30n, list: [1n, 2n] });
  check(bytes(out) <= MAX && out.id === "1000000000000000000000000000000", "BigInt becomes a string");
}

// --- The tightest tier can still overflow (three-byte keys and values), so the
// string head is a real last resort, not decoration.
{
  const key = (i, j) => String.fromCharCode(0x3042 + i * 12 + j).repeat(40);
  const hostile = {};
  for (let i = 0; i < 10; i++) {
    hostile[key(i, 0)] = {};
    for (let j = 1; j <= 10; j++) hostile[key(i, 0)][key(i, j)] = String.fromCharCode(0x4e00 + j).repeat(100);
  }
  const out = await call(hostile);
  check(bytes(out) <= MAX, "even the tightest tier's overflow is held to the budget: " + bytes(out));
  check(out.truncated === true && typeof out.text === "string" && out.total_chars > MAX,
        "it falls back to a flagged string head");
}

// --- The board path keeps its own projection and its total.
{
  const items = Array.from({ length: 400 }, (_, i) => ({ id: "id" + i + "x".repeat(40), title: "t".repeat(200), status: "open", resultSummary: "r".repeat(500) }));
  const out = await run(["coordination", "listWorkItems"], { projectId: "p" }, { coordination: { listWorkItems: async () => ({ workItems: items }) } });
  check(out.total === 400 && out.items.length === 6 && bytes(out) <= MAX, "the work-item list keeps six items and the total");
}

// --- coordination.portfolio projects up to 8 compact projects before TIERS.
{
  const projects = Array.from({ length: 12 }, (_, i) => ({
    id: "proj-" + i + "-" + "a".repeat(30),
    name: "Project " + i + " " + "n".repeat(60),
    description: "d".repeat(600),
    counts: { open: i + 1, inProgress: i % 3, blocked: i % 2, completed: i * 4 },
    extra: Array.from({ length: 10 }, () => "x".repeat(50)),
  }));
  const out = await run(["coordination", "portfolio"], {}, { coordination: { portfolio: async () => ({ projects }) } });
  check(bytes(out) <= MAX, "portfolio projection fits budget: " + bytes(out));
  check(out.total === 12 && Array.isArray(out.projects) && out.projects.length === 8, "portfolio keeps up to 8 projects and total");
  check(out.truncated === undefined, "compact portfolio is not truncated");
  const p0 = out.projects[0];
  check(p0.id.length === 24 && p0.name.length === 40 && p0.open === 1 && p0.inProgress === 0 && p0.blocked === 0 && p0.completed === 0,
        "portfolio project keeps compact id, name, and status counts");
  const p3 = out.projects[3];
  check(p3.open === 4 && p3.inProgress === 0 && p3.blocked === 1 && p3.completed === 12,
        "portfolio maps counts accurately");
  const flat = await run(["coordination", "portfolio"], {}, {
    coordination: { portfolio: async () => ([{ projectId: "jarvisnano-desk", title: "Desk", open: 2, in_progress: 1, blocked: 0, done: 5 }]) }
  });
  check(flat.total === 1 && flat.projects.length === 1 && flat.projects[0].id === "jarvisnano-desk" &&
        flat.projects[0].name === "Desk" && flat.projects[0].inProgress === 1 && flat.projects[0].completed === 5,
        "portfolio handles flat project shape and alias keys");
}

// --- Errors are bounded too, and policy lines are untouched.
{
  const out = await call(() => { throw new Error("e".repeat(1000)); });
  check(typeof out.error === "string" && out.error.length <= 200, "an error message is cut at 200");
  const unknown = await run(["nothing", "here"], {}, svc(1));
  check(unknown.error === "unknown tool", "an unknown tool is reported");
}

// --- Fuzz: whatever the shape, the budget holds, and a budget-forced cut says so.
{
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const pick = (a) => a[Math.floor(rnd() * a.length)];
  const str = () => pick(["a", "€", "😀", "é"]).repeat(Math.floor(rnd() * rnd() * 900));
  const gen = (d) => {
    const t = rnd();
    if (d > 7 || t < 0.25) return pick([str(), Math.floor(rnd() * 1e6), rnd(), true, null, str()]);
    if (t < 0.6) return Array.from({ length: Math.floor(rnd() * rnd() * (d < 2 ? 60 : 6)) }, () => gen(d + 1));
    const o = {};
    const n = Math.floor(rnd() * rnd() * (d < 2 ? 50 : 5));
    for (let k = 0; k < n; k++) o[str().slice(0, 60) + k] = gen(d + 1);
    return o;
  };
  let flagged = 0, cases = 0, over = 0;
  for (let i = 0; i < 1500; i++) {
    const v = gen(0);
    const raw = bytes(v);
    const out = await call(() => v);
    cases++;
    check(bytes(out) <= MAX, "fuzz " + i + ": " + bytes(out) + " bytes over " + MAX);
    if (raw > MAX) {
      over++;
      check(out !== null && typeof out === "object" && out.truncated === true, "fuzz " + i + ": an over-budget result carries truncated:true");
      flagged++;
    }
  }
  check(cases === 1500 && over >= 100, "the fuzz really produced over-budget results: " + over);
}

console.log("exec budget tests passed (" + checks + " checks)");
