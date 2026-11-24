"""Counter deltas, with wrap handled and a per-core runtime denominator."""
import json
from pathlib import Path
EV=Path(__file__).resolve().parent
def profile(name):
 a,b=[json.loads(s) for s in (EV/name).read_text().splitlines()]
 from datetime import datetime
 seconds=(datetime.fromisoformat(b['wall_utc'])-datetime.fromisoformat(a['wall_utc'])).total_seconds()
 da,db=a['display'],b['display']
 frames=(db['render_frames']-da['render_frames'])%(2**32)
 render=(db['render_us']-da['render_us'])%(2**32)
 ta,tb=a['tasks'],b['tasks']
 total=(tb['total_runtime']-ta['total_runtime'])%(2**32)
 assert total>0 and frames>0 and seconds>0
 runs={t['name']:t['run'] for t in ta['tasks']}
 task_pct={t['name']:round(((t['run']-runs[t['name']])%(2**32))/total*100,3)
           for t in tb['tasks'] if t['name'] in runs}
 duration=total/1e6
 return dict(wall_timestamp_span_s=seconds,counter_elapsed_s=duration,render_frames=frames,
  frames_per_s=round(frames/duration,3),render_mean_us=round(render/frames,3),
  render_wall_percent=round(render/total*100,3),
  task_one_core_percent=task_pct,cpu_mhz=[ta['cpu_mhz'],tb['cpu_mhz']],
  brightness=[a['voice']['brightness'],b['voice']['brightness']],
  battery_pct=[r.get('battery',{}).get('percent') for r in (a,b)],
  free_psram=[da['free_psram'],db['free_psram']],
  largest_internal=[ta['largest_internal_block'],tb['largest_internal_block']])
r={p:profile(name) for p,name in [('before','watch-profile.jsonl'),('after','after-watch-profile.jsonl')]}
r['render_mean_reduction_percent']=round((1-r['after']['render_mean_us']/r['before']['render_mean_us'])*100,3)
r['note']='Profile wall timestamps have one-second precision; rates use the microsecond runtime counter span. Separate GETs are not atomic. Before profile did not retain battery values (null); nearby live capture was 69%, after profile 58%. Live differences include scheduling and SoC differences; they do not measure electrical energy. Total runtime is the one-core elapsed counter, not summed two-core task runtime.'
(EV/'runtime-comparison.json').write_text(json.dumps(r,indent=2)+'\n')
print(json.dumps(r,indent=2))
