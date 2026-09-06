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
# The five suites, in the order they run:
#   jr_display_hud_tests      components/jr_display/tests   procedural renderer
#   jr_display_shell_tests    components/jr_display/tests   spatial shell
#   jr_host_tests             host/                         jr_core + jr_dsp + jr_transport
#   jr_tools_template_tests   components/jr_tools/host      the generated JS template
#   test_jarvis_desk          scripts/                      the desk CLI (Python)
#
# A suite must run a positive number of tests to pass; "0 Tests" is a failure,
# and a run in which nothing ran at all exits 2.
#
# No C compiler on PATH (a Windows desk)? With Docker present the four C suites
# re-run inside the jarvisnano-hosttests image (gcc:14 + cmake + ninja +
# python3, built from scripts/host-tests.Dockerfile on first use); the Python
# suite runs natively either way. docs/reference/build-toolchain.md has the
# Git Bash path-rewrite finding behind MSYS_NO_PATHCONV=1.
#
#   ./scripts/host-tests.sh          run everything
#   ./scripts/host-tests.sh -q       only report failures and the summary
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUIET=0
[ "${1:-}" = "-q" ] && QUIET=1
# Set by the docker re-exec below: run the C suites only and hand the counts
# back on one plain line instead of printing a verdict.
INNER="${JR_HOST_TESTS_INNER:-0}"
IMAGE="jarvisnano-hosttests"

GREEN='\033[0;32m'; RED='\033[0;31m'; DIM='\033[2m'; NC='\033[0m'
suites_run=0; suites_failed=0; failed_names=""

LOG="$(mktemp)"; INNER_OUT="$(mktemp)"
trap 'rm -f "$LOG" "$INNER_OUT"' EXIT

pass() {  # name summary
    suites_run=$((suites_run + 1))
    [ $QUIET -eq 1 ] || printf "${GREEN}PASS${NC}  %-26s %s\n" "$1" "$2"
}
fail() {  # name reason
    suites_run=$((suites_run + 1)); suites_failed=$((suites_failed + 1))
    failed_names="$failed_names $1"
    printf "${RED}FAIL${NC}  %-26s %s\n" "$1" "$2"
}

have_cc() { command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1; }

# python3 first, then python — but only one that actually runs (the Windows
# Store ships a `python3` stub that opens a shop window instead).
pick_python() {
    local p
    for p in python3 python; do
        command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys' >/dev/null 2>&1 && { echo "$p"; return 0; }
    done
    return 1
}

# A CMakeCache written on the other side of the Docker boundary names a source
# dir that does not exist here, and cmake refuses to reuse it. Start that build
# dir over rather than fail on it.
fresh_cache() {  # src bdir
    local cache="$2/CMakeCache.txt" home want
    [ -f "$cache" ] || return 0
    home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")"
    want="$(cygpath -m "$1" 2>/dev/null || echo "$1")"
    [ "$home" = "$1" ] || [ "$home" = "$want" ] && return 0
    printf "${DIM}fresh${NC} %s (its cache was built for %s)\n" "${2#"$ROOT"/}" "$home"
    rm -rf "$2"
}

# Judge a C suite: rc 0 AND a positive count, or it failed. Unity prints
# "N Tests M Failures K Ignored"; the display and tools suites print
# "... passed (N checks)" or "M failure(s) of N checks".
finish_c() {  # name rc out
    local name="$1" rc="$2" out="$3" summary count
    summary="$(printf '%s\n' "$out" \
        | grep -oE '[0-9]+ Tests [0-9]+ Failures [0-9]+ Ignored|[A-Za-z_ ]*passed \([0-9]+ checks\)|[0-9]+ failure\(s\) of [0-9]+ checks' \
        | tail -1 | sed 's/^ *//')"
    count="$(printf '%s' "$summary" | grep -oE '[0-9]+ (Tests|checks)' | tail -1 | cut -d' ' -f1)"
    if [ "$rc" -eq 0 ] && [ "${count:-0}" -gt 0 ] 2>/dev/null; then
        pass "$name" "$summary"
    elif [ "$rc" -eq 0 ]; then
        fail "$name" "exited 0 but ran no tests"
    else
        fail "$name" "${summary:-rc=$rc}"
        printf '%s\n' "$out" | grep -E ':FAIL|^FAIL|Failures|failure' | head -20 | sed 's/^/    /'
    fi
}

# Configure + build one cmake target, then run its binary directly.
run_cmake_suite() {  # src bdir target bin
    local src="$1" bdir="$2" target="$3" bin="$4" out rc
    [ -d "$src" ] || { printf "${DIM}skip${NC} %s (no such directory)\n" "${src#"$ROOT"/}"; return; }
    fresh_cache "$src" "$bdir"
    [ $QUIET -eq 1 ] || printf "${DIM}build${NC} %s\n" "$target"
    if ! (cmake -S "$src" -B "$bdir" >"$LOG" 2>&1 && cmake --build "$bdir" --target "$target" >>"$LOG" 2>&1); then
        fail "$target" "(build)"
        grep -E "error[: ]|CMake Error" "$LOG" | head -5 | sed 's/^/    /'
        return
    fi
    out="$("$bin" 2>&1)"; rc=$?
    finish_c "$target" "$rc" "$out"
}

