"""Synthetic controller replay from an original chart (test harness only).
With --by-kind the note kind gives the button (verified on Hotchkiss, all five
parts cleared): 0 Triangle, 1 Circle, 2 Cross, 3 Square, 5 Right, 6 Down; kind 8
is a special note. Use --origin-tick with the lead part's first note (513 on
Hotchkiss) so every part shares the song origin.

Converts the notes of a chart JSON written by music_chart.py into button
presses for --audio-anchored-input-replay: times are relative to the first
PCM of the song's audio channel (channel 3).

Verified on Watashi no Koi wa Hotchkiss (gt1, normal; 2026-10-08, AMD 780M):
one tick is 1/60 s, lane 3 is Cross and lane 2 is Circle, and with the first
note 1.1 s after the first PCM the song is cleared at 30/60/120/165 FPS
(ranks S/S/A/A). Other songs and parts reuse these as hypotheses.

With --menu MENU --start-us T the notes are merged into a menu replay in
absolute time, tick 0 at T: the moment the title card's Circle is pressed
(Hotchkiss at 30 and 165 FPS, Sunday Siesta at 165 FPS). This works for
every song; the first-PCM anchor does not (it comes 7.4-10.8 s after the
press depending on the song).
"""
import argparse,json
from pathlib import Path
BUTTONS={'circle':8192,'cross':16384,'square':32768,'triangle':4096,'up':16,'down':64,'left':128,'right':32}
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('chart',type=Path);p.add_argument('output',type=Path)
p.add_argument('--offset-ms',type=float,default=1100.0,help='first note time after the anchor (with --origin first-note)')
p.add_argument('--tick-us',type=float,default=1e6/60,help='microseconds per chart tick')
p.add_argument('--map',default='3:cross,2:circle',help='lane:button list')
p.add_argument('--press-ms',type=float,default=50.0)
p.add_argument('--origin',choices=('zero','first-note'),default='first-note',help='tick 0 or the first note at the offset')
p.add_argument('--origin-tick',type=int,default=None,help='tick placed at the offset (default: the first note of this chart; use the first note of the lead part so every part shares one origin, e.g. 513 for Hotchkiss)')
p.add_argument('--by-kind',action='store_true',help='--map keys are note kinds instead of lanes (bass: 5:right,6:down)')
p.add_argument('--menu',type=Path,help='menu replay to merge the notes into (absolute times)')
p.add_argument('--start-us',type=int,default=95000000,help='with --menu: time of tick 0')
a=p.parse_args()
notes=json.loads(a.chart.read_text())['layout']['notes']
mapping={int(k):BUTTONS[v] for k,v in (x.split(':') for x in a.map.split(','))}
origin=a.origin_tick if a.origin_tick is not None else (min(n['tick'] for n in notes) if a.origin=='first-note' else 0)
if a.menu:origin=0
events=[]
for n in notes:
    key=n['kind'] if a.by_kind else n['lane']
    if key not in mapping:continue
    t=(n['tick']-origin)*a.tick_us+(a.start_us if a.menu else a.offset_ms*1000)
    events.append((int(round(t)),mapping[key]))
events.sort()
lines=[f"# Synthetic chart replay: {a.chart.name} offset_ms={a.offset_ms} tick_us={a.tick_us} map={a.map}{' by kind' if a.by_kind else ''} origin={a.origin}","# Timing and mapping are test hypotheses."]
held={}
out=[]
menu_states=[]
if a.menu:
    for l in a.menu.read_text().splitlines():
        if l and not l.startswith('#'):tt,bb,_,_=l.split();menu_states.append((int(tt),int(bb)))
    lines[0]=f"# Synthetic: {a.menu.name} with chart {a.chart.name}, tick 0 at {a.start_us} us (title-card Circle press), map={a.map}"
for t,b in events:
    out.append((max(t,0),b,True));out.append((max(t,0)+int(a.press_ms*1000),b,False))
out.sort(key=lambda e:(e[0],e[2]))
state=0;menu_state=0
timeline=[(t,1,b,down) for t,b,down in out]+[(t,0,b,None) for t,b in menu_states]
timeline.sort(key=lambda e:(e[0],e[1]))
for t,kind,b,down in timeline:
    if kind==0:menu_state=b
    else:state=(state|b) if down else (state&~b)
    lines.append(f"{t} {state|menu_state} 128 128")
a.output.write_text("\n".join(lines)+"\n")
print(len(events),'notes')
