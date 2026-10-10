"""Record actual Vulkan executions separately from game/FPS acceptance."""
from pathlib import Path
import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--game-log', type=Path)
args = parser.parse_args()
def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
def read(name):
    return json.loads((root / name).read_text(encoding='utf-8-sig'))
def checked_test(name):
    exe = root / f'build/native_full/{name}.exe'
    log_path = root / f'analysis/{name}.log'
    log = log_path.read_text(encoding='utf-8-sig')
    digest = sha(exe)
    if f'tested_exe_sha256={digest}' not in log or 'PASS:' not in log:
        raise RuntimeError(f'{name}: current executable has no matching actual successful run')
    return {'executable': str(exe.relative_to(root)), 'executable_sha256': digest,
            'log': str(log_path.relative_to(root))}
shader = read('analysis/native_vulkan_shader_verification.json')
renderer = read('analysis/native_vulkan_renderer_verification.json')
triangles = read('analysis/native_vulkan_triangle_verification.json')
triangle_manifest = read('build/native_full/native_shaders/native_gpu_triangle_manifest.json')
raster = read('analysis/native_vulkan_raster_verification.json')
raster_manifest = read('build/native_full/native_shaders/native_gpu_raster_manifest.json')
if not raster['native_textured_raster_verified'] or raster['full_vram_comparisons'] < 106 or not raster['failure_before_vram_commit_verified']:
    raise RuntimeError('Textured GPU exactness proof missing')
for key, path in [('generator_sha256','tools/build_vulkan_raster_shader.py'),('source_sha256','src/shaders/native_raster.comp'),('shader_sha256','build/native_full/native_shaders/native_gpu_raster.spv'),('compiler_sha256','third_party/glslang/bin/glslang.exe')]:
    if raster_manifest[key] != sha(root / path): raise RuntimeError('Raster shader belongs to different sources: '+key)
if not triangles['native_flat_triangle_gpu_verified'] or triangles['full_vram_comparisons'] != 121 or not triangles['failure_before_vram_commit_verified']:
    raise RuntimeError('Flat triangle GPU exactness proof missing')
if triangle_manifest['generator_sha256'] != sha(root / 'tools/create_vulkan_triangle_shader.py') or triangle_manifest['shader_sha256'] != sha(root / 'build/native_full/native_shaders/native_gpu_triangle.spv'):
    raise RuntimeError('Triangle shader belongs to different sources')
if not shader['shader_execution_verified'] or shader['rounded_dispatch_guard_words_verified'] != 17:
    raise RuntimeError('Real shader or rounded bounds proof missing')
if not renderer['native_renderer_clear_integration_verified'] or renderer['full_vram_comparisons'] != 67 or renderer['all_color_formats_verified'] != 4 or not renderer['failure_before_vram_commit_verified']:
    raise RuntimeError('Integrated clear exactness proof missing')
manifest = read('build/native_full/native_shaders/native_gpu_clear_manifest.json')
if manifest['generator_sha256'] != sha(root / 'tools/create_vulkan_clear_shader.py') or manifest['shader_sha256'] != sha(root / 'build/native_full/native_shaders/native_gpu_clear.spv'):
    raise RuntimeError('Generated shader belongs to different sources')
