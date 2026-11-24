"""Parent-only reproduction; wraps the existing timer gate without changing it."""
import argparse, contextlib, hashlib, json, os, sys, time
from datetime import datetime, timezone
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device_lib as lib

OUT = Path(__file__).resolve().parent
ROOT = lib.ROOT
ctl = lib.ctl
def utc(): return datetime.now(timezone.utc).isoformat()
def save(name, obj):
    (OUT / name).write_text(json.dumps(obj, indent=2) + '\n', encoding='utf-8')
def emit(obj): print(json.dumps(obj), flush=True)
def boundary():
    h = lib.muted()
    c = ctl.get_json('/api/cockpit')
    s = ctl.get_json('/api/debug/sleep')
    b = ctl.get_json('/api/sensors')['battery']
    assert c['voice']['privacy_paused'] and not c['voice']['capturing']
    assert all(not h['audio_power'][k] for k in ('mic','adc','dac','pa'))
    assert h['memory']['largest_internal'] >= 8192 and h['display']['flush_errors'] == 0
    return dict(utc=utc(), health=h, uptime_ms=c['uptime_ms'], voice=c['voice'], sleep=s, battery=b)

def timers(image):
    results = []
    for number in range(1, 4):
        name = f'{image}-timer-{number}'
        assert not (OUT / (name + '.json')).exists(), 'preserve every attempted cycle'
        lib.home()
        pre = boundary()
        attempts = []
        original_api = ctl.api
        request_start = None
        request_end = None
        def observed_api(path, method='GET', body=None, timeout=10):
            nonlocal request_start, request_end
            start = time.monotonic()
            stamp = utc()
            is_sleep = path == '/api/debug/sleep?now=1&wake_s=300' and method == 'POST'
            if is_sleep: request_start = start
            try:
                value = original_api(path, method, body, timeout)
            except Exception as exc:
                if request_start is not None:
                    attempts.append(dict(path=path, method=method, utc=stamp,
                        start_s=round(start-request_start,3), end_s=round(time.monotonic()-request_start,3),
                        ok=False, error_type=type(exc).__name__))
                raise
            if request_start is not None:
                if is_sleep: request_end = time.monotonic()
                attempts.append(dict(path=path, method=method, utc=stamp,
                    start_s=round(start-request_start,3), end_s=round(time.monotonic()-request_start,3), ok=True))
            return value
        ctl.api = observed_api
        emit(dict(event='timer-start', image=image, run=number, timer_s=300,
                  utc=utc(), soc=pre['battery']['percent'], prior_wake=pre['sleep']['wake']))
        error = None
        try:
            lib.timer_sleep(300)
        except Exception as exc:
            error = dict(type=type(exc).__name__, message=str(exc))
        finally:
            ctl.api = original_api
        try:
            post = boundary()
        except Exception as exc:
            post = None
            if error is None: error = dict(type=type(exc).__name__, message=str(exc))
        first = next((x for x in attempts if x['method']=='GET' and x['ok']), None)
        passed = error is None and post is not None and post['sleep']['wake']=='timer' and (
            post['sleep']['sleeps']==pre['sleep']['sleeps']+1) and post['uptime_ms'] < 90000
        result = dict(image=image, run=number, timer_s=300, passed=passed,
            wake_detected_by_api=bool(post and post['sleep']['wake']=='timer'),
            seconds_to_api_back=first['end_s'] if first else None,
            seconds_after_nominal_timer=(round(first['end_s']-300,3) if first else None),
            sleep_request_round_trip_s=(round(request_end-request_start,3) if request_end else None),
            wake_reason=post['sleep']['wake'] if post else None,
            reset_reason=None, reset_reason_note='No reset-reason field is exposed by authenticated API; wake reason is ESP sleep wake cause.',
            pre=pre, post=post, attempts=attempts, error=error,
            timing_note='Unchanged harness starts polling 300 seconds after POST response; first successful response is an upper bound on availability.')
        save(name+'.json', result)
        results.append(result)
        emit({k:result[k] for k in ('image','run','passed','wake_detected_by_api','seconds_to_api_back','seconds_after_nominal_timer','wake_reason','reset_reason')})
        if post is None:
            save(image+'-timers.json', results)
            raise RuntimeError('Device is not authenticated/reachable after failed timer gate; owner wake required before baseline OTA.')
        # Keep it awake through privacy-preserving navigation between gates.
        lib.home()
    save(image+'-timers.json', results)

