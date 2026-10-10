"""Compare GPU batching policies using one executable on both actual laptop GPUs."""
from pathlib import Path
import subprocess,sys,json
ROOT=Path(__file__).resolve().parents[1]
for adapter in ('nvidia','amd'):
 command=[sys.executable,str(ROOT/'tools/benchmark_native_game.py'),'--baseline','build/native_full/kon_boot_probe.exe','--candidate','build/native_full/kon_boot_probe.exe','--name','vulkan_resident_'+adapter+'_20261005','--pairs','1','--vulkan','--gpu-adapter',adapter,'--baseline-vulkan-raster','--baseline-vulkan-batch','--baseline-raster-threads','16','--baseline-show-test-window','--candidate-vulkan-raster','--candidate-vulkan-batch','--candidate-vulkan-resident-batch','--candidate-raster-threads','16','--show-test-window','--runtime-ms','180000','--menu-input','work/input_hotchkiss_saved_menu_gpu_paced.txt']
 if adapter=='nvidia':command.append('--start-with-candidate')
 print('Testing one executable on '+adapter,flush=True)
 result=subprocess.run(command,cwd=ROOT)
 print(adapter+' comparison_exit='+str(result.returncode),flush=True)
