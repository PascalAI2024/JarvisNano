"""Summarize paired device facts, preserving conditional uncertainty."""
import json
from pathlib import Path
OUT=Path(__file__).resolve().parent
def load(n):return json.loads((OUT/n).read_text(encoding='utf-8-sig'))
pair={}
for stage in ['dream','idle-rest']:
    before=load('B-03864cc-r2-'+stage+'-fit.json');after=load('C-0b9645d-'+stage+'-fit.json')
    for f in (before,after):
        f['conditional_halfwidth_mv_h']=round(f['conditional95_scatter_mv_h']+f['conservative_rounding_mv_h'],2)
        f['conditional_interval_mv_h']=[round(f['drain_mv_h']-f['conditional_halfwidth_mv_h'],2),round(f['drain_mv_h']+f['conditional_halfwidth_mv_h'],2)]
    before['pre_snapshot_battery']=load('B-03864cc-r2-'+stage+'-start.json')['battery']
    after['pre_snapshot_battery']=load('C-0b9645d-'+stage+'-start.json')['battery']
    delta=round(after['drain_mv_h']-before['drain_mv_h'],2)
    bound=round(before['conditional_halfwidth_mv_h']+after['conditional_halfwidth_mv_h'],2)
    pair[stage]=dict(before=before,after=after,after_minus_before_mv_h=delta,conditional_difference_halfwidth_mv_h=bound,conditional_difference_interval_mv_h=[round(delta-bound,2),round(delta+bound,2)])
timers={}
for image in ['B-03864cc-r2','C-0b9645d']:
    pre=load(image+'-pre.json');timer=load(image+'-timer.json');post=timer['post']
    assert post['sleep']['wake']=='timer' and post['sleep']['sleeps']==pre['sleep']['sleeps']+1
    assert post['cockpit']['uptime_ms']<30000
    timers[image]=dict(api_upperbound_seconds=timer['seconds_to_readback'],reset_reason='not exposed by these API receipts',wake=post['sleep']['wake'],sleep_counter=[pre['sleep']['sleeps'],post['sleep']['sleeps']],boot_timing=post['display']['boot_timing'],privacy_muted=post['health']['privacy'])
before=timers['B-03864cc-r2']['boot_timing'];after=timers['C-0b9645d']['boot_timing']
summary=dict(source_commit='0b9645d3aa988e04c9d0ade4526b96b5ef277b60',baseline_source='03864cc',pair=pair,timers=timers,
    transfer_proxy_reduction_ms=before['first_transfer_done_boot_ms']-after['first_transfer_done_boot_ms'],
    timer_probe_protocol='Wait nominal300s after accepted request before probing; two-second retry cadence. Reported request-to-health seconds are detection upper bounds, not exact wake instants. Receipt boundary completes afterward.',
    wake_latency_note='First DMA completion since esp_timer boot excludes ROM, physical wake edge, optical first pixel and full face; no optical wake-to-pixel acceptance.',
    projected_battery_life=dict(before_hours=None,after_hours=None,reason='Short integer-gauge windows, non-linear voltage/SoC, unbounded relaxation/calibration and no current/cell-capacity measurement do not identify battery life.'),
    acceptance=dict(ota_valid=True,privacy_muted=True,matched_windows=True,nightstand_live_capture=False,glance_live_capture=False,controlled_lift_touch=False,natural_ladder_and_four_hour_timer=False,optical_wake_ms=False,new_image_voice=False,three_real_deck_workers=False),
    final=load('C-0b9645d-final.json'))
target=OUT/'C1-summary.json';assert not target.exists()
target.write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
print(json.dumps({k:v for k,v in summary.items() if k!='final'},indent=2))
