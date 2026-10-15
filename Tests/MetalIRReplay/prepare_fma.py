"""Prepare fused-fma float-control variants; this never executes a GPU workload."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--disassembler', default='spirv-dis')
    parser.add_argument('--assembler', default='spirv-as')
    parser.add_argument('--validator', default='spirv-val')
    args = parser.parse_args()
    source = subprocess.check_output([args.disassembler, str(args.input)], text=True)
    args.output.mkdir(parents=True, exist_ok=True)
    # An explicit fma is one rounding under every policy; Relaxed and Fast
    # only license contraction/reassociation of separate operations.
    for name, flags in (('relaxed', 0x3000c), ('fast', 0x3000f)):
        text = source.replace('OpCapability Shader', 'OpCapability Shader\n'
                              'OpCapability FloatControls2\n'
                              'OpExtension "SPV_KHR_float_controls2"')
        text = text.replace('OpExecutionMode %main LocalSize 1 1 1',
                            'OpExecutionMode %main LocalSize 1 1 1\n'
                            'OpExecutionModeId %main FPFastMathDefault %float %math_flags')
        text = text.replace('%float = OpTypeFloat 32',
                            f'%float = OpTypeFloat 32\n%math_flags = OpConstant %uint {flags}')
        if text.count('FPFastMathDefault') != 1 or '%math_flags = OpConstant' not in text:
            raise ValueError('unexpected fma fixture shape')
        assembly, binary = (args.output / ('fma-' + name + suffix) for suffix in ('.spvasm', '.spv'))
        assembly.write_text(text)
        subprocess.run([args.assembler, '--target-env', 'spv1.3', str(assembly),
                        '-o', str(binary)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.2', str(binary)], check=True)


if __name__ == '__main__':
    main()
