"""Read-only battery polls at five-minute intervals; no wake or other writes."""
import json,sys,time
from datetime import datetime,timezone
from pathlib import Path
OUT=Path(__file__).resolve().parent
sys.path.insert(0,str(OUT.parent))
import device_lib as lib
FIRST=datetime.fromisoformat('2026-10-12T02:41:42.819302+00:00').timestamp()
DEADLINE=FIRST+10800
initial={'utc':'2026-10-12T02:41:42.819302+00:00','battery':{'available':True,'present':True,'charging':True,'usb_present':True,'percent':16,'millivolts':3684,'sample_seq':76,'age_ms':575}}
path=OUT/'charge-polls.jsonl'
assert not path.exists(),'preserve every charge-wait attempt'
def record(r):
    with path.open('a',encoding='utf-8',newline='\n') as f:f.write(json.dumps(r)+'\n')
    print(json.dumps(r),flush=True)
record(initial)
number=1
while True:
    target=min(FIRST+number*300,DEADLINE)
    time.sleep(max(0,target-time.time()))
    stamp=datetime.now(timezone.utc).isoformat()
    try:
        b=lib.ctl.get_json('/api/sensors')['battery']
        r={'utc':stamp,'poll':number,'battery':b}
        ready=b['available'] and b['present'] and 50<=b['percent']<=100 and not b['usb_present'] and not b['charging'] and b['age_ms']<=2500
    except Exception as exc:
        r={'utc':stamp,'poll':number,'error_type':type(exc).__name__}
        ready=False
    record(r)
    if ready:
        (OUT/'charge-ready.json').write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8',newline='\n')
        print('READY: SoC >=50%, USB and charging disconnected; no device writes made.',flush=True)
        break
    if target>=DEADLINE:
        (OUT/'charge-timeout.json').write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8',newline='\n')
        print('BLOCKED: charge/disconnect readiness not met within three hours.',flush=True)
        sys.exit(2)
    number+=1
