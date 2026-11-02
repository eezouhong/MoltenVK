"""Build post_tessellation.spv: the vertex template with its inputs redecorated as PrimitiveId and TessCoord."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent / 'post_tessellation.vert'
    args.output.mkdir(parents=True, exist_ok=True)
    template = args.output / 'post_tessellation_template.spv'
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.1', str(source), '-o', str(template)],
                   check=True, capture_output=True)
    text = subprocess.check_output(['spirv-dis', '--raw-id', str(template)], text=True)
    names = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == 'OpName':
            names[parts[2].strip('"')] = parts[1]
    lines = []
    for line in text.splitlines():
        parts = line.split()
        if parts[:1] == ['OpDecorate'] and parts[1] == names['patchIndex'] and parts[2] == 'Location':
            line = f'OpDecorate {parts[1]} BuiltIn PrimitiveId'
        elif parts[:1] == ['OpDecorate'] and parts[1] == names['tessCoord'] and parts[2] == 'Location':
            line = f'OpDecorate {parts[1]} BuiltIn TessCoord'
        elif parts[:2] == ['OpCapability', 'Shader']:
            lines.append(line)
            line = 'OpCapability Tessellation'
        lines.append(line)
    assembly = args.output / 'post_tessellation.spvasm'
    assembly.write_text('\n'.join(lines) + '\n')
    subprocess.run(['spirv-as', '--preserve-numeric-ids', str(assembly), '-o', str(args.output / 'post_tessellation.spv')], check=True)
    print(args.output / 'post_tessellation.spv')


if __name__ == '__main__':
    main()
