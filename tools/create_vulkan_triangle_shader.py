"""Own exact signed-integer flat triangle kernel, using pinned SPIR-V grammar."""
from pathlib import Path
import json, struct, argparse, hashlib
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output-dir',type=Path,default=root/'work');args=p.parse_args()
grammar=root/'third_party/spirv_headers/SPIRV-Headers-vulkan-sdk-1.3.290.0/include/spirv/unified1/spirv.core.grammar.json'
ops={i['opname']:i['opcode'] for i in json.loads(grammar.read_text())['instructions']}
words=[];next_id=1
def alloc():
    global next_id
    n=next_id;next_id+=1;return n
def emit(op,*a):words.extend([((len(a)+1)<<16)|ops[op],*a])
def res(op,t,*a):
    n=alloc();emit(op,t,n,*a);return n
void,boolean,uint,sint,vec,func,arr,buf,pbuf,pword,pinput,params,pparams,pparam=[alloc() for _ in range(14)]
vbuf,vparams,gid,main=[alloc() for _ in range(4)]
ks={n:alloc() for n in range(17)}
start,body,header,work,cont,loopend,end=[alloc() for _ in range(7)]
iteration,current,covered,rejected,next_iteration,next_color,next_covered,next_rejected=[alloc() for _ in range(8)]
emit('OpCapability',1);emit('OpMemoryModel',0,1)
emit('OpEntryPoint',5,main,0x6e69616d,0,gid);emit('OpExecutionMode',main,17,64,1,1)
emit('OpDecorate',gid,11,28);emit('OpDecorate',arr,6,4);emit('OpDecorate',buf,3)
emit('OpMemberDecorate',buf,0,35,0);emit('OpDecorate',vbuf,34,0);emit('OpDecorate',vbuf,33,0)
emit('OpDecorate',params,2)
for n in range(8):emit('OpMemberDecorate',params,n,35,n*4)
emit('OpTypeVoid',void);emit('OpTypeBool',boolean);emit('OpTypeInt',uint,32,0);emit('OpTypeInt',sint,32,1)
emit('OpTypeVector',vec,uint,3);emit('OpTypeFunction',func,void);emit('OpTypeRuntimeArray',arr,uint)
emit('OpTypeStruct',buf,arr);emit('OpTypePointer',pbuf,2,buf);emit('OpTypePointer',pword,2,uint)
emit('OpTypePointer',pinput,1,vec);emit('OpTypeStruct',params,*([uint]*8))
emit('OpTypePointer',pparams,9,params);emit('OpTypePointer',pparam,9,uint)
for n,k in ks.items():emit('OpConstant',uint,k,n)
zero_s=res('OpConstant',sint,0)
emit('OpVariable',pbuf,vbuf,2);emit('OpVariable',pparams,vparams,9);emit('OpVariable',pinput,gid,1)
emit('OpFunction',void,main,0,func);emit('OpLabel',start)
def param(n):return res('OpLoad',uint,res('OpAccessChain',pparam,vparams,ks[n]))
def address(idx):return res('OpAccessChain',pword,vbuf,ks[0],idx)
def load(idx):return res('OpLoad',uint,address(idx))
def add(a,b):return res('OpIAdd',uint,a,b)
def mul(a,b):return res('OpIMul',uint,a,b)
def logical(a,b):return res('OpLogicalAnd',boolean,a,b)
count,stride,left,top,right,bottom,ntri,mask=[param(n) for n in range(8)]
id3=res('OpLoad',vec,gid)
x=add(res('OpCompositeExtract',uint,id3,0),left);y=add(res('OpCompositeExtract',uint,id3,1),top)
idx=add(mul(y,stride),x)
valid=logical(res('OpULessThan',boolean,idx,count),logical(res('OpULessThan',boolean,x,right),res('OpULessThan',boolean,y,bottom)))
emit('OpSelectionMerge',end,0);emit('OpBranchConditional',valid,body,end);emit('OpLabel',body)
original=load(idx);tri_base=mul(count,ks[3]);sx=res('OpBitcast',sint,add(mul(x,ks[16]),ks[8]));sy=res('OpBitcast',sint,add(mul(y,ks[16]),ks[8]))
emit('OpBranch',header);emit('OpLabel',header)
emit('OpPhi',uint,iteration,ks[0],body,next_iteration,cont)
emit('OpPhi',uint,current,original,body,next_color,cont)
emit('OpPhi',uint,covered,ks[0],body,next_covered,cont)
emit('OpPhi',uint,rejected,ks[0],body,next_rejected,cont)
condition=res('OpULessThan',boolean,iteration,ntri)
emit('OpLoopMerge',loopend,cont,0);emit('OpBranchConditional',condition,work,loopend);emit('OpLabel',work)
tbase=add(tri_base,mul(iteration,ks[16]));fields=[load(add(tbase,ks[n])) for n in range(13)]
ax,ay,bx,by,cx,cy=[res('OpBitcast',sint,v) for v in fields[:6]]
def sub(a,b):return res('OpISub',sint,a,b)
def edge(ax,ay,bx,by):return sub(res('OpIMul',sint,sub(bx,ax),sub(sy,ay)),res('OpIMul',sint,sub(by,ay),sub(sx,ax)))
in_bounds=logical(res('OpUGreaterThanEqual',boolean,x,fields[9]),res('OpUGreaterThanEqual',boolean,y,fields[10]))
in_bounds=logical(in_bounds,logical(res('OpULessThan',boolean,x,fields[11]),res('OpULessThan',boolean,y,fields[12])))
inside=in_bounds
for n,(px,py,qx,qy) in enumerate([(ax,ay,bx,by),(bx,by,cx,cy),(cx,cy,ax,ay)]):
    e=edge(px,py,qx,qy)
    positive=res('OpSGreaterThan',boolean,e,zero_s);equal=res('OpIEqual',boolean,e,zero_s)
    inclusive=res('OpINotEqual',boolean,res('OpBitwiseAnd',uint,fields[7],ks[1<<n]),ks[0])
    inside=logical(inside,res('OpLogicalOr',boolean,positive,logical(equal,inclusive)))
