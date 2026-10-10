"""PC frame-rate patch: real 30/60/120/165 FPS logic at the original speed.

The original engine counts time in 1/60 s ticks and assumes 30 FPS.  The port
computes, for every logic frame, how much game time it represents (see
boot_probe.cpp, `kon_logic_*`), and this script makes the generated code use
those values instead of the 30 FPS constants:

* 0x088EAC44 (`jr ra; li v0,2`, ticks per frame) returns `kon_logic_frame_ticks`
  (whole ticks this frame, accumulated so fractions are never lost).
* Call sites that use the ticks as a float (animation playback, per-frame steps)
  read `kon_logic_frame_ticks_f` (exact fraction: 0.5 at 120 FPS).
* Call sites that convert a duration in ticks into 30 FPS animation keyframes
  keep the constant 2 (unit conversion, not elapsed time).
* 0x088EAC3C (`jr ra; li v0,30`, logic FPS) returns `kon_logic_fps`.
* Per-frame timers authored in 30 FPS frames and the 2D animation frame advance
  by `kon_logic_step30` (whole 30 FPS frames elapsed this frame).
* `x += (target - x) / 2` smoothing uses a frame-rate independent factor.
* Pad auto-repeat (0x089F803C): the per-button hold counters count 30 FPS
  frames and a held button repeats only on frames where one elapses.
* Whole per-frame state functions written for 30 FPS (club room girls' AI and
  walking, blinking) run only on logic frames where a 30 FPS frame elapsed;
  everything they do (timers, steps, random draws) keeps its original rate.

With the original 30 FPS mode every value equals the original constant.
Idempotent; also upgrades units patched by the earlier 60 FPS-only version.
Run it after regenerating work/recompiled_corrected.
"""
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
import sys
# Optional argument: the corpus folder (the player's installer passes its own).
CORPUS=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/'work/recompiled_corrected'
DECLARATIONS='extern "C" std::uint32_t kon_logic_frame_ticks,kon_logic_step30,kon_logic_fps,kon_pad_hold_before,kon_pad_repeat_due; extern "C" float kon_logic_frame_ticks_f; extern "C" std::uint32_t kon_smooth_step(std::uint32_t); // PC FPS patch\n'
OLD_DECLARATIONS=['extern "C" std::uint32_t kon_logic_frame_ticks,kon_logic_step30,kon_logic_fps; extern "C" float kon_logic_frame_ticks_f; extern "C" std::uint32_t kon_smooth_step(std::uint32_t); // PC FPS patch\n',
                  'extern "C" std::uint32_t kon_logic_frame_ticks; // PC FPS patch\n',
                  'extern "C" std::uint32_t kon_logic_half_frame; // PC FPS patch\n',
                  'extern "C" std::uint32_t kon_smooth_step(std::uint32_t); // PC FPS patch\n']

def edit(unit,change):
    path=CORPUS/f'generated_unit_{unit}.cpp'
    raw=path.read_bytes().decode('utf-8');crlf='\r\n' in raw;text=raw.replace('\r\n','\n')
    new=change(text)
    if 'PC FPS patch' in new:
        # whole declaration lines only: an older one is a substring of the current
        new='\n'.join(l for l in new.split('\n') if l+'\n' not in OLD_DECLARATIONS and not (l.startswith('extern "C"') and 'PC FPS patch' in l and l+'\n'!=DECLARATIONS))
        if DECLARATIONS not in new:
            first=new.index('namespace psprecomp');new=new[:first]+DECLARATIONS+new[first:]
    if new==text:return False
    if crlf:new=new.replace('\n','\r\n')
    path.write_bytes(new.encode('utf-8'));return True

def replace_after(text,label,variants,patched,window=400):
    """Replace the first of `variants` (original or an older patch) found right after `label`."""
    if patched in text:return text
    at=text.index(label)+len(label);block=text[at:at+window]
    for original in variants:
        if block.count(original)==1:return text[:at]+block.replace(original,patched,1)+text[at+window:]
    raise RuntimeError(f'{label.strip()} no longer matches')

