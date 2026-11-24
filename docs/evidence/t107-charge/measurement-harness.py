"""Parent-only frozen B deployment and read-only charge/timing receipt."""
import hashlib, json, subprocess, sys, time
from datetime import datetime, timezone
from pathlib import Path
import device_lib as lib

ROOT = lib.ROOT
OUT = ROOT / '.evidence/t107b'
OUT.mkdir(exist_ok=True)
ctl = lib.ctl
IMAGE = ROOT / '.evidence/t107/B-03864cc.bin'
RECEIPT = json.loads((ROOT / '.evidence/t107/B-frozen-image.json').read_text())

def utc(): return datetime.now(timezone.utc).isoformat()
def save(name, value):
    p = OUT / name
    assert not p.exists(), 'preserve each attempted receipt: ' + name
    p.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
def boundary():
    h = lib.muted()
    c = ctl.get_json('/api/cockpit')
    s = ctl.get_json('/api/debug/sleep')
    b = ctl.get_json('/api/sensors')['battery']
    d = ctl.get_json('/api/display')
    assert c['voice']['privacy_paused'] and not c['voice']['capturing']
    assert all(not h['audio_power'][k] for k in ('mic', 'adc', 'dac', 'pa'))
    assert h['memory']['largest_internal'] >= 8192 and d['flush_errors'] == 0
    return dict(utc=utc(), health=h, cockpit=c, sleep=s, battery=b, display=d)

assert (ROOT / '.evidence/t107a/idle-ab-charged/summary.json').exists()
pair = json.loads((ROOT / '.evidence/t107a/idle-ab-charged/summary.json').read_text())
assert (ROOT / ('.evidence/t107a/idle-ab-charged/' + pair['selected_image'] + '-final.json')).exists()
assert IMAGE.stat().st_size == RECEIPT['bytes']
assert hashlib.sha256(IMAGE.read_bytes()).hexdigest() == RECEIPT['sha256']
pre = boundary()
b = pre['battery']
assert b['available'] and b['present'] and b['percent'] >= 30 and b['age_ms'] <= 2500
lib.home()
expected = 'ota_0' if pre['health']['ota']['running'] == 'ota_1' else 'ota_1'
save('ota-start.json', dict(image=RECEIPT, pre=pre, expected_slot=expected))
print('B OTA START', flush=True)
p = subprocess.run([sys.executable, str(ROOT/'scripts/jarvis-desk.py'), '--timeout', '30',
                    'ota', '--image', str(IMAGE)], capture_output=True, encoding='utf-8')
(OUT/'ota.log').write_text(p.stdout+p.stderr, encoding='utf-8')
assert p.returncode == 0, 'B OTA command failed; private receipt retained'
deadline = time.monotonic()+180
seen = False
while time.monotonic() < deadline:
    try:
        h = ctl.get_json('/api/device/health')
        c = ctl.get_json('/api/cockpit')
        o = h['ota']
        assert h['privacy'] and c['voice']['privacy_paused'] and not c['voice']['capturing']
        candidate = o['running'] == expected
        seen = seen or candidate
        if seen and not candidate: raise RuntimeError('B rolled back; stop')
        if candidate and o['state'] == 'valid' and c['uptime_ms'] >= 45000:
            assert o['boot'] == expected and not o['active'] and o['last_error'] == 'ESP_OK'
            save('ota-valid.json', boundary())
            break
    except (OSError, ValueError): pass
    time.sleep(5)
else: raise RuntimeError('B did not become valid within 180 s')
print('B OTA VALID; privacy muted', flush=True)
reads = []
for i in range(3):
    reads.append(dict(utc=utc(), charge=ctl.get_json('/api/power/charge'),
                      battery=ctl.get_json('/api/sensors')['battery']))
    if i < 2: time.sleep(5)
save('charge-reads.json', reads)
assert ctl.cmd_screen(str(OUT/'home.png')) == 0
pre_timer = boundary()
save('timer-start.json', pre_timer)
started = time.monotonic()
lib.timer_sleep(300)
post_timer = boundary()
assert post_timer['sleep']['wake'] == 'timer'
assert post_timer['sleep']['sleeps'] == pre_timer['sleep']['sleeps'] + 1
save('timer-end.json', dict(seconds_to_api_readback=time.monotonic()-started,
    measurement_note='Unchanged timer harness first polls after POST response plus 300 s; API timing is an upper bound, display marker excludes ROM and optical wake edge.',
    post=post_timer))
save('final.json', boundary())
print(json.dumps(dict(event='B-charge-read', charge=reads[-1]['charge'])), flush=True)
