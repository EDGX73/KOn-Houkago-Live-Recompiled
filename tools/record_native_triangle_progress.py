"""Record flat-triangle GPU tests, controlled synthetic timing and original scene limits."""
from pathlib import Path
from datetime import datetime,timezone
import json,hashlib,re,struct
root=Path(__file__).resolve().parents[1]
def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def read(p):return json.loads((root/p).read_text(encoding='utf-8-sig'))
bench=root/'build/native_full/benchmark_native_triangles.exe'
blog=root/'analysis/native_flat_triangle_benchmark.log'
if 'tested_exe_sha256='+sha(bench) not in blog.read_text(encoding='utf-8-sig'):raise RuntimeError('Benchmark log belongs to another binary')
timing={'checked_at':datetime.now(timezone.utc).isoformat(),'benchmark_executable_sha256':sha(bench),'log':str(blog.relative_to(root)),'synthetic_only':True,'default_enabled':False,'whole_game_fps_gain_verified':False,'conditions':'480 x 272, flat colors, no textures/depth/blend, 4 CPU lanes, alternating CPU/GPU; 40 repetitions plus 4 warmup','cases':[]}
for pairs in (1,16):
    runs=[]
    for trial in ('a','b','c'):
        for backend in ('cpu','gpu'):
            name=f'analysis/native_flat_triangle_{backend}_{pairs}_{trial}.json';r=read(name)
            if not r['cpu_reference_vram_exact'] or r['triangles']!=pairs*2:raise RuntimeError('Synthetic exactness not verified')
            if backend=='gpu' and r['actual_gpu_triangle_submissions']!=44:raise RuntimeError('No actual GPU benchmark work')
            runs.append({'trial':trial,'backend':backend,'record':name,'record_sha256':sha(root/name),'median_us':r['median_us'],'p95_us':r['p95_us']})
    timing['cases'].append({'triangles':pairs*2,'runs':runs})
(root/'analysis/native_flat_triangle_timing_verification.json').write_text(json.dumps(timing,indent=2)+'\n')
name='analysis/native_song_flat_triangle_gpu_en.log';log_path=root/name;log=log_path.read_text(encoding='utf-8-sig')
exe=root/'build/native_full/kon_boot_probe.exe';digest=sha(exe)
if 'tested_exe_sha256='+digest not in log:raise RuntimeError('Original scene belongs to another game executable')
rows=[]
for line in log.splitlines():
    if line.startswith('native_progress '):rows.append({k:int(v) for k,v in re.findall(r'(elapsed_ms|submitted_frames|render_clear_us|render_2d_us|render_3d_us|gpu_triangle_primitives|gpu_triangle_pixels)=(\d+)',line)})
start=next(r for r in rows if r['elapsed_ms']==110084);end=next(r for r in rows if r['elapsed_ms']==115085)
profile_name='analysis/native_song_flat_triangle_gpu_profile.json';profile=read(profile_name)
projected=sorted([r for r in profile['draws'] if r['key'][0]==2],key=lambda r:r['total_us'],reverse=True)
f=root/'analysis/native_song_stage_textured_en.kondraw'
with f.open('rb') as data:
    header=struct.unpack('<8I',data.read(32));registers=struct.unpack('<256I',data.read(1024))
if header[0]!=0x444E4F4B or header[2]+header[3]+header[4]+32!=f.stat().st_size:raise RuntimeError('3D snapshot framing invalid')
user_save=root/'userdata/ms0/PSP/SAVEDATA/ULJM05709SYSTEM00/SYSTEM.DAT.konpc'
if sha(user_save)!='6b8ceccd250ffb2216edd783d619e705bfcc467548ed3dce063e0af19179af62':raise RuntimeError('User save changed')
tri_calls=int(re.findall(r'native_vulkan_triangle_submissions=(\d+)',log)[-1])
result={'checked_at':datetime.now(timezone.utc).isoformat(),'executable_sha256':digest,'log':name,'log_sha256':sha(log_path),'runtime_limit_ms':145000,'option':'--vulkan-triangles','actual_gpu_triangle_submissions':tri_calls,'original_gpu_triangle_route_used':tri_calls>0,'renderer_flat_triangle_test':'analysis/native_vulkan_triangle_verification.json','synthetic_diagnostic_only':True,'userdata':'work/song_regression_poll_20261005/ms0','user_save_unchanged':True,'input_replay':'work/input_hotchkiss_saved_menu_fast.txt','audio_input_replay':'work/input_hotchkiss_chart_first_note_anchor.txt','stop':re.findall(r'worker_stop=([^\n]+)',log)[-1],'masked_context_switches':int(re.findall(r'native_masked_context_switches=(\d+)',log)[-1]),'interrupt_restore_anomalies':len(re.findall(r'InterruptRestoreAnomaly elapsed_us=',log)),'full_song_verified':False,'physical_controls_verified':False,'music_auditioned':False,'sync_verified':False,'game_fps_gain_verified':False,'fps_observation':{'start_elapsed_ms':start['elapsed_ms'],'end_elapsed_ms':end['elapsed_ms'],'start_frames':start['submitted_frames'],'end_frames':end['submitted_frames'],'actual_submitted_frames_per_second':(end['submitted_frames']-start['submitted_frames'])*1000/(end['elapsed_ms']-start['elapsed_ms']),'clear_us':end['render_clear_us']-start['render_clear_us'],'through_us':end['render_2d_us']-start['render_2d_us'],'projected_us':end['render_3d_us']-start['render_3d_us'],'controlled_comparison':False},'visual_observations':[{'file':'analysis/native_song_flat_triangle_gpu_en_110084ms.bmp','observed':'original stage and notes'},{'file':'analysis/native_song_flat_triangle_gpu_en_115085ms.bmp','observed':'notes and Tension Down'},{'file':'analysis/native_song_flat_triangle_gpu_en_120086ms.bmp','observed':'GAME OVER'},{'file':'analysis/native_song_flat_triangle_gpu_en.bmp','observed':'NO RANK / TOO BAD; 11 Miss, score 0; successful full song not verified'}],'profile':{'file':profile_name,'sha256':sha(root/profile_name),'entries':len(profile['draws']),'unrecorded_draws':profile['unrecorded_draws'],'top_projected_draws':projected[:10],'key_fields':['category','kind','count','vertex_type','color_format','texture','blend','depth_test','alpha_test','texture_format','filter','texture_function','blend_function','depth_function']},'snapshot':{'file':str(f.relative_to(root)),'sha256':sha(f),'size_bytes':f.stat().st_size,'capture':'Before draw, after 105 seconds, first projected primitive with at least 64 vertices','pc':header[5],'kind':header[6],'vertices':header[7],'registers':{hex(n):hex(registers[n]&0xFFFFFF) for n in (0x12,0x1e,0x21,0x22,0x23,0xc3,0xc6,0xc9,0xdb,0xde,0xdf)},'capture_is_full_song_proof':False},'next_gpu_requirements':['indexed textures and bilinear sampling','perspective UV and vertex colors','alpha/depth comparisons, blend and RGB565 packing','grouped GE draws and resident GPU destinations'], 'source_sha256':{n:sha(root/n) for n in ['src/native_vulkan_clear.cpp','src/native_renderer.hpp','src/boot_probe.cpp','tools/create_vulkan_triangle_shader.py']}}
(root/'analysis/native_song_flat_triangle_gpu_en.json').write_text(json.dumps(result,indent=2)+'\n')
print(f'Recorded real scene: {tri_calls} GPU triangle submissions; {result["fps_observation"]["actual_submitted_frames_per_second"]:.2f} observed FPS; full song and GPU FPS gain pending')
