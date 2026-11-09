"""Parent-only paired stage windows; no privacy, PMIC or lease writes."""
import argparse, importlib.util, json, sys, time
from datetime import datetime, timezone
from pathlib import Path
OUT=Path(__file__).resolve().parent
ROOT=OUT.parents[1]
sys.path.insert(0,str(ROOT/'.evidence/t107a'))
import device_lib as lib
spec=importlib.util.spec_from_file_location('fit',ROOT/'.evidence/t107a/idle-ab-charged/fit.py')
fit=importlib.util.module_from_spec(spec);spec.loader.exec_module(fit)
p=argparse.ArgumentParser();p.add_argument('image');p.add_argument('--wait-b',action='store_true');p.add_argument('--timer',action='store_true');a=p.parse_args()
ctl=lib.ctl
def utc():return datetime.now(timezone.utc).isoformat()
def save(name,obj):
    path=OUT/(a.image+'-'+name)
    assert not path.exists(),'retain every attempted receipt'
    path.write_text(json.dumps(obj,indent=2)+'\n',encoding='utf-8')
def boundary():
    h=lib.muted();c=ctl.get_json('/api/cockpit');b=ctl.get_json('/api/sensors')['battery'];d=ctl.get_json('/api/display');s=ctl.get_json('/api/debug/sleep')
    assert all(not h['audio_power'][k] for k in ('mic','adc','dac','pa'))
    assert c['voice']['privacy_paused'] and not c['voice']['capturing']
    assert h['memory']['largest_internal']>=8192 and d['flush_errors']==0
    assert b['available'] and b['present'] and b['percent']>30 and b['age_ms']<=2500 and not b['usb_present'] and not b['charging']
    return dict(utc=utc(),health=h,cockpit=c,battery=b,display=d,sleep=s)
def window(state,seconds,mood,brightness):
    save(state+'-start.json',boundary())
    capture_failures=[]
    for attempt in range(3):
        try:
            assert ctl.cmd_screen(str(OUT/(a.image+'-'+state+'.png')))==0
            break
        except Exception as error:
            capture_failures.append(dict(attempt=attempt+1,utc=utc(),error=str(error)))
            save(state+'-capture-failure-'+str(attempt+1)+'.json',capture_failures[-1])
            if attempt==2:raise
            time.sleep(1)
    rows=[];started=time.monotonic();prior_uptime=-1;prior_seq=-1
    print(json.dumps(dict(event='stage-start',image=a.image,state=state,seconds=seconds,utc=utc())),flush=True)
    path=OUT/(a.image+'-'+state+'.jsonl');assert not path.exists()
    for i in range(seconds//15+1):
        time.sleep(max(0,started+i*15-time.monotonic()))
        b=ctl.get_json('/api/sensors')['battery'];c=ctl.get_json('/api/cockpit');d=ctl.get_json('/api/display')
        row=dict(image=a.image,state=state,elapsed_s=round(time.monotonic()-started,3),wall_utc=utc(),battery=b,voice=c['voice'],display=d,uptime_ms=c['uptime_ms'],watch_style=c['display']['watch_style'])
        with path.open('a',encoding='utf-8') as f:f.write(json.dumps(row)+'\n')
        assert b['available'] and b['present'] and b['percent']>20 and not b['usb_present'] and not b['charging'] and b['age_ms']<=2500
        assert c['voice']['privacy_paused'] and not c['voice']['capturing'] and c['voice']['mood']==mood and c['voice']['brightness']==brightness
        assert d['flush_errors']==0 and c['uptime_ms']>prior_uptime and b['sample_seq']>prior_seq
        prior_uptime=c['uptime_ms'];prior_seq=b['sample_seq'];rows.append(row)
        if i%4==0:print(json.dumps(dict(event='sample',state=state,elapsed_s=row['elapsed_s'],soc=b['percent'],mv=b['millivolts'])),flush=True)
    save(state+'-end.json',boundary());result=fit.voltage_fit(rows,state)
    hours=result['duration_s']/3600
    result['soc_drain_pp_h']=(rows[0]['battery']['percent']-rows[-1]['battery']['percent'])/hours
    result['soc_endpoint_bound_pp_h']=2/hours
    save(state+'-fit.json',result)
    print(json.dumps(dict(event='stage-complete',image=a.image,state=state,fit=result)),flush=True)
if a.wait_b:
    deadline=time.monotonic()+600;exitfile=ROOT/'.evidence/t107/B-device.exit'
    while not exitfile.exists():
        assert time.monotonic()<deadline,'B did not finish; no measurement calls made'
        time.sleep(2)
    assert exitfile.read_text(encoding='utf-8-sig').strip()=='0','B failed; no measurement calls made'
pre=boundary();save('pre.json',pre)
if a.timer:
    lib.home();started=time.monotonic();lib.timer_sleep(300)
    save('timer.json',dict(seconds_to_readback=time.monotonic()-started,post=boundary(),optical=False,measurement_note='POST-to-API upper bound includes timer, ROM and boot; boot_timing markers exclude ROM and optical wake edge.'))
else:
    assert pre['sleep']['wake']=='timer' and pre['cockpit']['uptime_ms']<30000,'matched DREAM requires fresh timer boot'
print('SETTLE DREAM 120',flush=True);time.sleep(120)
window('dream',420,'DREAM',5)
lib.home();print('SETTLE HOME 120',flush=True);time.sleep(120)
window('idle-rest',720,'WHISPER',14)
lib.home();save('final.json',boundary());print('STAGE PAIR COMPLETE',flush=True)