# --- the two engine getters --------------------------------------------------
def patch_getters(text):
    text=replace_after(text,'L_088EAC44:\n',['    ctx.set_gpr(2, 0u | 2u);\n',
        '    ctx.set_gpr(2, kon_logic_frame_ticks); // PC FPS patch: ticks (1/60 s) per logic frame\n'],
        '    ctx.set_gpr(2, kon_logic_frame_ticks); // PC FPS patch: whole ticks (1/60 s) this logic frame\n')
    return replace_after(text,'L_088EAC3C:\n',['    ctx.set_gpr(2, 0u | 30u);\n',
        '    ctx.set_gpr(2, 60u / kon_logic_frame_ticks); // PC FPS patch: logic frames per second\n'],
        '    ctx.set_gpr(2, kon_logic_fps); // PC FPS patch: logic frames per second\n')

# --- tick getter call sites that are unit conversions (keep 2) -------------------
KEYFRAME_MARK='    ctx.set_gpr(2, 2u); // PC FPS patch: ticks per 30 FPS keyframe (unit conversion)\n'
KEYFRAME_SITES={
    '0033':['0888AF9C','0888AFD8','0888B010','0888B04C'],  # int(time/ticks) -> motion frame
    '0044':['088B62B8','088B66A0'],                          # smoothed animation frame target [+88]
    '0049':['088CA050','088CA080'],                          # motion frame [+268] from tick time
    '0115':['089D0104'],                                     # band/stage animation frame target [+48]
}
def patch_keyframes(addresses):
    def change(text):
        for address in addresses:
            label=f'L_{address}:\n'
            at=text.index(label)+len(label)
            if text.startswith(KEYFRAME_MARK,at):continue
            if '0x088EAC44u' not in text[max(0,at-600):at]:raise RuntimeError(f'{address} is not the return of a 0x088EAC44 call')
            text=text[:at]+KEYFRAME_MARK+text[at:]
        return text
    return change

# --- tick getter call sites that use the ticks as a float: exact fraction --------
FLOAT_SITES={
    '0060':['088F426C','088F4334','088F440C','088F524C'],  # motion playback: frame += ticks*0.5*speed
    '0112':['089C6F30','089C71B8','089C720C','089C746C'],  # per-frame steps scaled by ticks
    '0059':['088F2678'],                                     # global 1/ticks (per-frame step = d/(t*60/ticks))
    '0062':['088FCA50'],                                     # global 1/ticks
}
FLOAT_MARK='// PC FPS patch: fractional ticks'
def patch_float_ticks(addresses):
    def change(text):
        lines=text.split('\n')
        for address in addresses:
            start=lines.index(f'L_{address}:')
            if any(FLOAT_MARK in l for l in lines[start+1:start+18]):continue
            loaded=None
            for n in range(start+1,start+18):
                m=re.match(r'^\s*ctx\.fpr\[(\d+)\] = std::bit_cast<float>\(ctx\.gpr\[2\]\);$',lines[n])
                if m:loaded=m.group(1);continue
                m=re.match(r'^(\s*)ctx\.fpr\[(\d+)\] = static_cast<float>\(static_cast<std::int32_t>\(ctx\.fpr_bits\((\d+)\)\)\);$',lines[n])
                if m and loaded==m.group(3):
                    lines[n]=f'{m.group(1)}ctx.fpr[{m.group(2)}] = kon_logic_frame_ticks_f; {FLOAT_MARK}';break
            else:raise RuntimeError(f'L_{address}: no int-to-float conversion of the ticks')
        return '\n'.join(lines)
    return change

# --- tick getter call sites dividing by the ticks (0x089E67BC/F0: r16 / ticks) ---
DIVIDE_SITES={'0120':['089E67BC','089E67F0']}
DIVIDE_MARK='// PC FPS patch: divide by fractional ticks'
def patch_divides(addresses):
    def change(text):
        lines=text.split('\n')
        for address in addresses:
            start=lines.index(f'L_{address}:');line=lines[start+1]
            if DIVIDE_MARK in line:continue
            if not line.lstrip().startswith('{ const std::int32_t dividend = static_cast<std::int32_t>(ctx.gpr[16]); const std::int32_t divisor = static_cast<std::int32_t>(ctx.gpr[2]);'):
                raise RuntimeError(f'L_{address}: expected r16 / ticks')
            lines[start+1]=f'    ctx.lo = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<float>(static_cast<std::int32_t>(ctx.gpr[16])) / kon_logic_frame_ticks_f)); ctx.hi = 0u; {DIVIDE_MARK}'
        return '\n'.join(lines)
    return change

