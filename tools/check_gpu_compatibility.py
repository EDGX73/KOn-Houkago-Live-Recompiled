"""Run the same native GPU correctness tests on explicitly selected laptop adapters."""
from pathlib import Path
import os,subprocess,json,hashlib,argparse
from datetime import datetime,timezone
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--adapters',nargs='+',choices=['amd','nvidia'],default=['amd']);args=parser.parse_args()
 suffix='_'.join(args.adapters);record_path=ROOT/('analysis/native_gpu_compatibility_'+suffix+'_active_20261005.json')
 build=ROOT/'build/native_full';out=ROOT/('analysis/gpu_compatibility_'+suffix+'_active_20261005');out.mkdir(exist_ok=True)
 record={'checked_at':datetime.now(timezone.utc).isoformat(),'full_song_verified':False,'stable_fps_verified':False,'complete_gpu_renderer':False,'adapters':{},'failures':[]}
 sources=['src/native_gpu_selection.hpp','src/native_vulkan_clear.cpp','src/native_renderer.hpp','src/native_texture.hpp','src/native_frame_stream.hpp','src/native_render_timeline.hpp','src/native_vram_ownership.hpp','third_party/PSPRecomp/src/guest_memory.cpp','src/shaders/native_raster.comp']
 record['sources_sha256']={p:sha(ROOT/p) for p in sources}
 for selector in args.adapters:
  expected={'amd':'AMD Radeon 780M','nvidia':'NVIDIA GeForce RTX 5050'}[selector]
  folder=out/selector;folder.mkdir(exist_ok=True);env=os.environ.copy();env['KON_VULKAN_ADAPTER']=selector;rows=[]
  for name,inputs in [('native_vulkan_shader_test',[build/'native_shaders/native_gpu_clear.spv']),('native_vulkan_renderer_test',[]),('native_vulkan_triangle_test',[]),('native_vulkan_raster_test',[ROOT])]:
   exe=build/(name+'.exe');report=folder/(name+'_verification.json');log=folder/(name+'.log');digest=sha(exe)
   print('Running',selector,name,flush=True)
   result=subprocess.run([str(exe),*[str(i) for i in inputs],str(report)],cwd=ROOT,env=env,text=True,capture_output=True)
   log.write_text('tested_exe_sha256='+digest+'\nrequested_adapter='+selector+'\n'+result.stdout+result.stderr+'\nprocess_exit_code='+str(result.returncode)+'\n',encoding='utf-8')
   row={'test':name,'executable_sha256':digest,'exit_code':result.returncode,'log':str(log.relative_to(ROOT)),'report':str(report.relative_to(ROOT))}
   if result.returncode==0:
    proof=json.loads(report.read_text());row['selected_adapter']=proof['selected_adapter']
    if expected not in proof['selected_adapter']:raise RuntimeError('Requested GPU was silently substituted')
    row['verified']=True;row['full_vram_comparisons']=proof.get('full_vram_comparisons')
    if selector==args.adapters[0]:
     stem=name.replace('_test','_verification');(ROOT/'analysis'/(stem+'.json')).write_text(report.read_text(),encoding='utf-8');(ROOT/'analysis'/(name+'.log')).write_text(log.read_text(),encoding='utf-8')
   else:
    row['verified']=False;record['failures'].append({'selector':selector,'test':name,'error':result.stderr[-2000:]});print(result.stderr[-2000:],flush=True)
   rows.append(row)
   record['adapters'][selector]={'tests':rows,'all_correctness_tests_passed':len(rows)==4 and all(x['verified'] for x in rows)}
   record_path.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
   print(selector,name,'PASS' if row['verified'] else 'FAIL',row.get('selected_adapter'),flush=True)
 # An explicit unavailable adapter must fail rather than run on another GPU.
 env=os.environ.copy();env['KON_VULKAN_ADAPTER']='kon_missing_gpu_validation'
 failed=subprocess.run([str(build/'native_vulkan_renderer_test.exe'),str(out/'unavailable.json')],env=env,cwd=ROOT,text=True,capture_output=True)
 record['unavailable_selector_rejected']=failed.returncode!=0 and 'matches requested selector' in failed.stderr
 if not record['unavailable_selector_rejected']:raise RuntimeError('Explicit missing GPU did not fail clearly')
 record['all_requested_adapters_correctness_verified']=all(a['all_correctness_tests_passed'] for a in record['adapters'].values())
 record_path.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 if record['failures']:raise SystemExit(1)
 print('PASS: native GPU correctness on '+', '.join(args.adapters)+'; gameplay performance remains separate',flush=True)
if __name__=='__main__':main()