pass_alpha=res('OpINotEqual',boolean,fields[8],ks[0]);paint=logical(inside,pass_alpha)
inc=res('OpSelect',uint,inside,ks[1],ks[0]);reject_inc=res('OpSelect',uint,logical(inside,res('OpLogicalNot',boolean,pass_alpha)),ks[1],ks[0])
new_value=res('OpBitwiseOr',uint,res('OpBitwiseAnd',uint,current,res('OpNot',uint,mask)),res('OpBitwiseAnd',uint,fields[6],mask))
emit('OpSelect',uint,next_color,paint,new_value,current);emit('OpIAdd',uint,next_covered,covered,inc);emit('OpIAdd',uint,next_rejected,rejected,reject_inc)
emit('OpBranch',cont);emit('OpLabel',cont);emit('OpIAdd',uint,next_iteration,iteration,ks[1]);emit('OpBranch',header)
emit('OpLabel',loopend);emit('OpStore',address(idx),current)
stat=add(count,mul(idx,ks[2]));emit('OpStore',address(stat),covered);emit('OpStore',address(add(stat,ks[1])),rejected)
emit('OpBranch',end);emit('OpLabel',end);emit('OpReturn');emit('OpFunctionEnd')
module=[0x07230203,0x00010000,0,next_id,0,*words];at=5
while at<len(module):
    size=module[at]>>16
    if not size or at+size>len(module):raise RuntimeError('Invalid own triangle shader framing')
    at+=size
encoded=struct.pack('<'+'I'*len(module),*module);args.output_dir.mkdir(parents=True,exist_ok=True)
(args.output_dir/'native_gpu_triangle.spv').write_bytes(encoded)
h='#pragma once\n#include <cstdint>\nnamespace kon {\ninline constexpr std::uint32_t native_triangle_shader[] = {\n'
for n in range(0,len(module),8):h+='    '+','.join(f'0x{v:08X}u' for v in module[n:n+8])+',\n'
(args.output_dir/'native_gpu_triangle_shader.hpp').write_text(h+'};\n}\n',encoding='utf-8')
manifest={'generator':'tools/create_vulkan_triangle_shader.py','generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'grammar_sha256':hashlib.sha256(grammar.read_bytes()).hexdigest(),'shader_sha256':hashlib.sha256(encoded).hexdigest(),'words':len(module),'spirv_version':'1.0','ownership':'Own ordered signed integer flat-triangle compute kernel','gpu_execution_verified':False}
(args.output_dir/'native_gpu_triangle_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(f'Generated own flat triangle shader: {len(module)} words; GPU validation pending')
