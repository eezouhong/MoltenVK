"""Prepare vertex/fragment mixed-precision fixtures without executing graphics."""
import argparse
from pathlib import Path
import subprocess
from prepare_math import mixed_precision_variant


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--glslang', default='glslangValidator')
    parser.add_argument('--disassembler', default='spirv-dis')
    parser.add_argument('--assembler', default='spirv-as')
    parser.add_argument('--validator', default='spirv-val')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for stage in ('vert', 'frag'):
        for kind in ('plain', 'graphics'):
            source = args.source / ('math_' + kind + '.' + stage)
            binary = args.output / ('math_' + kind + '.' + stage + '.spv')
            subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.1',
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([args.validator, '--target-env', 'vulkan1.2', str(binary)], check=True)
            if kind == 'plain':
                continue
            text = subprocess.check_output([args.disassembler, str(binary)], text=True)
            for variant, transform in (('no-transform', False), ('transform', True)):
                assembly = args.output / ('math_graphics-' + variant + '.' + stage + '.spvasm')
                target = assembly.with_suffix('.spv')
                assembly.write_text(mixed_precision_variant(text, transform))
                subprocess.run([args.assembler, '--target-env', 'spv1.3', str(assembly),
                                '-o', str(target)], check=True)
                subprocess.run([args.validator, '--target-env', 'vulkan1.2', str(target)], check=True)


if __name__ == '__main__':
    main()