record = {
    'checked_at': datetime.now(timezone.utc).isoformat(),
    'shader_test': checked_test('native_vulkan_shader_test'),
    'renderer_test': checked_test('native_vulkan_renderer_test'),
    'shader_manifest': manifest,
    'triangle_test': checked_test('native_vulkan_triangle_test'),
    'triangle_manifest': triangle_manifest,
    'renderer_flat_triangles_verified': True,
    'raster_test': checked_test('native_vulkan_raster_test'),
    'raster_manifest': raster_manifest,
    'renderer_textured_triangles_verified': True,
    'raster_full_vram_comparisons': raster['full_vram_comparisons'],
    'raster_game_run': None,
    'raster_option': '--vulkan-raster',
    'batch_option': '--vulkan-batch',
    'triangle_game_run': None,
    'renderer_clear_verified': True,
    'complete_gpu_renderer': False,
    'game_fps_gain_verified': False,
    'full_song_verified': False,
    'default_enabled': False,
    'option': '--vulkan-clear',
    'triangle_option': '--vulkan-triangles',
    'cpu_operations_remaining': ['texture decoding', 'presentation to Windows', 'geometry transformation, clipping and GE state', 'VRAM feedback or mirrored/aliased destinations'],
    'game_run': None,
    'provenance': 'docs/PROCEDENCIA.md',
    'source_sha256': {name: sha(root / name) for name in ['src/native_vulkan_clear.cpp', 'src/native_vulkan_clear.hpp', 'src/native_gpu_clear.hpp', 'src/native_gpu_triangles.hpp', 'src/native_renderer.hpp', 'src/native_gpu_raster.hpp', 'src/native_frame_stream.hpp', 'src/native_render_worker.hpp', 'src/native_render_timeline.hpp', 'src/native_texture.hpp', 'src/shaders/native_raster.comp', 'tools/build_vulkan_raster_shader.py', 'tools/create_vulkan_triangle_shader.py']},
    'timing_evidence': 'analysis/native_vulkan_clear_timing_verification.json',
}
if args.game_log:
    log_path = args.game_log if args.game_log.is_absolute() else root / args.game_log
    log = log_path.read_text(encoding='utf-8-sig')
    exe = root / 'build/native_full/kon_boot_probe.exe'
    digest = sha(exe)
    if f'tested_exe_sha256={digest}' not in log:
        raise RuntimeError('Game log belongs to another executable')
    submissions = re.findall(r'native_vulkan_clear_submissions=(\d+)', log)
    triangle_submissions = re.findall(r'native_vulkan_triangle_submissions=(\d+)', log)
    triangle_calls = int(triangle_submissions[-1]) if triangle_submissions else 0
    raster_submissions = re.findall(r'native_vulkan_raster_submissions=(\d+)', log)
    raster_calls = int(raster_submissions[-1]) if raster_submissions else 0
    raster_observations = re.findall(r'gpu_raster_primitives=(\d+) gpu_raster_pixels=(\d+)', log)
    observations = re.findall(r'native_progress[^\n]+gpu_clear_primitives=(\d+) gpu_clear_pixels=(\d+)', log)
    adapters = re.findall(r'native_vulkan_clear_adapter=([^\n]+)', log)
    if not submissions or (int(submissions[-1]) + triangle_calls + raster_calls) <= 0 or not observations or not adapters:
        raise RuntimeError('Original game did not verify actual GPU work')
    game_run = {
        'log': str(log_path.relative_to(root)), 'executable_sha256': digest,
        'actual_submissions': int(submissions[-1]) + triangle_calls + raster_calls,
        'clear_submissions': int(submissions[-1]), 'triangle_submissions': triangle_calls,
        'raster_submissions': raster_calls,
        'last_observed_gpu_raster_primitives': int(raster_observations[-1][0]) if raster_observations else 0,
        'last_observed_gpu_raster_pixels': int(raster_observations[-1][1]) if raster_observations else 0,
        'last_observed_gpu_clear_primitives': int(observations[-1][0]),
        'last_observed_gpu_clear_pixels': int(observations[-1][1]),
        'adapter': adapters[-1],
        'stop': re.findall(r'worker_stop=([^\n]+)', log)[-1],
        'masked_context_switches': int(re.findall(r'native_masked_context_switches=(\d+)', log)[-1]),
        'interrupt_restore_anomalies': len(re.findall(r'InterruptRestoreAnomaly elapsed_us=', log)),
        'physical_controls_verified': False,
        'music_sync_verified': False,
        'synthetic_diagnostic_only': True,
    }
    record['game_run'] = game_run if int(submissions[-1]) > 0 else None
    record['triangle_game_run'] = game_run if triangle_calls > 0 else None
    record['raster_game_run'] = game_run if raster_calls > 0 else None
(root / 'analysis/native_vulkan_integration_verification.json').write_text(json.dumps(record, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
print('Vulkan evidence recorded; full GPU renderer, FPS gain and full song remain pending')
