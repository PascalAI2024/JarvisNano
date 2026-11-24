from device_lib import *
import argparse
p=argparse.ArgumentParser();p.add_argument('phase',choices=('before','after'));p.add_argument('--sha',required=True);a=p.parse_args()
OUT=ROOT/'.evidence/t107a';rows_path=OUT/(a.phase+'-long.jsonl')
assert not rows_path.exists(),'preserve existing window; no selective reruns'
def boundary(label):
    h=muted();audio=h['audio_power']
    assert all(not audio[k] for k in ('mic','adc','dac','pa'))
    assert h['memory']['largest_internal']>=8192 and h['display']['flush_errors']==0
    with (OUT/(a.phase+'-boundaries.jsonl')).open('a') as f:
        f.write(json.dumps(dict(label=label,sha=a.sha,health=h))+'\n')
def sample(state,elapsed):
    b=ctl.get_json('/api/sensors')['battery'];c=ctl.get_json('/api/cockpit');d=ctl.get_json('/api/display')
    assert b['available'] and b['present'] and not b['usb_present'] and not b['charging'] and b['percent']>20
    assert c['voice']['privacy_paused'] and not c['voice']['capturing']
    r=dict(phase=a.phase,sha=a.sha,state=state,elapsed_s=round(elapsed,3),wall_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),battery=b,voice=c['voice'],display=d,uptime_ms=c['uptime_ms'],watch_style=c['display']['watch_style'])
    with rows_path.open('a') as f:f.write(json.dumps(r)+'\n')
    return r
def window(state,seconds,mood):
    boundary('start-'+state);assert ctl.cmd_screen(str(OUT/(a.phase+'-'+state+'.png')))==0
    start=time.monotonic();print('START',state,seconds,flush=True)
    for i in range(seconds//15+1):
        time.sleep(max(0,start+i*15-time.monotonic()))
        r=sample(state,time.monotonic()-start)
        assert r['voice']['mood']==mood,(state,r['voice']['mood'])
        if i%4==0:print(state,round(r['elapsed_s']),r['battery']['millivolts'],r['battery']['percent'],r['display']['actual_fps'],flush=True)
    boundary('end-'+state);print('END',state,flush=True)
try:
    muted();post('/api/device/levels?brightness=65&volume=95')
    # Same timer boot footprint for both images, after the panel walk.
    home();timer_sleep(10)
    home();time.sleep(120);window('idle-rest',720,'WHISPER')
    home();swipe('down');time.sleep(120)
    assert ctl.get_json('/api/cockpit')['display']['watch_style']=='FUTURE'
    window('watch',720,'WHISPER')
    home();time.sleep(120);boundary('start-sleep')
    sample('sleep-start',0);start=time.monotonic()
    print('START sleep timer=1800',flush=True);timer_sleep(1800)
    r=sample('sleep-end',time.monotonic()-start);boundary('end-sleep')
    print('END sleep',r['elapsed_s'],r['battery']['millivolts'],r['battery']['percent'],flush=True)
    # Timer wake resumes genuine DREAM. No navigation or input in this window.
    time.sleep(120);window('dream',420,'DREAM')
finally:
    # Privacy is never cleared by this harness. Leave a readable muted home.
    post('/api/voice/control?armed=0');home()
    print('FINAL',json.dumps(muted()),flush=True)