# --- 2D animation frame += step (frames authored at 30 FPS) -----------------------
def patch_2d_animation(text):
    return replace_after(text,'L_088F9834:\n',['    ctx.set_gpr(4, ctx.gpr[4] + ctx.gpr[6]);\n',
        '    ctx.set_gpr(4, ctx.gpr[4] + (kon_logic_half_frame ? 0u : ctx.gpr[6])); // PC FPS patch: 2D animation frame += step\n'],
        '    ctx.set_gpr(4, ctx.gpr[4] + ctx.gpr[6] * kon_logic_step30); // PC FPS patch: 2D animation frame += step per 30 FPS frame\n')

# --- per-frame +/-1 timers authored in 30 FPS frames ------------------------------
# Measured with tools/instrument_frame_counters.py + classify_frame_counters.py
# (one update per logic frame for long runs).  Excluded: 0087 L_08963E28/E88
# (paired +1/-1 on 12 objects, 1.24 per frame: reference counting, not time).
# 0089 L_0896BB4C [+56] is set to 3 x fps (0x088EAC3C) frames; those sites use
# the original 30 (FPS_AS_30 below) so the 30 FPS count lasts exactly 3 s.
FRAME_TIMERS=[
    ('0060','088F6AAC',40,+1),('0059','088F17E0',3912,+1),('0078','0893CB68',20,+1),
    ('0113','089CA7D4',1352,+1),('0113','089CAE98',1348,+1),('0093','0897877C',104,-1),
    ('0093','0897887C',108,-1),('0090','0896CDD0',0,-1),('0037','0889827C',128,-1),
    ('0037','088982CC',130,-1),('0040','088A5F64',16,-1),('0037','0889845C',74,-1),
    ('0089','0896BB4C',56,-1),('0090','0896CD08',104,-1),('0009','08828F68',16,+1),
    ('0026','0886C280',64,-1),('0026','0886C2D0',64,-1),('0024','08864048',200,-1),
    ('0024','08866770',80,-1),
]
TIMER_MARK='// PC FPS patch: 30 FPS frame timer'
def patch_timers(entries):
    def change(text):
        lines=text.split('\n')
        for _,label,offset,step in entries:
            start=lines.index(f'L_{label}:');sign='+' if step>0 else '-'
            for n in range(start+1,len(lines)):
                line=lines[n]
                if line.startswith('L_'):raise RuntimeError(f'L_{label}: no +/-1 update of offset {offset}')
                old=re.match(r'^(\s*)ctx\.set_gpr\((\d+), ctx\.gpr\[(\d+)\] \+ \(kon_logic_half_frame \? 0u : [^)]*\)?\)\); '+re.escape(TIMER_MARK)+r' \(offset (\d+) \)$',line)
                if TIMER_MARK in line and 'kon_logic_step30' in line and f'(offset {offset} )' in line:break
                m=re.match(r'^(\s*)ctx\.set_gpr\((\d+), ctx\.gpr\[(\d+)\] \+ static_cast<std::uint32_t>\((-?1)\)\);$',line)
                stores=lambda reg:any(f'static_cast<std::uint32_t>({offset}), {cast}ctx.gpr[{reg}]' in l for l in lines[n+1:n+12] for cast in ('','static_cast<std::uint16_t>(','static_cast<std::uint8_t>('))
                if old and int(old.group(4))==offset:m2=(old.group(1),old.group(2),old.group(3))
                elif m and int(m.group(4))==step and stores(m.group(2)):m2=(m.group(1),m.group(2),m.group(3))
                else:continue
                lines[n]=f'{m2[0]}ctx.set_gpr({m2[1]}, ctx.gpr[{m2[2]}] {sign} kon_logic_step30); {TIMER_MARK} (offset {offset} )'
                break
        return '\n'.join(lines)
    return change

