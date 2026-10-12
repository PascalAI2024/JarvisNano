#!/usr/bin/env bash
# From the workspace root in jarvisnano-hosttests, mounted at /repo.
# Host timing evidence only; no timing assertions or device operations.
set -eu
root=.evidence/t107a/perf
inc='-Icomponents/jr_display/include -Icomponents/jr_display/src -Icomponents/jr_ports/include -Icomponents/jr_display/tests/probe/stubs'
for opt in Os O2; do
  cc -std=gnu11 -"$opt" -Wall -Wextra -Werror -Wno-unused-function $inc \
    "$root/overlay-bench.c" components/jr_display/src/hud_render.c -ldl \
    -o "$root/overlay-current-$opt"
  cc -std=gnu11 -"$opt" -fPIC -shared -Icomponents/jr_display/include \
    "$root/hud-before.c" -o "$root/hud-before-$opt.so"
done
"$root/overlay-current-Os" compare "$PWD/$root/hud-before-Os.so"
"$root/overlay-current-Os" paired "$PWD/$root/hud-before-Os.so"
"$root/overlay-current-Os" paired "$PWD/$root/hud-before-Os.so"
"$root/overlay-current-O2" paired "$PWD/$root/hud-before-O2.so"
