"""Continue after local CLI usage rejection; first window remains unchanged."""
import json, subprocess, sys
from pathlib import Path
out = Path(__file__).resolve().parent
def run(*args):
    subprocess.run([sys.executable, str(out/'run.py'), *args], check=True)
assert (out/'7578238-idle-end.json').exists()
assert (out/'0b4108f-ota-valid.json').exists()
run('idle', '--image', '0b4108f')
subprocess.run([sys.executable, str(out/'analyze.py')], check=True)
selected = json.loads((out/'summary.json').read_text(encoding='utf-8'))['selected_image']
if selected == '7578238':
    run('ota', '--image', selected, '--file', str(out.parent/'reproduction-300/7578238.bin'))
run('boundary', '--image', selected)
print('PAIR COMPLETE: ' + selected, flush=True)