# --- `x += (target - x) / 2` smoothing --------------------------------------------
SMOOTH_SITES=[('0115','L_089D01A4:\n',16),('0044','L_088B6760:\n',19)]
def patch_smoothing(sites):
    def change(text):
        for _,label,reg in sites:
            shift=f'static_cast<std::uint32_t>(static_cast<std::int32_t>(ctx.gpr[6]) >> 1u)'
            text=replace_after(text,label,[f'    ctx.set_gpr({reg}, {shift});\n',
                f'    ctx.set_gpr({reg}, kon_logic_frame_ticks == 1u ? kon_smooth_step(ctx.gpr[6]) : {shift}); // PC FPS patch: smoothing\n'],
                f'    ctx.set_gpr({reg}, kon_logic_fps == 30u ? {shift} : kon_smooth_step(ctx.gpr[6])); // PC FPS patch: frame-rate independent smoothing\n')
        return text
    return change

# --- pad auto-repeat (0x089F803C) ------------------------------------------------
# Per button, two byte counters advance while the button is held:
#   [+36+i] = clamp(c+1, 0, 10): repeat A fires when it becomes 1 (the press)
#           and on every frame once it reaches 10 (0.3 s, then 30 per second);
#   [+60+i] = clamp(c+1, 0, 7): repeat B fires at 1 and at 7, then restarts at 2
#           (the press, then every 5 frames).
# Both count 30 FPS frames: the first held frame still counts 1 (no added
# latency), later ones advance by kon_logic_step30, and repeat A at its limit
# fires only on frames where a 30 FPS frame elapsed.  At 30 FPS every value
# equals the original.
PAD_MARK='// PC FPS patch: pad repeat in 30 FPS frames'
def patch_pad_repeat(text):
    if PAD_MARK in text:return text
    lines=text.split('\n')
    def after(label,original,patched):
        start=lines.index(label+':')
        for n in range(start+1,start+6):
            if lines[n]==original:lines[n]=patched;return
        raise RuntimeError(f'{label}: {original.strip()} not found')
    inc='    ctx.set_gpr(4, ctx.gpr[4] + static_cast<std::uint32_t>(1));'
    after('L_089F81F0',inc,f'    kon_pad_hold_before = ctx.gpr[4]; ctx.set_gpr(4, ctx.gpr[4] + (ctx.gpr[4] == 0u ? 1u : kon_logic_step30)); {PAD_MARK}')
    after('L_089F8204','    ctx.set_gpr(4, ctx.gpr[2] & 255u);',f'    ctx.set_gpr(4, ctx.gpr[2] & 255u); kon_pad_repeat_due = ctx.gpr[4] != kon_pad_hold_before || kon_logic_step30 != 0u; {PAD_MARK}')
    after('L_089F8224','    ctx.set_gpr(5, rt.memory().aot_load32(ctx.gpr[21] + static_cast<std::uint32_t>(4)));',f'    ctx.set_gpr(5, kon_pad_repeat_due ? rt.memory().aot_load32(ctx.gpr[21] + static_cast<std::uint32_t>(4)) : 0u); {PAD_MARK}')
    after('L_089F826C',inc,f'    ctx.set_gpr(4, ctx.gpr[4] + (ctx.gpr[4] == 0u ? 1u : kon_logic_step30)); {PAD_MARK}')
    return '\n'.join(lines)

# --- per-frame state functions run at the original 30 FPS cadence --------------
# Pure state updates (no drawing) found with tools/scan_step_sites.py and the
# speed audit: at function entry, on logic frames where no 30 FPS frame elapsed
# (kon_logic_step30 == 0) they return at once, so their counters, steps and
# random draws keep the original rate.  Callers do not use their return value.
GATED_FUNCTIONS=[
    ('0038','0889CA54'),  # club room girl: AI state machine (walk, turn, wait timers)
    ('0036','08894A00'),  # girl blink: random timer, 6-frame blink
    ('0036','08894AE8'),  # girl idle gesture: counter to a random gesture
]
GATE_MARK='// PC FPS patch: 30 FPS state update'
GATE_RETURN=('    if (kon_logic_step30 == 0u) { jump_target = ctx.gpr[31]; local_pc = jump_target; '
             'if (++local_transfers < 2048u) { entry_id = 0u; goto LOCAL_DISPATCH; } ctx.pc = jump_target; return; } '+GATE_MARK)
