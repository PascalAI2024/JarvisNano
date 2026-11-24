"""Render the completed paired telemetry; this script performs no device calls."""
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

OUT = Path(__file__).resolve().parent
target = OUT / 'C1-telemetry.png'
assert not target.exists(), 'preserve the first complete plot'
fig, axes = plt.subplots(2, 2, figsize=(11, 7), sharex='row')
for row, (stage, name, expected) in enumerate([
    ('dream', 'DREAM — brightness 5, 7 minutes', 29),
    ('idle-rest', 'HOME / WHISPER — brightness 14, 12 minutes', 49),
]):
    for image, label, color in [
        ('B-03864cc-r2', 'Before: B / 03864cc', '#2463a0'),
        ('C-0b9645d', 'After: C1 / 0b9645d', '#b06011'),
    ]:
        rows = [json.loads(s) for s in (OUT / f'{image}-{stage}.jsonl').read_text().splitlines()]
        assert len(rows) == expected
        assert all(r['voice']['privacy_paused'] and not r['battery']['usb_present']
                   and not r['battery']['charging'] for r in rows)
        x = [r['elapsed_s'] / 60 for r in rows]
        for col, key in [(0, 'millivolts'), (1, 'percent')]:
            axes[row, col].plot(x, [r['battery'][key] for r in rows],
                                color=color, label=label, marker='.', markersize=4)
    axes[row, 0].set_title(name, loc='left', fontsize=11)
    axes[row, 0].set_ylabel('Battery voltage (mV)')
    axes[row, 1].set_title('Integer gauge reading', loc='left', fontsize=11)
    axes[row, 1].set_ylabel('Gauge SoC (%)')
    for ax in axes[row]:
        ax.set_xlabel('Elapsed minutes within window')
        ax.grid(alpha=.2)
        ax.legend(fontsize=8)
        ax.spines[['top', 'right']].set_visible(False)
fig.suptitle('C1 matched telemetry: muted, unplugged, identical window and brightness', fontsize=13)
fig.text(.5, .02, 'Chronological image order, cell relaxation and calibration remain confounds. '
         'These traces do not measure current or battery life.', ha='center', fontsize=9)
fig.tight_layout(rect=(0, .05, 1, .94))
fig.savefig(target, dpi=150, metadata={'Software': 'Matplotlib'})
plt.close(fig)
print(target)
