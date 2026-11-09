"""Interpose a scratch-only counter in the actual current production source.

Run on the Windows host, then run the generated shell runner in
jarvisnano-hosttests. No production instrumentation, public API or timer test.
The old traversal must remain pixel-correct but fail the work assertion.
"""
from pathlib import Path

workspace = Path(__file__).resolve().parents[3]
destination = (workspace / '.evidence/t107a/rim-mutants').resolve()
assert destination.is_relative_to(workspace / '.evidence/t107a')
assert not destination.exists(), 'Preserve previous evidence; do not overwrite'
source = (workspace / 'components/jr_display/src/hud_render.c').read_text()
old_arc = '''    for (int i = 0; i <= steps; ++i) {
        const int a = (i - 64) & 255;            /* -64 Q8 == 12 o'clock */
        ov_polar(s, cx, cy, a, OV_R_BATT, rim, 2);
    }
'''
begin = source.index('    /* Each half of the clockwise arc is monotone in y.')
end = source.index('    if (env->charging && steps > 0)', begin)
cases = {
    'control': source,
    'full-arc-restored': source[:begin] + old_arc + source[end:],
    'ascending-stamp-edge': source.replace('middle, s->y0 - 1, false)', 'middle, s->y0, false)', 1),
    'descending-stamp-edge': source.replace('steps + 1, s->y0 - 2, true)', 'steps + 1, s->y0 - 1, true)', 1),
    'full-endpoint-equivalent': source.replace('cy, begin, steps + 1, s->y0 - 2, true)',
        'cy, begin, steps == 256 ? steps : steps + 1, s->y0 - 2, true)', 1),
}
for name, text in cases.items():
    assert name == 'control' or text != source, name
    d = destination / name
    d.mkdir(parents=True)
    (d / 'hud.c').write_text(text, newline='\n')
    text = text.replace('static inline void ov_polar(',
        'static unsigned long long polar_visits;\n'
        'unsigned long long rim_visits(void) { return polar_visits; }\n'
        'void rim_visits_reset(void) { polar_visits = 0; }\n'
        'static inline void ov_polar(', 1)
    old = '    const int y = cy + ((lsin(a) * r) >> 15);'
    assert text.count(old) == 1
    text = text.replace(old, '    ++polar_visits;\n' + old, 1)
    (d / 'hud-count.c').write_text(text, newline='\n')

commands = [
    'set -eu',
    'root=.evidence/t107a/rim-mutants',
    'inc="-Icomponents/jr_display/include -Icomponents/jr_display/src -Icomponents/jr_ports/include -Icomponents/jr_display/tests/probe/stubs"',
    ': > "$root/results.tsv"',
]
for name in cases:
    commands += [
        f'd="$root/{name}"',
        'cc -std=gnu11 -Os -Wall -Wextra -Werror -Wno-unused-function -DCOUNT_VISITS $inc .evidence/t107a/perf/overlay-bench.c "$d/hud-count.c" -ldl -o "$d/work-test" > "$d/work-build.log" 2>&1',
        'cc -std=gnu11 -O0 -Wall -Wextra -Werror -Wno-unused-function $inc components/jr_display/tests/test_shell.c "$d/hud.c" -o "$d/shell-test" > "$d/shell-build.log" 2>&1',
        'set +e',
        '"$d/work-test" bound > "$d/work.log" 2>&1',
        'work=$?',
        '"$d/shell-test" > "$d/shell.log" 2>&1',
        'pixels=$?',
        'set -e',
        f'printf "{name}\\t%s\\t%s\\n" "$work" "$pixels" >> "$root/results.tsv"',
        'tail -1 "$d/work.log"',
        'tail -1 "$d/shell.log"',
    ]
commands.append('cat "$root/results.tsv"')
(destination / 'run.sh').write_text('\n'.join(commands) + '\n', newline='\n')
print(destination)
