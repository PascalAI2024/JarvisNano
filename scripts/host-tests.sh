#!/usr/bin/env bash
# Run every host test suite. One command, so none can be forgotten.
#
# This exists because one was. jr_display had TWO suites — jr_display_hud_tests
# (the procedural renderer) and jr_display_shell_tests (the spatial shell) — and
# only the first was being run. Eight failures sat green-looking across several
# commits, hiding a real defect: the nav word's space field was two bits wide,
# so three of the seven ring screens were unreachable.
#
# The lesson is not "remember to run the other one". It is that a suite nobody
# runs is worse than no suite, because it looks like coverage. Add new suites
# HERE, not to a habit.
#
#   ./scripts/host-tests.sh          run everything
#   ./scripts/host-tests.sh -q       only report failures and the summary
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1

GREEN='\033[0;32m'; RED='\033[0;31m'; DIM='\033[2m'; NC='\033[0m'
suites_run=0; suites_failed=0; failed_names=""

# dir : cmake target : binary
SUITES="
components/jr_display/tests:jr_display_hud_tests:jr_display_hud_tests
components/jr_display/tests:jr_display_shell_tests:jr_display_shell_tests
"

for entry in $SUITES; do
    dir="${entry%%:*}"; rest="${entry#*:}"
    target="${rest%%:*}"; bin="${rest#*:}"
    path="$ROOT/$dir"
    [ -d "$path" ] || { printf "${DIM}skip${NC} %s (no such directory)\n" "$dir"; continue; }

    [ $QUIET -eq 1 ] || printf "${DIM}build${NC} %s\n" "$target"
    if ! (cd "$path" && cmake -B build >/dev/null 2>&1 && cmake --build build --target "$target" >/tmp/ht_build.log 2>&1); then
        printf "${RED}BUILD FAIL${NC} %s\n" "$target"
        grep -E "error:" /tmp/ht_build.log | head -5 | sed 's/^/    /'
        suites_failed=$((suites_failed + 1)); failed_names="$failed_names $target(build)"
        suites_run=$((suites_run + 1))
        continue
    fi

    suites_run=$((suites_run + 1))
    out="$("$path/build/$bin" 2>&1)"; rc=$?
    if [ $rc -eq 0 ]; then
        [ $QUIET -eq 1 ] || printf "${GREEN}PASS${NC}  %-26s %s\n" "$bin" "$(echo "$out" | tail -1)"
    else
        printf "${RED}FAIL${NC}  %s\n" "$bin"
        echo "$out" | grep -E "^FAIL|failure" | head -20 | sed 's/^/    /'
        suites_failed=$((suites_failed + 1)); failed_names="$failed_names $bin"
    fi
done

echo
# Assert what RAN, not just what failed: a suite that never executed reports no
# failures, and "did anything fail?" reads that as success.
printf "suites run: %d   failed: %d\n" "$suites_run" "$suites_failed"
if [ "$suites_run" -eq 0 ]; then
    printf "${RED}NOTHING RAN — that is a failure, not a pass${NC}\n"; exit 2
fi
if [ "$suites_failed" -ne 0 ]; then
    printf "${RED}failing:%s${NC}\n" "$failed_names"; exit 1
fi
printf "${GREEN}all %d host suites green${NC}\n" "$suites_run"
