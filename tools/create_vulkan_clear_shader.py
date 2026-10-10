"""Assemble our bounded integer rectangle-clear kernel using pinned Khronos grammar.
The generated SPIR-V runs on the GPU; this is not a shader interpreter.
"""
from pathlib import Path
import json
import struct
import argparse
import hashlib

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output-dir", type=Path, default=root / "work")
args = parser.parse_args()
grammar = root / "third_party/spirv_headers/SPIRV-Headers-vulkan-sdk-1.3.290.0/include/spirv/unified1/spirv.core.grammar.json"
g = json.loads(grammar.read_text(encoding="utf-8"))
opcodes = {i["opname"]: i["opcode"] for i in g["instructions"]}
next_id = 1
words = []
def allocate():
    global next_id
    result = next_id
    next_id += 1
    return result
def emit(name, *operands):
    words.extend([((len(operands)+1)<<16) | opcodes[name], *operands])
def result(name, type_id, *operands):
    target = allocate()
    emit(name, type_id, target, *operands)
    return target

t_void, t_bool, t_uint, t_vec3, t_func = [allocate() for _ in range(5)]
t_array, t_buffer, p_buffer, p_word, p_input = [allocate() for _ in range(5)]
t_params, p_params, p_param, v_buffer, v_params, v_gid, main = [allocate() for _ in range(7)]
constants = [allocate() for _ in range(8)]
start, body, end = [allocate() for _ in range(3)]
emit("OpCapability", 1)  # Shader
emit("OpMemoryModel", 0, 1)  # Logical, GLSL450
emit("OpEntryPoint", 5, main, 0x6e69616d, 0, v_gid)  # GLCompute, main
emit("OpExecutionMode", main, 17, 64, 1, 1)  # LocalSize
emit("OpDecorate", v_gid, 11, 28)  # BuiltIn GlobalInvocationId
emit("OpDecorate", t_array, 6, 4)  # ArrayStride
emit("OpDecorate", t_buffer, 3)  # BufferBlock (SPIR-V 1.0)
emit("OpMemberDecorate", t_buffer, 0, 35, 0)  # Offset
emit("OpDecorate", v_buffer, 34, 0)  # DescriptorSet
emit("OpDecorate", v_buffer, 33, 0)  # Binding
emit("OpDecorate", t_params, 2)  # Block
for n in range(8):
    emit("OpMemberDecorate", t_params, n, 35, n*4)
emit("OpTypeVoid", t_void)
emit("OpTypeBool", t_bool)
emit("OpTypeInt", t_uint, 32, 0)
emit("OpTypeVector", t_vec3, t_uint, 3)
emit("OpTypeFunction", t_func, t_void)
emit("OpTypeRuntimeArray", t_array, t_uint)
emit("OpTypeStruct", t_buffer, t_array)
emit("OpTypePointer", p_buffer, 2, t_buffer)  # Uniform
emit("OpTypePointer", p_word, 2, t_uint)
emit("OpTypePointer", p_input, 1, t_vec3)  # Input
emit("OpTypeStruct", t_params, *([t_uint]*8))
emit("OpTypePointer", p_params, 9, t_params)  # PushConstant
emit("OpTypePointer", p_param, 9, t_uint)
for n, c in enumerate(constants):
    emit("OpConstant", t_uint, c, n)
emit("OpVariable", p_buffer, v_buffer, 2)
emit("OpVariable", p_params, v_params, 9)
emit("OpVariable", p_input, v_gid, 1)
emit("OpFunction", t_void, main, 0, t_func)
emit("OpLabel", start)
def parameter(n):
    address = result("OpAccessChain", p_param, v_params, constants[n])
    return result("OpLoad", t_uint, address)
gid = result("OpLoad", t_vec3, v_gid)
# Dispatch a rectangle's bounding region instead of scanning the whole target
# for each stripe. Rounded x invocations must branch out BEFORE any memory
# access: writing an unchanged value outside the rectangle can race a valid
# invocation on the following row when a framebuffer has a narrow stride.
stride = parameter(1)
left, top, right, bottom = [parameter(n) for n in range(2, 6)]
x = result("OpIAdd", t_uint, result("OpCompositeExtract", t_uint, gid, 0), left)
y = result("OpIAdd", t_uint, result("OpCompositeExtract", t_uint, gid, 1), top)
index = result("OpIAdd", t_uint, result("OpIMul", t_uint, y, stride), x)
valid = result("OpULessThan", t_bool, index, parameter(0))
valid = result("OpLogicalAnd", t_bool, valid, result("OpULessThan", t_bool, x, right))
valid = result("OpLogicalAnd", t_bool, valid, result("OpULessThan", t_bool, y, bottom))
emit("OpSelectionMerge", end, 0)
emit("OpBranchConditional", valid, body, end)
emit("OpLabel", body)
address = result("OpAccessChain", p_word, v_buffer, constants[0], index)
previous = result("OpLoad", t_uint, address)
color, mask = parameter(6), parameter(7)
inverse = result("OpNot", t_uint, mask)
kept = result("OpBitwiseAnd", t_uint, previous, inverse)
painted = result("OpBitwiseAnd", t_uint, color, mask)
combined = result("OpBitwiseOr", t_uint, kept, painted)
emit("OpStore", address, combined)
emit("OpBranch", end)
emit("OpLabel", end)
emit("OpReturn")
emit("OpFunctionEnd")
module = [0x07230203, 0x00010000, 0, next_id, 0, *words]
# Check framing separately before passing the module to a Vulkan driver.
at = 5
while at < len(module):
    size = module[at] >> 16
    if not size or at+size > len(module):
        raise RuntimeError("Invalid own shader instruction framing")
    at += size
if at != len(module):
    raise RuntimeError("Invalid own shader length")
args.output_dir.mkdir(parents=True, exist_ok=True)
destination = args.output_dir / "native_gpu_clear.spv"
encoded = struct.pack("<"+"I"*len(module), *module)
destination.write_bytes(encoded)
header = "#pragma once\n#include <cstdint>\nnamespace kon {\ninline constexpr std::uint32_t native_clear_shader[] = {\n"
for start in range(0, len(module), 8):
    header += "    " + ",".join(f"0x{v:08X}u" for v in module[start:start+8]) + ",\n"
header += "};\n}\n"
(args.output_dir / "native_gpu_clear_shader.hpp").write_text(header, encoding="utf-8")
manifest = {
    "generator": "tools/create_vulkan_clear_shader.py",
    "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    "grammar": str(grammar.relative_to(root)),
    "grammar_sha256": hashlib.sha256(grammar.read_bytes()).hexdigest(),
    "shader_sha256": hashlib.sha256(encoded).hexdigest(),
    "spirv_version": "1.0", "words": len(module),
    "dispatch": "rectangle bounding region, 64 x 1 local size; no write outside bounds",
    "ownership": "Own bounded integer rectangle-clear kernel",
    "generation_is_gpu_execution_proof": False,
}
(args.output_dir / "native_gpu_clear_manifest.json").write_text(json.dumps(manifest, indent=2)+"\n", encoding="utf-8")
print(f"Generated own integer clear shader: {len(module)} words; GPU execution still requires validation")
