"""Capture evidence for the original textured stage draw; CPU reference only."""
from pathlib import Path
from datetime import datetime,timezone
import json,hashlib,re,struct
root=Path(__file__).resolve().parents[1]
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def load(p):return json.loads((root/p).read_text(encoding='utf-8-sig'))
exe=root/'build/native_full/kon_boot_probe.exe';log_path=root/'analysis/native_song_stage_gpu_capture_en.log';log=log_path.read_text(encoding='utf-8-sig');digest=sha(exe)
if 'tested_exe_sha256='+digest not in log:raise RuntimeError('Capture game executable hash mismatch')
ref=root/'analysis/native_song_stage_main_cpu_reference.vram.bin';replay_log_path=root/'analysis/native_song_stage_main_cpu_reference.log';replay_log=replay_log_path.read_text(encoding='utf-8-sig')
replay_exe=root/'build/native_full/replay_native_draw.exe'
if 'tested_exe_sha256='+sha(replay_exe) not in replay_log:raise RuntimeError('Reference executable mismatch')
coverage=re.search(r'covered=(\d+) drawn=(\d+)',replay_log)
if not coverage or min(int(n) for n in coverage.groups())<=0 or ref.stat().st_size!=2097152:raise RuntimeError('Captured draw lacks nonempty exact VRAM reference')
capture=root/'analysis/native_song_stage_main_textured_en.kondraw'
with capture.open('rb') as f:
    header=struct.unpack('<8I',f.read(32));r=struct.unpack('<256I',f.read(1024))
if header[0]!=0x444E4F4B or header[7]<3000 or header[2]+header[3]+header[4]+32!=capture.stat().st_size:raise RuntimeError('Representative snapshot mismatch')
profile_name='analysis/native_song_stage_gpu_capture_profile.json';profile=load(profile_name)
rows=[]
for line in log.splitlines():
    if line.startswith('native_progress '):rows.append({k:int(v) for k,v in re.findall(r'(elapsed_ms|submitted_frames|render_clear_us|render_2d_us|render_3d_us)=(\d+)',line)})
start=min(rows,key=lambda x:abs(x['elapsed_ms']-110000));end=min(rows,key=lambda x:abs(x['elapsed_ms']-115000))
if not 4900<end['elapsed_ms']-start['elapsed_ms']<5300:raise RuntimeError('Expected diagnostic stage timing segment missing')
user_save=root/'userdata/ms0/PSP/SAVEDATA/ULJM05709SYSTEM00/SYSTEM.DAT.konpc'
if sha(user_save)!='6b8ceccd250ffb2216edd783d619e705bfcc467548ed3dce063e0af19179af62':raise RuntimeError('User save changed')
result={'checked_at':datetime.now(timezone.utc).isoformat(),'executable_sha256':digest,'log':str(log_path.relative_to(root)),'log_sha256':sha(log_path),'stop':re.findall(r'worker_stop=([^\n]+)',log)[-1],'runtime_limit_ms':145000,'synthetic_diagnostic_only':True,'userdata':'work/song_regression_poll_20261005/ms0','user_save_unchanged':True,'actual_gpu_triangle_submissions':int(re.findall(r'native_vulkan_triangle_submissions=(\d+)',log)[-1]),'masked_context_switches':int(re.findall(r'native_masked_context_switches=(\d+)',log)[-1]),'interrupt_restore_anomalies':len(re.findall(r'InterruptRestoreAnomaly elapsed_us=',log)),'game_fps_gain_verified':False,'textured_gpu_execution_verified':False,'full_song_verified':False,'music_sync_verified':False,'physical_input_verified':False,'fps_observation':{'start_elapsed_ms':start['elapsed_ms'],'end_elapsed_ms':end['elapsed_ms'],'frames':end['submitted_frames']-start['submitted_frames'],'fps':(end['submitted_frames']-start['submitted_frames'])*1000/(end['elapsed_ms']-start['elapsed_ms']),'controlled_comparison':False},'profile':{'file':profile_name,'sha256':sha(root/profile_name),'entries':len(profile['draws']),'unrecorded_draws':profile['unrecorded_draws'],'top_projected_draws':sorted([x for x in profile['draws'] if x['key'][0]==2],key=lambda x:x['total_us'],reverse=True)[:10]},'snapshot':{'file':str(capture.relative_to(root)),'sha256':sha(capture),'capture_before_draw':True,'pc':header[5],'kind':header[6],'vertices':header[7],'registers':{hex(n):hex(r[n]&0xFFFFFF) for n in (0x12,0x1e,0x21,0x22,0x23,0xc3,0xc6,0xc9,0xdb,0xde,0xdf)}},'cpu_reference':{'file':str(ref.relative_to(root)),'sha256':sha(ref),'bytes':ref.stat().st_size,'covered_pixels':int(coverage[1]),'drawn_pixels':int(coverage[2]),'replay_executable_sha256':sha(replay_exe),'log':str(replay_log_path.relative_to(root)),'timing':'analysis/native_song_stage_main_cpu_timing.json','bmp':'analysis/native_song_stage_main_cpu_reference.bmp','gpu_exactness_verified':False},'source_sha256':{n:sha(root/n) for n in ['src/native_renderer.hpp','src/boot_probe.cpp','src/replay_native_draw.cpp']}}
(root/'analysis/native_song_stage_gpu_capture_en.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'Representative original textured draw: {header[7]} vertices, {coverage[1]} covered and {coverage[2]} drawn pixels; CPU reference stored; GPU migration pending')
