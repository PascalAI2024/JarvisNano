"""Prespecified same-window comparison; conditional intervals are not energy."""
import json
from pathlib import Path
from fit import voltage_fit
OUT=Path(__file__).resolve().parent
fits={}
for image in ('7578238','0b4108f'):
    rows=[json.loads(x) for x in (OUT/(image+'-idle.jsonl')).read_text(encoding='utf-8').splitlines()]
    assert len(rows)==49
    assert all(20<r['battery']['percent']<=100 and r['battery']['age_ms']<=2500 for r in rows)
    assert all(r['voice']['mood']=='WHISPER' and r['voice']['brightness']==14 for r in rows)
    assert all(r['display']['flush_errors']==0 for r in rows)
    f=voltage_fit(rows,'idle-rest')
    f.update(source=image,start_utc=rows[0]['wall_utc'],end_utc=rows[-1]['wall_utc'],
             soc_start=rows[0]['battery']['percent'],soc_end=rows[-1]['battery']['percent'])
    f['soc_loss_points']=f['soc_start']-f['soc_end']
    f['soc_loss_rounding_points']=2
    f['conditional_interval_mv_h']=[round(f['drain_mv_h']-f['conditional95_scatter_mv_h']-f['conservative_rounding_mv_h'],2),
                                    round(f['drain_mv_h']+f['conditional95_scatter_mv_h']+f['conservative_rounding_mv_h'],2)]
    fits[image]=f
new,old=fits['7578238'],fits['0b4108f']
voltage_separated=old['conditional_interval_mv_h'][1]<new['conditional_interval_mv_h'][0]
soc_separated=old['soc_loss_points']+2<new['soc_loss_points']-2
clearly_better=voltage_separated and soc_separated
r=dict(method=dict(order=['7578238','0b4108f'],duration_s=720,settle_s=120,cadence_s=15,samples=49,brightness_cap=65,brightness_actual=14,volume=95,privacy_muted=True,usb_present=False,charging=False,
                   uncertainty='Original six two-minute mean OLS, t(4)=2.776 conditional scatter, conservative 1 mV/block rounding; SoC difference ±2 percentage points per window. Cell relaxation, charge history, temperature, chronological order and correlated samples remain unbounded.'),
       fits=fits,comparison=dict(new_minus_old_mv_h=round(new['drain_mv_h']-old['drain_mv_h'],2),
                                 conservative_conditional_difference_halfwidth_mv_h=round(new['conditional95_scatter_mv_h']+new['conservative_rounding_mv_h']+old['conditional95_scatter_mv_h']+old['conservative_rounding_mv_h'],2),
                                 voltage_intervals_favor_old=voltage_separated,soc_intervals_favor_old=soc_separated,baseline_clearly_better=clearly_better),
       selected_image='0b4108f' if clearly_better else '7578238',
       selection_note='Retain baseline only when both prespecified conservative voltage and SoC intervals favor it. Otherwise return to candidate. This single ordered A/B cannot establish a causal battery/energy saving.')
(OUT/'summary.json').write_text(json.dumps(r,indent=2)+'\n',encoding='utf-8',newline='\n')
print(json.dumps(r,indent=2))
