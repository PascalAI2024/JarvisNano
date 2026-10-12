# A/w2/ux result

Parent: t107/addendum-a. Executor: /root/a1_ux. Scope: main/main.c gesture corrections plus scratch production-routing test/mutation harness. Parent alone owns device, privacy and commits.

## Result

Implemented DOWN on DETAIL as close while retaining the current panel, then the following DOWN resumes ordinary ring navigation. Outside-sheet DISMISS now names the retained panel; WATCH clears its caption consistently with ordinary WATCH navigation. Ordinary UP/DOWN ring behavior, DESK conditional inclusion, and both nav/API shade doors retain their prior behavior.

Production code: shell_caption_space main/main.c:1596, shell_vertical_navigation :1606, shell_dismiss :1621. Existing input dispatch now calls those policies at :3453, :3477 and :3574. main.c diff is 50 added / 51 removed lines, net -1. The shared caption table is extracted rather than duplicated. Stale comments describing unconditional detail navigation were removed. No typography change.

## Evidence

The scratch Python harness extracts the actual three static main.c policy bodies and compiles them in the existing shell test translation unit, which includes actual jr_display.c and production navigation, caption and hit-test functions. It does not supply a replacement nav or caption model. Dispatch checks fail closed if main.c bypasses either policy. Builds use GCC with -std=c11 -Wall -Wextra -Werror -Wno-unused-function and the existing IDF stubs in the existing jarvisnano-hosttests Docker image.

| Run | Result | Durable raw output |
|---|---|---|
| Positive | 109/109 checks passed, exit 0 | main-shell-routing/positive/result.txt |
| Mutation: restore unconditional DETAIL DOWN to nav_next | 15/109 checks failed, exit 1 | main-shell-routing/detail-down-advances/result.txt |
| Mutation: restore controls caption after DETAIL dismissal | 5/109 checks failed, exit 1 | main-shell-routing/detail-dismiss-controls-caption/result.txt |

Coverage: all five production-reachable detail panels; outside/inside detail hit classification; retained panel and correct caption after either dismissal; following DOWN resume; ordinary full ring forward/backward and wrap with DESK dark/live; existing UP-from-detail behavior; shade dismissal through both nav and API state; existing overlay-free fallback. WATCH is checked for inactive caption rather than its stale private text bytes.

Commands: `python3 .evidence/t107a/test_main_shell_routing.py`, same with `--mutation detail-down-advances`, and same with `--mutation detail-dismiss-controls-caption`; each run inside jarvisnano-hosttests with this worktree mounted at /w. Mutations alter generated scratch translation units only, never production files, so no restore step is needed.

Focused compilation passed. `git diff --check` passed. No full host or firmware gate was run by this executor; parent owns the frozen combined-source gates.

## Changed artifacts

main/main.c; a1-ux-status.md; a2-ux-result.md; test_main_shell_routing.py; generated scratch source/binaries/result logs under main-shell-routing/. Wave 1 audit remains in a1-ux-audit.md. No shared register, display file, device, MCP or commit mutation.

## Remaining risk and decision

The harness executes production routing policies against actual display behavior, but not the RTOS input polling loop. Text dispatch assertions ensure the live loop selects those policies. Parent firmware build and post-OTA captures must prove both dismissed details preserve the selected panel and remain privacy muted.

The initial proposal was assigned and implemented below. No display API or display test source edit was required.

Listening and voice remain owner check pending. No owner surface, lease, task or privacy state was touched.

## Persistent coverage follow-on

Parent assigned main/tests and scripts/host-tests.sh to this executor. Added main/tests/test_shell_input.py and main/tests/test_shell_input.c.in; hooked jr_main_shell_tests into run_c_suites so both native C-compiler and established Docker fallback run it. The existing finish_c parser requires rc=0 and a positive check count. The full host gate now contains six suites. New tests use a temporary directory by default; --output-dir retains scratch evidence. --mutation changes generated extracted bodies only.

The persistent extractor skips braces inside comments and string/character literals. Dispatch validation requires the vertical branch to contain the production policy call directly, checks common panel feedback and the DISMISS case, and rejects guarded/unreachable helper calls. Function/linkage bodies remain production main.c. No nav or caption replacement model was added.

| Persistent run | Result | Durable raw output |
|---|---|---|
| Persistent runner, retained positive | 109 checks passed, exit 0 | main-shell-persistent/positive/result.txt |
| Actual production hook + finish_c, focused | 1 suite, 0 failed; 109 checks, exit 0 | main-shell-persistent/hook-result.txt |
| Restore DETAIL DOWN to nav_next in scratch extracted body | 15 failures of 109 checks, exit 1 | main-shell-persistent/detail-down-advances/result.txt |
| Restore controls caption in scratch extracted body | 5 failures of 109 checks, exit 1 | main-shell-persistent/detail-dismiss-controls-caption/result.txt |
| Dispatch fail-closed verification | 5 checks passed: actual accepted, vertical/dismiss/feedback bypasses rejected, unreachable vertical helper rejected | main-shell-persistent/dispatch-result.txt |

Focused hook proof executes the actual source prefix of scripts/host-tests.sh through run_main_shell_suite and finish_c, then asserts suites_run=1 and suites_failed=0. It avoids rerunning unrelated suites while the parent owns combined gates. Scratch wrapper: run_main_shell_hook.sh. bash -n scripts/host-tests.sh and git diff --check passed.

Exact persistent commands inside the existing jarvisnano-hosttests image: `python3 main/tests/test_shell_input.py --output-dir .evidence/t107a/main-shell-persistent/positive`; same with either mutation plus its matching output directory. Parent full gate remains `./scripts/host-tests.sh` and must now report six suites.

Explicit parent commit paths for this executor's production/test changes: main/main.c; main/tests/test_shell_input.py; main/tests/test_shell_input.c.in; scripts/host-tests.sh. Evidence remains scratch. No further main.c edit was made during persistence. No device/MCP/commit/privacy change. No executor decision remains; frozen combined-source gates, firmware build and post-OTA acceptance are parent work.