def patch_gates(addresses):
    def change(text):
        lines=text.split('\n')
        for address in addresses:
            start=lines.index(f'L_{address}:')
            if GATE_MARK in lines[start+1]:continue
            if 'ctx.set_gpr(29, ctx.gpr[29] + static_cast<std::uint32_t>(-' not in lines[start+1]:raise RuntimeError(f'L_{address}: not a function entry')
            lines.insert(start+1,GATE_RETURN)
        return '\n'.join(lines)
    return change

# --- per-frame increments scaled by elapsed time ------------------------------------
# (unit, label, original line, increment expression): `x = x + step` per logic
# frame becomes `x = x + step * ticks/2` (ticks/2 = 30 FPS frames elapsed,
# exactly 1 at 30 FPS).
SCALED_STEPS=[
    ('0060','L_088F4254','    ctx.fpr[12] = ctx.fpr[12] + ctx.fpr[13];','ctx.fpr[13]'),  # motion crossfade weight += rate
]
SCALE_MARK='// PC FPS patch: step per 30 FPS frame'
def patch_scaled(entries):
    def change(text):
        lines=text.split('\n')
        for _,label,original,step in entries:
            start=lines.index(label+':')
            for n in range(start+1,start+12):
                if SCALE_MARK in lines[n]:break
                if lines[n]==original:
                    target=original.split(' = ')[0]
                    lines[n]=f'{target} = {original.split(" = ")[1].split(" + ")[0]} + {step} * (kon_logic_frame_ticks_f * 0.5f); {SCALE_MARK}'
                    break
            else:raise RuntimeError(f'{label}: {original.strip()} not found')
        return '\n'.join(lines)
    return change

# --- other per-frame countdowns -----------------------------------------------------
# (unit, label, original line, patched line): countdowns that are not plain
# load/+-1/store sequences.  They subtract the 30 FPS frames elapsed and stop at
# zero, so the event at zero always fires once, as in the original.
COUNTDOWN_MARK='// PC FPS patch: countdown in 30 FPS frames'
COUNTDOWNS=[
    # Menu delayed-command queue (entries [+76+8i] frames, [+80+8i] command): the
    # side wheel's "play" confirmation waits on it (stuck at 120 FPS otherwise).
    ('0040','L_088A5E64','    ctx.set_gpr(4, ctx.gpr[4] + static_cast<std::uint32_t>(-1));',
     '    ctx.set_gpr(4, ctx.gpr[4] > kon_logic_step30 ? ctx.gpr[4] - kon_logic_step30 : 0u);'),
    # Screen transition (0x08881xxx): [+4] counts down to -1, then [+12] advances
    # (first wait, then 10 frames).  Only the sign is tested, so it may pass 0.
    ('0031','L_088813A0','    ctx.set_gpr(4, ctx.gpr[4] + static_cast<std::uint32_t>(-1));','    ctx.set_gpr(4, ctx.gpr[4] - kon_logic_step30);'),
    ('0031','L_088813C8','    ctx.set_gpr(4, ctx.gpr[4] + static_cast<std::uint32_t>(-1));','    ctx.set_gpr(4, ctx.gpr[4] - kon_logic_step30);'),
    # Club room girl behaviour wait [+172] (0x088A0764, counted down to 0).
    ('0039','L_088A0764','    ctx.set_gpr(4, ctx.gpr[4] + static_cast<std::uint32_t>(-1));','    ctx.set_gpr(4, ctx.gpr[4] - kon_logic_step30);'),
    # Story scene element (0x088AFEA0): [+12] waits down to 0.
    ('0043','L_088B1970','    ctx.set_gpr(4, ctx.gpr[4] + static_cast<std::uint32_t>(-1));','    ctx.set_gpr(4, ctx.gpr[4] - kon_logic_step30);'),
]
def patch_countdowns(entries):
    def change(text):
        lines=text.split('\n')
        for _,label,original,patched in entries:
            start=lines.index(label+':')
            for n in range(start+1,start+12):
                if COUNTDOWN_MARK in lines[n]:break
                if lines[n]==original:lines[n]=f'{patched} {COUNTDOWN_MARK}';break
            else:raise RuntimeError(f'{label}: {original.strip()} not found')
        return '\n'.join(lines)
    return change

