"""Prepare synthetic float-control variants; this never executes a GPU workload."""
import argparse
from pathlib import Path
import re
import subprocess


def mixed_precision_variant(source, allow_transform=True):
    # GLSLang propagates precise to expressions using a precise value. This
    # fixture selects exactly the producer and sum that the CPU checks bitwise.
    keep = set()
    for variable in ('product', 'exact'):
        ids = re.findall(r'OpStore %' + variable + r' (%\w+)', source)
        if len(ids) != 1:
            raise ValueError('unexpected precise fixture shape: ' + variable)
        keep.add(ids[0])
    decorated = set(re.findall(r'OpDecorate (%\w+) NoContraction', source))
    if not keep <= decorated:
        raise ValueError('precise fixture lost NoContraction')
    text = re.sub(r'^.*OpDecorate (%\w+) NoContraction.*$',
                  lambda match: match.group(0) if match.group(1) in keep else '', source, flags=re.M)
    ordinary = [value for value in re.findall(r'(%\w+) = OpFMul ', text) if value not in keep]
    if len(ordinary) != 1:
        raise ValueError('unexpected ordinary multiply fixture shape')
    text = text.replace('OpCapability Shader', 'OpCapability Shader\n'
                        'OpCapability FloatControls2\nOpExtension "SPV_KHR_float_controls2"')
    text = text.replace('%void = OpTypeVoid',
                        f'OpDecorate {ordinary[0]} FPFastMathMode '
                        'NSZ|AllowRecip|AllowContract|AllowReassoc' +
                        ('|AllowTransform' if allow_transform else '') + '\n%void = OpTypeVoid')
    return text


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

    def save(name, text):
        assembly, binary = (args.output / ('math-' + name + suffix)
                            for suffix in ('.spvasm', '.spv'))
        assembly.write_text(text)
        subprocess.run([args.assembler, '--target-env', 'spv1.3', str(assembly),
                        '-o', str(binary)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.2', str(binary)], check=True)

    # NSZ|AllowRecip|AllowContract|AllowReassoc, with NaN/Inf still preserved.
    # 0x40000 additionally permits transforms. Without it, both pinned compilers
    # treat the operation as precise even when their global policy is Relaxed.
    for name, flags, precise in (('safe', 0, True), ('mixed-precise', 0x3000c, True),
                                 ('relaxed', 0x3000c, False), ('fast', 0x3000f, False),
                                 ('relaxed-transform', 0x7000c, False),
                                 ('fast-transform', 0x7000f, False)):
        text = source.replace('OpCapability Shader', 'OpCapability Shader\n'
                              'OpCapability FloatControls2\n'
                              'OpExtension "SPV_KHR_float_controls2"')
        text = text.replace('OpExecutionMode %main LocalSize 1 1 1',
                            'OpExecutionMode %main LocalSize 1 1 1\n'
                            'OpExecutionModeId %main FPFastMathDefault %float %math_flags')
        text = text.replace('%float = OpTypeFloat 32',
                            f'%float = OpTypeFloat 32\n%math_flags = OpConstant %uint {flags}')
        # NoContraction is illegal with FPFastMathDefault; None is the precise
        # per-operation permission set in the float-controls2 variants.
        text = re.sub(r'(OpDecorate %\w+) NoContraction',
                      r'\1 FPFastMathMode None' if precise else '', text)
        save(name, text)

    save('mixed-no-contraction', mixed_precision_variant(source))



if __name__ == '__main__':
    main()
