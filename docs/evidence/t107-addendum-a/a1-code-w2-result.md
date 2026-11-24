# Addendum A wave 2: C1 subtraction complete

Owner: a1_code. Parent item: t107/addendum-a / A/w2/code.
Status: implemented and focused proof passed; ready for parent integration.

## Result

Removed the preprocessor-excluded tilt math and redundant return. Replaced
historical commentary with the current zero-offset invariant and its reasons.
Corrected the API documentation to promise zero for every angle and tolerate
null output pointers. Retained HUD_TILT_MAX and the public function signature.

Only assigned source files changed:

| File | Added | Removed |
|---|---:|---:|
| components/jr_display/src/hud_render.c | 4 | 33 |
| components/jr_display/include/jr_display/hud_render.h | 2 | 9 |
| Total | 6 | 42 |

Net subtraction: 36 lines. No runtime or battery gain is claimed for code that
was already excluded by the preprocessor.

## Evidence

The existing HUD suite passed **11,451 checks**, exit 0. Compiler: GNU 14.4.0
inside the existing jarvisnano-hosttests image. Log:
.evidence/t107a/a1-code-hud-positive.log.

Command, with the worktree mounted read-only at /w and all build output confined
to this container's /tmp:

```sh
cmake -S components/jr_display/tests -B /tmp/a1-hud-build -G Ninja &&
cmake --build /tmp/a1-hud-build --target jr_display_hud_tests &&
/tmp/a1-hud-build/jr_display_hud_tests
```

Scratch mutation: copied the production C source to
/tmp/a1_hud_render_mutant.c in a separate container, asserted the needle occurs
exactly once, and changed `if (ox) *ox = 0;` to `if (ox) *ox = 1;`. Tests and
headers were read through the same read-only mount. The production worktree
was never mutated for this proof.

```sh
gcc -std=c11 -Wall -Wextra -Werror \
  -I/w/components/jr_display/include \
  /w/components/jr_display/tests/test_hud_render.c \
  /tmp/a1_hud_render_mutant.c -o /tmp/a1_hud_mutant &&
/tmp/a1_hud_mutant
```

Result: **6 failures of 11,451 checks**, exit 1 as required. All six existing
test_tilt_offset_is_disabled angle cases rejected the mutated `(1,0)` output.
Log: .evidence/t107a/a1-code-hud-mutant.log. The host wrapper separately checked
the exit code was 1 and exactly six `tilt must not deflect` failures appeared.

`git diff --check --` with both explicit source paths exited 0. Git emitted its
existing LF-to-CRLF normalization notices; no whitespace errors.

## Handoff

Result: C1 complete; no new tests or public seam added.
Changed artifacts: the two assigned source files; this result; the two logs.
Remaining risk: parent still owns the full five-suite gate, firmware build,
commit, OTA, and hardware proof. This focused run is not the final full gate.
Decision needed: none for C1. No commit, device operation, privacy action,
jr_display.c, test_shell.c, main.c, or unassigned source edit was made.