# --- logic FPS read as a duration unit for a 30 FPS timer -----------------------
# 0x088EAC3C returns the current logic FPS (it changes with load).  These sites
# turn it into a wait for a timer counted in 30 FPS frames, so they use 30.
FPS30_MARK='// PC FPS patch: 30 FPS frames for a 30 FPS timer'
FPS_AS_30=[('0089',label) for label in ('L_0896B1E4','L_0896B980','L_0896BC80')]  # [+56] = 3 s
def patch_fps_as_30(labels):
    def change(text):
        lines=text.split('\n')
        for label in labels:
            start=lines.index(label+':')
            if FPS30_MARK in lines[start+1]:continue
            if lines[start+1]!='    ctx.set_gpr(4, ctx.gpr[2] + ctx.gpr[2]);':raise RuntimeError(f'{label}: unexpected use of the FPS getter')
            lines.insert(start+1,f'    ctx.set_gpr(2, 30u); {FPS30_MARK}')
        return '\n'.join(lines)
    return change

def main():
    changed=[]
    def run(unit,change):
        if edit(unit,change):changed.append(unit)
    run('0057',patch_getters)
    for unit,addresses in KEYFRAME_SITES.items():run(unit,patch_keyframes(addresses))
    for unit,addresses in FLOAT_SITES.items():run(unit,patch_float_ticks(addresses))
    for unit,addresses in DIVIDE_SITES.items():run(unit,patch_divides(addresses))
    run('0061',patch_2d_animation)
    for unit in sorted({t[0] for t in FRAME_TIMERS}):run(unit,patch_timers([t for t in FRAME_TIMERS if t[0]==unit]))
    for unit in sorted({s[0] for s in SMOOTH_SITES}):run(unit,patch_smoothing([s for s in SMOOTH_SITES if s[0]==unit]))
    run('0125',patch_pad_repeat)
    run('0089',patch_fps_as_30([l for _,l in FPS_AS_30]))
    for unit in sorted({c[0] for c in COUNTDOWNS}):run(unit,patch_countdowns([c for c in COUNTDOWNS if c[0]==unit]))
    for unit in sorted({s[0] for s in SCALED_STEPS}):run(unit,patch_scaled([s for s in SCALED_STEPS if s[0]==unit]))
    for unit in sorted({g[0] for g in GATED_FUNCTIONS}):run(unit,patch_gates([g[1] for g in GATED_FUNCTIONS if g[0]==unit]))
    corpus=' '.join(p.read_text(encoding='utf-8') for p in CORPUS.glob('generated_unit_*.cpp'))
    if 'kon_logic_half_frame' in corpus:raise RuntimeError('stale 60 FPS-only patch left behind')
    counts={'fps30':corpus.count(FPS30_MARK),'countdowns':corpus.count(COUNTDOWN_MARK),'scaled':corpus.count(SCALE_MARK),'gates':corpus.count(GATE_MARK),'pad':corpus.count(PAD_MARK),'timers':corpus.count(TIMER_MARK),'float':corpus.count(FLOAT_MARK),'divide':corpus.count(DIVIDE_MARK),'keyframe':corpus.count(KEYFRAME_MARK.strip())}
    expected={'fps30':len(FPS_AS_30),'countdowns':len(COUNTDOWNS),'scaled':len(SCALED_STEPS),'gates':len(GATED_FUNCTIONS),'pad':4,'timers':len(FRAME_TIMERS),'float':sum(map(len,FLOAT_SITES.values())),'divide':sum(map(len,DIVIDE_SITES.values())),'keyframe':sum(map(len,KEYFRAME_SITES.values()))}
    if counts!=expected:raise RuntimeError(f'patch counts {counts} != {expected}')
    print('patched units:',', '.join(sorted(set(changed))) if changed else 'none (already patched)',counts)

if __name__=='__main__':main()
