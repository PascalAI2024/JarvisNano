import math
def voltage_fit(rows,state):
    count,width,tcrit=(7,60,2.571) if state=='dream' else (6,120,2.776)
    assert len(rows)==(420 if state=='dream' else 720)//15+1
    duration=rows[-1]['elapsed_s'];assert duration>=count*width-1
    assert all(r['voice']['privacy_paused'] and not r['voice']['capturing'] for r in rows)
    assert all(not r['battery']['usb_present'] and not r['battery']['charging'] for r in rows)
    blocks=[]
    for i in range(count):
        g=[r for r in rows if min(int(r['elapsed_s']//width),count-1)==i];assert g
        blocks.append((sum(r['elapsed_s'] for r in g)/len(g)/3600,sum(r['battery']['millivolts'] for r in g)/len(g)))
    xb=sum(x for x,y in blocks)/count;yb=sum(y for x,y in blocks)/count
    sxx=sum((x-xb)**2 for x,y in blocks)
    slope=sum((x-xb)*(y-yb) for x,y in blocks)/sxx
    rss=sum((y-yb-slope*(x-xb))**2 for x,y in blocks)
    first,last=rows[0],rows[-1];hours=duration/3600
    d0,d1=first['display'],last['display'];frames=(d1['render_frames']-d0['render_frames'])%(2**32)
    render=(d1['render_us']-d0['render_us'])%(2**32)
    return dict(samples=len(rows),duration_s=duration,drain_mv_h=round(-slope,2),
        conditional95_scatter_mv_h=round(tcrit*math.sqrt(rss/(count-2)/sxx),2),
        # Each block mean can be displaced by at most 1 mV under the existing
        # conservative rounding assumption. Bound the actual OLS weights,
        # rather than applying an endpoint bound to a multi-point slope.
        conservative_rounding_mv_h=round(sum(abs(x-xb) for x,y in blocks)/sxx,2),
        endpoint_drain_mv_h=round((first['battery']['millivolts']-last['battery']['millivolts'])/hours,2),
        endpoint_rounding_mv_h=round(2/hours,2),
        endpoint_mv=[first['battery']['millivolts'],last['battery']['millivolts']],
        endpoint_pct=[first['battery']['percent'],last['battery']['percent']],
        moods=sorted(set(r['voice']['mood'] for r in rows)),watch_styles=sorted(set(r['watch_style'] for r in rows)),
        brightness=sorted(set(r['voice']['brightness'] for r in rows)),
        frames=frames,frames_per_s=round(frames/duration,3),render_wall_percent=round(render/(duration*1e6)*100,3),
        render_mean_us=round(render/frames,1) if frames else None,
        flushes=(d1['flush_completions']-d0['flush_completions'])%(2**32),
        max_flush_errors=max(r['display']['flush_errors'] for r in rows),block_means=blocks,
        uncertainty_note='Conditional residual scatter assumes independent blocks; ADC calibration, correlated relaxation, SoC, temperature and chronological image order remain unbounded.')
