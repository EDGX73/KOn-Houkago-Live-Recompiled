"""One bounded visible AMD trial; prior measurements are retained, not rerun."""
from pathlib import Path
import argparse,subprocess,hashlib,json,re,shutil,uuid
from datetime import datetime,timezone
from benchmark_native_game import measure,save_hashes
ROOT=Path(__file__).resolve().parents[1]
# Fixed test save (Hotchkiss reference, SYSTEM.DAT sha256 6b8ceccd...): the synthetic menu
# inputs were authored for it. The player's own userdata/ms0 keeps changing and is never used.
TEST_SEED=ROOT/'work/test_seed_hotchkiss_20261005/ms0'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--name',default='cache_20261005');parser.add_argument('--runtime-ms',type=int,default=135000);parser.add_argument('--verify-frames',type=int,default=0);parser.add_argument('--exe',type=Path,default=ROOT/'build/native_full/kon_boot_probe.exe');parser.add_argument('--extended-window-end',type=int,default=0);parser.add_argument('--logic-vblank-cycles',type=int,default=0);parser.add_argument('--logic-fps',type=int,default=0);parser.add_argument('--extra',default='',help='extra game options, space separated');parser.add_argument('--language',default='en',choices=('en','ja'));parser.add_argument('--saved-fps',type=int,default=0,help='pre-write logic_fps into the private settings file');parser.add_argument('--menu-input',type=Path,default=None,help='menu input replay (default: Hotchkiss song path)');parser.add_argument('--verify-after-ms',type=int,default=124000);parser.add_argument('--audible',action='store_true',help='with --visible: play the audio (only when the user allows it)');parser.add_argument('--visible',action='store_true',help='show the window (audio stays muted; default: off-screen window)');parser.add_argument('--ram-snapshots',nargs=3,metavar=('DIR','START_MS','COUNT'));parser.add_argument('--seed',type=Path,default=None,help='ms0 folder copied privately instead of the Hotchkiss test save (never modified)');parser.add_argument('--saved-language',choices=('en','ja'),default=None,help='pre-write language into the private settings file and start without --language');parser.add_argument('--setting',action='append',default=[],metavar='KEY=VALUE',help='append a line to the private settings file');args=parser.parse_args()
 if not args.name.replace('_','').isalnum() or not 30000<=args.runtime_ms<=360000 or not 0<=args.verify_frames<=4:raise RuntimeError('Invalid bounded probe')
 exe=args.exe.resolve()
 if ROOT/'build' not in exe.parents or exe.name!='kon_boot_probe.exe' or not 0<=args.extended_window_end<=40:raise RuntimeError('Probe executable must be a kon_boot_probe.exe under build/')
 digest=sha(exe);seed=args.seed.resolve() if args.seed else TEST_SEED;storage=ROOT/'work'/('amd_probe_'+uuid.uuid4().hex)/'ms0';shutil.copytree(seed,storage)
 menu=args.menu_input.resolve() if args.menu_input else ROOT/'work/input_hotchkiss_saved_menu_gpu_paced.txt';chart=ROOT/'work/input_hotchkiss_chart_first_note_anchor.txt'
 stem=ROOT/'analysis'/('native_visible_vulkan_resident_'+args.name+'_amd_'+args.language);log=stem.with_suffix('.log');profile=Path(str(stem)+'_frames.json');report=stem.with_suffix('.json')
 cmd=[str(exe),'--settings-file',str(storage.parent/'settings.ini'),'--root',str(ROOT),'--language',args.language,'--bootstrap','--show-test-window','--userdata',str(storage),'--input-replay',str(menu),'--audio-anchored-input-replay',str(chart),'--input-anchor-channel','3','--vulkan','--vulkan-resident-batch','--gpu-adapter','amd','--raster-threads','16','--max-runtime-ms',str(args.runtime_ms),'--max-dispatches','1000000000','--capture-frame',str(stem)+'.bmp','--capture-interval-ms','1000','--profile-frames',str(profile)]
 # Test audio is always muted (the user's request); --visible only shows the window.
 if not (args.visible and args.audible):cmd.append('--mute-audio' if args.visible else '--silent-test')
 if args.ram_snapshots:cmd.extend(['--ram-snapshots',*args.ram_snapshots])
 if args.saved_fps:(storage.parent/'settings.ini').write_text(f'logic_fps={args.saved_fps}\n')
 if args.saved_language:
  with (storage.parent/'settings.ini').open('a') as f:f.write(f'language={args.saved_language}\n')
 if args.setting:
  with (storage.parent/'settings.ini').open('a') as f:f.write(''.join(f'{line}\n' for line in args.setting))
  at=cmd.index('--language');del cmd[at:at+2]
 if args.extra:cmd.extend(args.extra.split())
 if args.logic_fps:cmd.extend(['--logic-fps',str(args.logic_fps)])
 if args.logic_vblank_cycles:cmd.extend(['--logic-vblank-cycles',str(args.logic_vblank_cycles)])
 if args.verify_frames:cmd.extend(['--verify-render-frames',str(args.verify_frames),'--verify-render-after-ms',str(args.verify_after_ms)])
 record={'created_at':datetime.now(timezone.utc).isoformat(),'executable_sha256':digest,'arguments':cmd,'requested_gpu_adapter':'amd','initial_save_files_sha256':save_hashes(storage),'input_files_sha256':{str(p.relative_to(ROOT)):sha(p) for p in [menu,chart]},'log':str(log.relative_to(ROOT)),'frame_profile':str(profile.relative_to(ROOT)),'full_song_verified':False,'complete_gpu_renderer':False,'arbitrary_fps_verified':False,'synthetic_input':True,'runtime_ms':args.runtime_ms}
 def write():report.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
 write();print('Starting one '+('visible' if args.visible else 'silent')+' AMD trial sha256='+digest,flush=True)
 with log.open('w',encoding='utf-8') as output:
  output.write('tested_exe_sha256='+digest+'\n');output.flush();result=subprocess.run(cmd,cwd=ROOT,stdout=output,stderr=subprocess.STDOUT);output.write('\nprocess_exit_code='+str(result.returncode)+'\n')
 text=log.read_text(encoding='utf-8-sig',errors='replace');adapter=re.search(r'^native_vulkan_clear_adapter=([^\r\n]+)',text,re.M);record['selected_gpu_adapter']=adapter[1].split(' (experimental;')[0] if adapter else None
 record['exit_code']=result.returncode;stop=re.search(r'^worker_stop=([^\r\n]+)',text,re.M);record['diagnostic_stop']=stop[1] if stop else None
 record['verified_private_frames']=len(re.findall(r'^native_frame_replay_verified ',text,re.M));write()
 if result.returncode!=2 or not record['selected_gpu_adapter'] or 'AMD Radeon 780M' not in record['selected_gpu_adapter']:raise RuntimeError('AMD game trial did not use the requested adapter')
 data=json.loads(profile.read_text());record['audio_anchor_us']=data['audio_anchor_us']
 try:record['song_window']=measure(data)
 except RuntimeError as error:record['measurement_error']=str(error);write();raise
 if args.extended_window_end:record['extended_song_window']=measure(data,1,args.extended_window_end)
 if args.verify_frames:record['measurement_includes_private_replay']=True
 else:
  prior=ROOT/'analysis/native_performance_vulkan_resident_amd_20261005.json'
  if prior.exists():
   baseline=json.loads(prior.read_text());row=next((r for r in baseline['runs'] if r['label']=='candidate'),None)
   if baseline.get('comparison_completed') and row and baseline['initial_save_files_sha256']==record['initial_save_files_sha256'] and baseline['input_files_sha256']==record['input_files_sha256']:
    record['retained_amd_reference']={'report':str(prior.relative_to(ROOT)),'report_sha256':sha(prior),'executable_sha256':row['executable_sha256'],'song_window':row['song_window']}
    record['change_from_retained_reference_percent']=(record['song_window']['fps']/row['song_window']['fps']-1)*100
    record['comparison_design']='One fresh AMD trial against a retained earlier AMD measurement; not a repeated alternating comparison'
 if sha(exe)!=digest:raise RuntimeError('Executable changed during trial')
 write();print(json.dumps({'song_window':record['song_window'],'retained_reference_change_percent':record.get('change_from_retained_reference_percent'),'verified_private_frames':record['verified_private_frames']},ensure_ascii=False),flush=True)
if __name__=='__main__':main()
