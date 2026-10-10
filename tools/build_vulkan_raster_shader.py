from pathlib import Path
import argparse,subprocess,hashlib,json,struct
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output-dir',type=Path,required=True);args=p.parse_args();args.output_dir.mkdir(parents=True,exist_ok=True)
source=root/'src/shaders/native_raster.comp';compiler=root/'third_party/glslang/bin/glslang.exe';spv=args.output_dir/'native_gpu_raster.spv'
subprocess.run([str(compiler),'-V','--target-env','vulkan1.0','-Os','-o',str(spv),str(source)],check=True)
data=spv.read_bytes();words=struct.unpack('<'+'I'*(len(data)//4),data)
header='#pragma once\n#include <cstdint>\nnamespace kon {inline constexpr std::uint32_t native_raster_shader[]={\n'+','.join(hex(w)+'u' for w in words)+'};}\n'
(args.output_dir/'native_gpu_raster_shader.hpp').write_text(header,encoding='utf-8')
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(args.output_dir/'native_gpu_raster_manifest.json').write_text(json.dumps({'source_sha256':sha(source),'compiler_sha256':sha(compiler),'shader_sha256':sha(spv),'generator_sha256':sha(Path(__file__)),'shader_owned_by_project':True,'float64_required':True,'int64_required':True},indent=2)+'\n',encoding='utf-8')
