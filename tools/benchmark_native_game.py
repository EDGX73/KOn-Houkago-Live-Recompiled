"""Private A/B performance runs of real native game code with identical save/input.
Measures delivered buffers near the same original song-audio anchor, never WM_PAINT.
The diagnostic exit and synthetic inputs do not certify gameplay completion or sync.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
TEST_SEED = ROOT / 'work/test_seed_hotchkiss_20261005/ms0'  # fixed test save; never the player's userdata

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def save_hashes(folder):
    return {str(p.relative_to(folder)):sha(p) for p in sorted(folder.rglob('*')) if p.is_file()}

def measure(profile, start_seconds=1, end_seconds=5):
    anchor=profile['audio_anchor_us']
    if anchor is None:
        raise RuntimeError('Original song audio did not start')
    samples=profile['samples']
    requested=[anchor+int(start_seconds*1e6),anchor+int(end_seconds*1e6)]
    if not samples or samples[-1][0]<requested[-1]:
        raise RuntimeError('Run ended before the requested song window')
    lo,hi=[min(samples,key=lambda p:abs(p[0]-target)) for target in requested]
    if any(abs(p[0]-target)>100000 for p,target in zip([lo,hi],requested)):
        raise RuntimeError('Insufficient sampling near the requested song phase')
    duration=hi[0]-lo[0]
    return {'requested_phase_seconds':[start_seconds,end_seconds],
            'actual_phase_seconds':[(lo[0]-anchor)/1e6,(hi[0]-anchor)/1e6],
            'duration_us':duration,'submitted_frames':hi[1]-lo[1],
            'fps':(hi[1]-lo[1])*1e6/duration,
            'renderer_2d_us':hi[2]-lo[2],'renderer_3d_us':hi[3]-lo[3],
            **({'main_thread_mcycles_per_frame':(hi[4]-lo[4])/1e6/max(hi[1]-lo[1],1),
                'gpu_collect_wait_us_per_frame':(hi[5]-lo[5])/max(hi[1]-lo[1],1)} if len(lo)>5 else {})}

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--baseline',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--name',required=True)
    parser.add_argument('--pairs',type=int,default=2)
    parser.add_argument('--start-with-candidate',action='store_true')
    parser.add_argument('--vulkan',action='store_true')
    parser.add_argument('--gpu-adapter')
    parser.add_argument('--show-test-window',action='store_true')
    parser.add_argument('--baseline-show-test-window',action='store_true')
    parser.add_argument('--baseline-vulkan-raster',action='store_true')
    parser.add_argument('--baseline-vulkan-batch',action='store_true')
    parser.add_argument('--candidate-vulkan-resident-batch',action='store_true')
    parser.add_argument('--runtime-ms',type=int,default=145000)
    parser.add_argument('--menu-input',type=Path)
    parser.add_argument('--baseline-raster-threads',type=int)
    parser.add_argument('--candidate-raster-threads',type=int,default=4)
    parser.add_argument('--candidate-vulkan-raster',action='store_true')
    parser.add_argument('--candidate-vulkan-batch',action='store_true')
    args=parser.parse_args()
    if not 30000<=args.runtime_ms<=300000:raise RuntimeError('Invalid bounded runtime')
    if args.baseline_raster_threads is not None and not 1<=args.baseline_raster_threads<=16:raise RuntimeError('Invalid baseline worker count')
    if not 1<=args.candidate_raster_threads<=16:raise RuntimeError('Invalid candidate worker count')
    if not args.name.replace('_','').isalnum() or not 1<=args.pairs<=3:
        raise RuntimeError('Invalid bounded test name or repeat count')
    exe_paths={label:(path if path.is_absolute() else ROOT/path) for label,path in [('baseline',args.baseline),('candidate',args.candidate)]}
    expected_exes={label:sha(path) for label,path in exe_paths.items()}
    session=ROOT/'work'/('perf_'+args.name+'_'+uuid.uuid4().hex)
    seed=session/'seed'/'ms0';shutil.copytree(TEST_SEED,seed)
    seed_hashes=save_hashes(seed)
    report_path=ROOT/'analysis'/('native_performance_'+args.name+'.json')
    report={'created_at':datetime.now(timezone.utc).isoformat(),'same_initial_save':True,
            'initial_save_files_sha256':seed_hashes,'seed_directory':str(seed.relative_to(ROOT)),
            'headless':not args.show_test_window,'synthetic_input':True,'normal_speed_independently_verified':False,
            'multi_vblank_corrected_in_both':True,'start_with_candidate':args.start_with_candidate,'vulkan_enabled':args.vulkan,'requested_gpu_adapter':args.gpu_adapter,
            'complete_gpu_renderer':False,'full_song_verified':False,'candidate_raster_threads':args.candidate_raster_threads,'candidate_vulkan_raster':args.candidate_vulkan_raster,'candidate_vulkan_batch':args.candidate_vulkan_batch,'candidate_vulkan_resident_batch':args.candidate_vulkan_resident_batch,'baseline_vulkan_raster':args.baseline_vulkan_raster,'baseline_vulkan_batch':args.baseline_vulkan_batch,'baseline_visible':args.baseline_show_test_window,'runs':[]}
    menu=args.menu_input or ROOT/'work/input_hotchkiss_saved_menu_paced.txt'
    if not menu.is_absolute():menu=ROOT/menu
    chart=ROOT/'work/input_hotchkiss_chart_first_note_anchor.txt'
    report['input_files_sha256']={str(p.relative_to(ROOT)):sha(p) for p in [menu,chart]}
    def record():report_path.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    record()
    for pair in range(args.pairs):
        for label in (['baseline','candidate'] if (pair+int(args.start_with_candidate))%2==0 else ['candidate','baseline']):
            exe=exe_paths[label]
            if sha(exe)!=expected_exes[label]:raise RuntimeError('Executable changed during experiment')
            name=f'{args.name}_{label}_{pair+1}_en';stem=ROOT/'analysis'/('native_performance_'+name)
            storage=session/name/'ms0';shutil.copytree(seed,storage)
            if save_hashes(storage)!=seed_hashes:raise RuntimeError('Initial save differs')
            frame_profile=Path(str(stem)+'_frames.json');log=Path(str(stem)+'.log')
            command=[str(exe),'--root',str(ROOT),'--language','en','--bootstrap','--userdata',str(storage),
                     '--input-replay',str(menu),'--audio-anchored-input-replay',str(chart),
                     '--input-anchor-channel','3','--max-runtime-ms',str(args.runtime_ms),'--max-dispatches','1000000000',
                     '--capture-frame',str(stem)+'.bmp','--capture-interval-ms','1000',
                     '--profile-frames',str(frame_profile)]
            if (args.show_test_window and label=='candidate') or (args.baseline_show_test_window and label=='baseline'):command.append('--show-test-window')
            if label=='baseline':
                if args.baseline_vulkan_raster:command.append('--vulkan-raster')
                if args.baseline_vulkan_batch:command.append('--vulkan-batch')
            if args.vulkan:command.extend(['--vulkan-clear','--vulkan-triangles'])
            if args.gpu_adapter:command.extend(['--gpu-adapter',args.gpu_adapter])
            if label=='baseline' and args.baseline_raster_threads is not None:command.extend(['--raster-threads',str(args.baseline_raster_threads)])
            if label=='candidate':
                command.extend(['--raster-threads',str(args.candidate_raster_threads)])
                if args.candidate_vulkan_raster:command.append('--vulkan-raster')
                if args.candidate_vulkan_batch:command.append('--vulkan-batch')
                if args.candidate_vulkan_resident_batch:command.append('--vulkan-resident-batch')
            entry={'label':label,'pair':pair+1,'executable':str(exe.relative_to(ROOT)),
                   'executable_sha256':expected_exes[label],'arguments':command,
                   'log':str(log.relative_to(ROOT)),'frame_profile':str(frame_profile.relative_to(ROOT))}
            report['runs'].append(entry);record()
            print('Starting '+name+' sha256='+expected_exes[label],flush=True)
            with log.open('w',encoding='utf-8') as output:
                output.write('tested_exe_sha256='+expected_exes[label]+'\n');output.flush()
                process=subprocess.run(command,cwd=ROOT,stdout=output,stderr=subprocess.STDOUT)
                output.write('\nprocess_exit_code='+str(process.returncode)+'\n')
            entry['exit_code']=process.returncode
            text=log.read_text(encoding='utf-8-sig',errors='replace')
            adapter=re.search(r'^native_vulkan_clear_adapter=([^\r\n]+)',text,re.M)
            entry['selected_gpu_adapter']=adapter[1].split(' (experimental;')[0] if adapter else None
            if args.gpu_adapter in ('amd','nvidia') and (entry['selected_gpu_adapter'] is None or args.gpu_adapter not in entry['selected_gpu_adapter'].lower()):
                record();raise RuntimeError('Requested performance GPU was not actually selected')
            if process.returncode!=2:record();raise RuntimeError('Unexpected native stop')
            profile=json.loads(frame_profile.read_text(encoding='utf-8'))
            entry['audio_anchor_us']=profile['audio_anchor_us']
            try:entry['song_window']=measure(profile)
            except RuntimeError as error:
                entry['measurement_error']=str(error);report['comparison_completed']=False;record();raise
            if sha(exe)!=expected_exes[label]:raise RuntimeError('Executable changed during experiment')
            record();print(name+' '+json.dumps(entry['song_window']),flush=True)
    report['paired_fps_gain_percent']=[]
    for pair in range(args.pairs):
        rows={r['label']:r for r in report['runs'] if r['pair']==pair+1}
        report['paired_fps_gain_percent'].append((rows['candidate']['song_window']['fps']/rows['baseline']['song_window']['fps']-1)*100)
    report['comparison_completed']=True;record()
    print('Paired FPS gains percent:',report['paired_fps_gain_percent'],flush=True)

if __name__=='__main__':main()