# Same, but the suite is registered with ctest; -V so the Unity count is
# visible on a pass, not only on failure.
run_ctest_suite() {  # src bdir name
    local src="$1" bdir="$2" name="$3" out rc
    [ -d "$src" ] || { printf "${DIM}skip${NC} %s (no such directory)\n" "${src#"$ROOT"/}"; return; }
    fresh_cache "$src" "$bdir"
    [ $QUIET -eq 1 ] || printf "${DIM}build${NC} %s\n" "$name"
    if ! (cmake -S "$src" -B "$bdir" >"$LOG" 2>&1 && cmake --build "$bdir" >>"$LOG" 2>&1); then
        fail "$name" "(build)"
        grep -E "error[: ]|CMake Error" "$LOG" | head -5 | sed 's/^/    /'
        return
    fi
    out="$(ctest --test-dir "$bdir" --output-on-failure -V 2>&1)"; rc=$?
    finish_c "$name" "$rc" "$out"
}

run_c_suites() {
    run_cmake_suite "$ROOT/components/jr_display/tests" "$ROOT/components/jr_display/tests/build" \
        jr_display_hud_tests "$ROOT/components/jr_display/tests/build/jr_display_hud_tests"
    run_cmake_suite "$ROOT/components/jr_display/tests" "$ROOT/components/jr_display/tests/build" \
        jr_display_shell_tests "$ROOT/components/jr_display/tests/build/jr_display_shell_tests"
    run_cmake_suite "$ROOT/host" "$ROOT/host/build" jr_host_tests "$ROOT/host/build/jr_host_tests"
    run_ctest_suite "$ROOT/components/jr_tools/host" "$ROOT/build-tools-host" jr_tools_template_tests
}

run_python_suite() {
    local py out rc count
    if ! py="$(pick_python)"; then
        fail "test_jarvis_desk" "no working python3/python on PATH"; return
    fi
    [ $QUIET -eq 1 ] || printf "${DIM}run${NC}   test_jarvis_desk (%s)\n" "$py"
    out="$(cd "$ROOT" && "$py" -m unittest scripts/test_jarvis_desk.py 2>&1)"; rc=$?
    count="$(printf '%s\n' "$out" | sed -n 's/^Ran \([0-9][0-9]*\) tests\{0,1\} .*/\1/p' | tail -1)"
    if [ "$rc" -eq 0 ] && [ "${count:-0}" -gt 0 ] 2>/dev/null; then
        pass "test_jarvis_desk" "Ran $count tests OK"
    elif [ "$rc" -eq 0 ]; then
        fail "test_jarvis_desk" "exited 0 but ran no tests"
    else
        fail "test_jarvis_desk" "Ran ${count:-0} tests, rc=$rc"
        printf '%s\n' "$out" | grep -E '^(FAIL|ERROR):' | head -20 | sed 's/^/    /'
    fi
}

ensure_image() {
    docker image inspect "$IMAGE" >/dev/null 2>&1 && return 0
    echo "building $IMAGE (gcc:14 + cmake + ninja + python3) from scripts/host-tests.Dockerfile"
    docker build -q -t "$IMAGE" - <"$ROOT/scripts/host-tests.Dockerfile" >"$LOG" 2>&1 && return 0
    tail -5 "$LOG" | sed 's/^/    /'
    return 1
}

# The C suites in the container. It prints its own per-suite lines; the counts
# come back on one plain "inner:" line and fold into this run's verdict.
run_c_suites_in_docker() {
    local rc line
    if ! ensure_image; then
        fail "c-suites(docker)" "$IMAGE could not be built"; return
    fi
    # Git Bash rewrites "/w" into "C:/Program Files/Git/w" before docker sees
    # it; MSYS_NO_PATHCONV stops that and is inert everywhere else.
    MSYS_NO_PATHCONV=1 docker run --rm -e JR_HOST_TESTS_INNER=1 \
        -v "$ROOT:/w" -w /w "$IMAGE" ./scripts/host-tests.sh "$@" | tee "$INNER_OUT" | grep -v '^inner: '
    rc=${PIPESTATUS[0]}
    line="$(grep -E '^inner: run=[0-9]+ failed=[0-9]+' "$INNER_OUT" | tail -1)"
    if [ -z "$line" ]; then
        fail "c-suites(docker)" "container exited $rc without running the suites"; return
    fi
    suites_run=$((suites_run + $(printf '%s' "$line" | sed 's/.*run=\([0-9]*\).*/\1/')))
    suites_failed=$((suites_failed + $(printf '%s' "$line" | sed 's/.*failed=\([0-9]*\).*/\1/')))
    failed_names="$failed_names$(printf '%s' "$line" | sed 's/.*names=//')"
}

if [ "$INNER" = 1 ]; then
    run_c_suites
    printf 'inner: run=%d failed=%d names=%s\n' "$suites_run" "$suites_failed" "$failed_names"
    [ "$suites_run" -eq 0 ] && exit 2
    [ "$suites_failed" -ne 0 ] && exit 1
    exit 0
fi

if have_cc; then
    run_c_suites
elif command -v docker >/dev/null 2>&1; then
    echo "no cc/gcc on PATH — running the C suites in the $IMAGE container"
    run_c_suites_in_docker "$@"
else
    fail "c-suites" "no cc/gcc and no docker on PATH"
fi
run_python_suite

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
