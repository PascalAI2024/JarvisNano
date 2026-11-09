"""Export every retained voltage point; never smooth away relaxation."""
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
EV=Path(__file__).resolve().parent
rows={p:[json.loads(x) for x in (EV/(p+'-long.jsonl')).read_text().splitlines()] for p in ('before','after')}
fig,axes=plt.subplots(2,2,figsize=(11,7),constrained_layout=True)
for ax,state in zip(axes.flat,('idle-rest','watch','dream','sleep')):
    for p,color in (('before','#486cba'),('after','#bd6335')):
        g=[r for r in rows[p] if r['state']==state] if state!='sleep' else [r for r in rows[p] if r['state'] in ('sleep-start','sleep-end')]
        if not g:
            ax.text(.04,.08,f'{p}: missing — timer wake unavailable',transform=ax.transAxes,color=color,fontsize=9)
            continue
        mv=[r['battery']['millivolts'] for r in g];pct=[r['battery']['percent'] for r in g]
        suffix=' (start only)' if state=='sleep' and len(g)==1 else ''
        ax.plot([r['elapsed_s']/60 for r in g],mv,'.-' if state!='sleep' else 'o--',color=color,lw=1,ms=4,label=f'{p}: {pct[0]}% → {pct[-1]}%{suffix}')
    ax.set_title({'idle-rest':'HOME idle / WHISPER','watch':'FUTURE WATCH / WHISPER','dream':'Timer-wake DREAM','sleep':'Timer sleep cycle (endpoints only)'}[state])
    ax.set_xlabel('Retained elapsed minutes');ax.set_ylabel('Battery mV');ax.grid(alpha=.2);ax.legend(fontsize=9)
fig.suptitle('Partial muted comparison — after timer wake failed\nVoltage is not current; SoC, relaxation, temperature and image-order bias remain unbounded',fontsize=12)
fig.savefig(EV/'battery-windows.png',dpi=160)
print(EV/'battery-windows.png')
