"""Short-window voltage evidence; the interval does not bound state-of-charge drift."""
import json
import math
import sys
from pathlib import Path

root = Path(__file__).resolve().parent

def awake(rows):
    blocks = []
    for minute in range(5):
        points = [r for r in rows if min(int(r['elapsed_s'] // 60), 4) == minute]
        if not points:
            raise ValueError('incomplete five-minute window')
        blocks.append((sum(r['elapsed_s'] for r in points) / len(points) / 3600,
                       sum(r['battery']['millivolts'] for r in points) / len(points)))
    xbar = sum(x for x, y in blocks) / 5
    ybar = sum(y for x, y in blocks) / 5
    sxx = sum((x - xbar) ** 2 for x, y in blocks)
    slope = sum((x - xbar) * (y - ybar) for x, y in blocks) / sxx
    resid = sum((y - ybar - slope * (x - xbar)) ** 2 for x, y in blocks)
    scatter = 3.182 * math.sqrt(resid / 3 / sxx)
    quant = 2 / (max(x for x, y in blocks) - min(x for x, y in blocks))
    return dict(samples=len(rows), duration_s=rows[-1]['elapsed_s'],
                drain_mv_h=round(-slope, 1),
                interval95_scatter_mv_h=round(scatter, 1),
                quantization_bound_mv_h=round(quant, 1),
                endpoint_mv=[rows[0]['battery']['millivolts'], rows[-1]['battery']['millivolts']],
                endpoint_pct=[rows[0]['battery']['percent'], rows[-1]['battery']['percent']],
                mood=sorted(set(r['voice']['mood'] for r in rows)),
                privacy_all_muted=all(r['voice']['privacy_paused'] for r in rows),
                block_means=blocks)

for phase in sys.argv[1:] or ['before', 'after']:
    path = root / (phase + '-samples.jsonl')
    if not path.exists():
        continue
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    result = {}
    for state in ('idle-rest', 'watch'):
        group = [r for r in rows if r['state'] == state]
        if group and group[-1]['elapsed_s'] >= 299:
            result[state] = awake(group)
    starts = [r for r in rows if r['state'] == 'sleep-start']
    ends = [r for r in rows if r['state'] == 'sleep-end']
    if starts and ends:
        a, b = starts[-1], ends[-1]
        hours = b['elapsed_s'] / 3600
        result['sleep-cycle'] = dict(duration_s=b['elapsed_s'],
            endpoint_mv=[a['battery']['millivolts'], b['battery']['millivolts']],
            endpoint_pct=[a['battery']['percent'], b['battery']['percent']],
            drain_mv_h=round((a['battery']['millivolts']-b['battery']['millivolts'])/hours, 1),
            quantization_bound_mv_h=round(2/hours, 1),
            note='Includes shutdown, cold boot and reconnect; voltage relaxation is unbounded.')
    (root / (phase + '-summary.json')).write_text(json.dumps(result, indent=2) + '\n')
    print(phase, json.dumps(result, indent=2))
