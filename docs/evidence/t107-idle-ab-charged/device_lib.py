import importlib.util, json, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('jarvisctl',ROOT/'scripts/jarvisctl.py')
ctl=importlib.util.module_from_spec(spec);spec.loader.exec_module(ctl)
def post(p,body=None):
    return json.loads(ctl.api(p,'POST',json.dumps(body).encode() if body is not None else b''))
def home():
    post('/api/ui/shade?action=close')
    post('/api/debug/input?kind=double&x=233&y=100');time.sleep(1.2)
def swipe(direction):
    post('/api/debug/input?kind=swipe&dir='+direction);time.sleep(1.2)
def muted():
    h=ctl.get_json('/api/device/health');assert h['privacy'] and h['ota']['state']=='valid'
    assert not ctl.get_json('/api/operator/lease')['active']
    return h
def timer_sleep(seconds):
    muted();post('/api/debug/sleep?now=1&wake_s='+str(seconds))
    time.sleep(seconds)
    deadline=time.monotonic()+90
    while time.monotonic()<deadline:
        try:
            if ctl.get_json('/api/debug/sleep')['wake']=='timer':
                return muted()
        except Exception: pass
        time.sleep(2)
    raise RuntimeError('timer wake failed')