def idle(image):
    path = OUT / (image+'-idle.jsonl')
    assert not path.exists(), 'preserve every attempted measurement'
    pre = boundary()
    b = pre['battery']
    assert b['available'] and b['present'] and not b['usb_present'] and not b['charging']
    lib.post('/api/device/levels?brightness=65&volume=95')
    lib.home()
    emit(dict(event='idle-settle', image=image, seconds=120, utc=utc(), soc=b['percent']))
    time.sleep(120)
    start_boundary = boundary()
    save(image+'-idle-start.json', start_boundary)
    assert ctl.cmd_screen(str(OUT/(image+'-idle.png'))) == 0
    start = time.monotonic()
    emit(dict(event='idle-start',image=image,utc=utc(),soc=start_boundary['battery']['percent']))
    for i in range(49):
        time.sleep(max(0,start+i*15-time.monotonic()))
        b = ctl.get_json('/api/sensors')['battery']
        c = ctl.get_json('/api/cockpit')
        d = ctl.get_json('/api/display')
        row=dict(image=image,state='idle-rest',elapsed_s=round(time.monotonic()-start,3),wall_utc=utc(),
                 battery=b,voice=c['voice'],display=d,uptime_ms=c['uptime_ms'],watch_style=c['display']['watch_style'])
        with path.open('a',encoding='utf-8') as f: f.write(json.dumps(row)+'\n')
        assert b['available'] and b['present'] and not b['usb_present'] and not b['charging']
        assert c['voice']['privacy_paused'] and not c['voice']['capturing']
        assert c['voice']['mood']=='WHISPER' and c['voice']['brightness']==14
        assert d['flush_errors']==0
        if i%4==0: emit(dict(event='idle-sample',image=image,elapsed_s=row['elapsed_s'],mv=b['millivolts'],soc=b['percent'],brightness=c['voice']['brightness']))
    save(image+'-idle-end.json', boundary())
    emit(dict(event='idle-complete',image=image,samples=49))

def ota(image, file):
    import subprocess
    file = Path(file).resolve()
    digest = hashlib.sha256(file.read_bytes()).hexdigest()
    pre = boundary()
    lib.home()
    expected = 'ota_0' if pre['health']['ota']['running']=='ota_1' else 'ota_1'
    save(image+'-ota-start.json',dict(image=image,sha256=digest,bytes=file.stat().st_size,expected_slot=expected,pre=pre))
    emit(dict(event='ota-start',image=image,sha256=digest,expected_slot=expected,soc=pre['battery']['percent']))
    proc = subprocess.run([sys.executable,str(ROOT/'scripts/jarvis-desk.py'),'--timeout','120','ota','--image',str(file)],capture_output=True,encoding='utf-8')
    (OUT/(image+'-ota.log')).write_text(proc.stdout+proc.stderr,encoding='utf-8')
    assert proc.returncode==0, 'OTA command failed; receipt preserved privately'
    deadline=time.monotonic()+180
    seen=False
    while time.monotonic()<deadline:
        try:
            h=ctl.get_json('/api/device/health');c=ctl.get_json('/api/cockpit');o=h['ota']
            assert h['privacy'] and c['voice']['privacy_paused'] and not c['voice']['capturing']
            candidate=o['running']==expected;seen=seen or candidate
            if seen and not candidate: raise RuntimeError('Uploaded image rolled back')
            if candidate and o['state']=='valid' and c['uptime_ms']>=45000:
                assert o['boot']==expected and not o['active'] and o['last_error']=='ESP_OK'
                post=boundary()
                save(image+'-ota-valid.json',dict(image=image,sha256=digest,expected_slot=expected,post=post))
                emit(dict(event='ota-valid',image=image,slot=expected,uptime_ms=c['uptime_ms'],privacy=h['privacy']))
                return
        except (OSError,ValueError): pass
        time.sleep(5)
    raise RuntimeError('Uploaded image did not become valid within 180 seconds')

p=argparse.ArgumentParser();p.add_argument('action',choices=('timers','idle','ota','boundary'))
p.add_argument('--image',default='7578238');p.add_argument('--file');a=p.parse_args()
if a.action=='timers': timers(a.image)
elif a.action=='idle': idle(a.image)
elif a.action=='ota': ota(a.image,a.file)
else:
    b=boundary();save(a.image+'-final.json',b)
    emit(dict(ota=b['health']['ota'],privacy=b['health']['privacy'],audio_power=b['health']['audio_power'],uptime_ms=b['uptime_ms'],wake=b['sleep']['wake'],battery=b['battery']))
