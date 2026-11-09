"""Use exactly the previous six two-minute block fit, without rerunning it."""
import ast,hashlib,json,math,shutil
from pathlib import Path
OUT=Path(__file__).resolve().parent
original=OUT.parent/'summarize_long.py'
tree=ast.parse(original.read_text(encoding='utf-8'))
function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='voltage_fit')
scope={'math':math}
exec(compile(ast.Module(body=[function],type_ignores=[]),str(original),'exec'),scope)
result={'method':{'duration_s':720,'cadence_s':15,'settle_s':120,'samples':49,
    'brightness_cap':65,'brightness_actual':14,'volume':95,
    'fit':'OLS on six two-minute mean voltages; t(4)=2.776 conditional scatter and conservative 1 mV/block rounding bound',
    'low_soc_note':'Starting SoC 19% violated the prior greater-than-20% window precondition. This attempt failed when battery saver changed mood/brightness. No complete idle fit exists; voltage/SoC slope is not energy or current.'},'timers':{},'idle':{}}
for image in ('7578238','0b4108f'):
    timers=OUT/(image+'-timers.json')
    if timers.exists():
        data=json.loads(timers.read_text(encoding='utf-8'))
        result['timers'][image]=[{k:r[k] for k in ('run','timer_s','passed','wake_detected_by_api','seconds_to_api_back','seconds_after_nominal_timer','wake_reason','reset_reason','error')} for r in data]
    path=OUT/(image+'-idle.jsonl')
    if path.exists():
        rows=[json.loads(x) for x in path.read_text(encoding='utf-8').splitlines()]
        if len(rows)==49:
            fit=scope['voltage_fit'](rows,'idle-rest')
            fit['start_utc']=rows[0]['wall_utc'];fit['end_utc']=rows[-1]['wall_utc']
            fit['soc_start_pct']=rows[0]['battery']['percent']
            fit['soc_end_pct']=rows[-1]['battery']['percent']
            fit['endpoint_soc_drain_pct_points_h']=round((fit['soc_start_pct']-fit['soc_end_pct'])/(fit['duration_s']/3600),2)
            result['idle'][image]=fit
        else: result['idle'][image]={'complete':False,'samples':len(rows)}
result.update(status='BLOCKED',gate='idle-pair',reason='Candidate idle changed to DREAM at 105.391 seconds; SoC 18%, below battery-only OTA minimum 30%. No OTA attempted.',final_image='7578238',baseline_timer_fallback='not triggered: candidate passed all three cycles')
(OUT/'summary.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,indent=2))
