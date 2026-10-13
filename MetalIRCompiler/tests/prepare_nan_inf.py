#!/usr/bin/env python3
"""Prepare a synthetic Vulkan 1.2 NaN/Inf oracle with explicit float controls."""
import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error('use an empty output directory')
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().with_name('nan_inf.comp')
    subprocess.run(['glslangValidator', '-V', source, '-o', out / 'input.spv'], check=True)
    subprocess.run(['spirv-dis', out / 'input.spv', '-o', out / 'input.spvasm'], check=True)
    text = (out / 'input.spvasm').read_text()
    assert text.count('OpCapability Shader') == 1
    text = text.replace('OpCapability Shader',
        'OpCapability Shader\n               OpCapability SignedZeroInfNanPreserve\n'
        '               OpExtension "SPV_KHR_float_controls"', 1)
    execution = re.search(r'OpExecutionMode (%\S+) LocalSize [^\n]+', text)
    assert execution
    text = text[:execution.end()] + '\n               OpExecutionMode ' + execution.group(1) + \
        ' SignedZeroInfNanPreserve 32' + text[execution.end():]
    (out / 'nan_inf.spvasm').write_text(text)
    # The source uses pre-SPIR-V-1.4 BufferBlock. Float controls are supplied
    # through their extension, so do not accidentally assemble as SPIR-V 1.5.
    subprocess.run(['spirv-as', '--target-env', 'spv1.3', out / 'nan_inf.spvasm',
                    '-o', out / 'nan_inf.spv'], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.2', out / 'nan_inf.spv'], check=True)


if __name__ == '__main__':
    main()
