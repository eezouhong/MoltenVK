"""Actual pinned MSC FP64 rejection and same-device negative-cache reuse.

Run through the shared graphics queue. No dispatch is attempted: the input
tests a structurally valid SPIR-V optional feature unsupported by this backend.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'native', 'plugin', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in (args.build, args.native, args.plugin))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    files = [build / 'msc_negative_probe', build / 'msc_negative.comp.spv', native,
             plugin, *plugin.parent.glob('*.dylib'), *[Path(__file__).with_name(n)
             for n in ('msc_negative_probe.cpp', 'msc_negative.comp', 'vulkan_context.h')]]
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    identity = {str(p): sha(p) for p in files}
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    env.update(MELONX_EXPERIMENTAL_METAL_IR='1', MELONX_METAL_IR_PLUGIN=str(plugin),
               MELONX_METAL_IR_TELEMETRY='1', MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',
               MVK_CONFIG_METAL4_COMPILER='1', MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',
               MVK_CONFIG_LOG_LEVEL='4', DYLD_PRINT_LIBRARIES='1',
               DYLD_LIBRARY_PATH=str(native.parent), MTL_ENABLE_DEBUG_LAYER='1',
               MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1')
    with (out / 'stdout.jsonl').open('x') as stdout, (out / 'stderr.log').open('x') as stderr:
        process = subprocess.run([str(build / 'msc_negative_probe'),
                                  str(build / 'msc_negative.comp.spv')], env=env,
                                 stdout=stdout, stderr=stderr, timeout=90)
    log = (out / 'stderr.log').read_text()
    rows = [json.loads(line) for line in (out / 'stdout.jsonl').read_text().splitlines()
            if line.startswith('{')]
    loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', log, re.M)
    codes = [int(code) for code in re.findall(r'MetalIR stage 5 rejected: MSC (\d+):', log)]
    passed = process.returncode == 0 and len(rows) == 2
    passed &= all(row['vkResult'] != 0 and row['compilerCalls'] == 1 and row['mscNs'] > 0 for row in rows)
    passed &= len(loaded) == 1 and Path(loaded[0]).resolve() == native and str(plugin) in log
    # IRErrorCodeFP64Usage in the pinned MSC 3.1.1 header; not a Mesa/frontend rejection.
    passed &= codes == [19] and 'MSL fallback disabled' in log
    passed &= len(rows) == 2 and rows[0]['mscNs'] == rows[1]['mscNs']
    passed &= [row['rejected'] for row in rows] == [1, 2]
    passed &= all(sha(Path(path)) == digest for path, digest in identity.items())
    result = {'exitCode': process.returncode, 'passed': bool(passed),
              'mscErrorCodes': codes, 'attempts': rows,
              'scope': 'Expected unsupported FP64 rejection and same-device negative-cache reuse; no dispatch'}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
