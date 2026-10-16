"""Create dynamic F/N min, max, clamp and saturate fixtures. CPU only."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
base='OpCapability Shader\nOpCapability FloatControls2\nOpExtension "SPV_KHR_float_controls2"\n%glsl = OpExtInstImport "GLSL.std.450"\nOpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %main "main" %gid\nOpExecutionMode %main LocalSize 1 1 1\nOpExecutionModeId %main FPFastMathDefault %float %flags\nOpDecorate %gid BuiltIn GlobalInvocationId\nOpDecorate %Words ArrayStride 4\nOpMemberDecorate %Data 0 Offset 0\nOpDecorate %Data Block\nOpDecorate %storage DescriptorSet 0\nOpDecorate %storage Binding 0\n%void = OpTypeVoid\n%fn = OpTypeFunction %void\n%uint = OpTypeInt 32 0\n%float = OpTypeFloat 32\n%f0 = OpConstant %float 0\n%f1 = OpConstant %float 1\n%v3uint = OpTypeVector %uint 3\n%Words = OpTypeRuntimeArray %uint\n%Data = OpTypeStruct %Words\n%pData = OpTypePointer StorageBuffer %Data\n%pUint = OpTypePointer StorageBuffer %uint\n%pInput = OpTypePointer Input %v3uint\n%storage = OpVariable %pData StorageBuffer\n%gid = OpVariable %pInput Input\n'
for i in range(17):base+=f'%u{i} = OpConstant %uint {i}\n'
body="%main = OpFunction %void None %fn\n%entry = OpLabel\n%xyz = OpLoad %v3uint %gid\n%index = OpCompositeExtract %uint %xyz 0\n%start = OpIMul %uint %index %u16\n"
for i,name in enumerate(('x','y','lo','hi')):
 body+=f'%idx{i} = OpIAdd %uint %start %u{i}\n%ptr{i} = OpAccessChain %pUint %storage %u0 %idx{i}\n%word{i} = OpLoad %uint %ptr{i}\n%{name} = OpBitcast %float %word{i}\n'
ops=[('FMin','%x %y'),('FMin','%y %x'),('FMax','%x %y'),('FMax','%y %x'),('NMin','%x %y'),('NMin','%y %x'),('NMax','%x %y'),('NMax','%y %x'),('FClamp','%x %lo %hi'),('NClamp','%x %lo %hi'),('FClamp','%x %f0 %f1'),('NClamp','%x %f0 %f1')]
for i,(op,operands) in enumerate(ops,4):
 body+=f'%result{i} = OpExtInst %float %glsl {op} {operands}\n%outword{i} = OpBitcast %uint %result{i}\n%idx{i} = OpIAdd %uint %start %u{i}\n%ptr{i} = OpAccessChain %pUint %storage %u0 %idx{i}\nOpStore %ptr{i} %outword{i}\n'
body+='OpReturn\nOpFunctionEnd\n'
for mode,flags in (('safe',0),('relaxed',0x3000c),('fast',0x7000f)):
 asm=a.output/f'minmax-{mode}.spvasm';binary=asm.with_suffix('.spv')
 asm.write_text(base+f'%flags = OpConstant %uint {flags}\n'+body)
 subprocess.run(['spirv-as','--target-env','spv1.3',str(asm),'-o',str(binary)],check=True)
 subprocess.run(['spirv-val','--target-env','vulkan1.2',str(binary)],check=True)
