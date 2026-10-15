"""Fused-fma oracle for MSL and IR under every math policy. Use the host graphics queue."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('build', 'native', 'plugin', 'output'):
        parser.add_argument('--' + argument, type=Path, required=True)
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in
                             (args.build, args.native, args.plugin))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    kinds = ('legacy', 'relaxed', 'fast')
    inputs = [build / 'fma_probe', *[build / ('fma-' + k + '.spv') for k in kinds]]
    sources = [Path(__file__), *[Path(__file__).with_name(name) for name in
               ('fma_probe.cpp', 'vulkan_context.h', 'fused_fma.comp', 'prepare_fma.py')]]
    identity = {str(p): sha(p) for p in
                (native, plugin, *plugin.parent.glob('*.dylib'), *inputs, *sources)}
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(
        ('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    env.update(DYLD_LIBRARY_PATH=str(native.parent), DYLD_PRINT_LIBRARIES='1',
               MELONX_METAL_IR_PLUGIN=str(plugin), MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',
               MVK_CONFIG_LOG_LEVEL='4', MTL_SHADER_VALIDATION='1')
    results = []
    for kind in kinds:
        for preference in (2, 0, 1):
            # Same policy resolution as run_math.py's legacy/relaxed/fast kinds.
            expected = (0 if preference == 1 else 1 if preference == 0 else
                        2 if kind == 'relaxed' else 0)
            for enabled in (0, 1):
                label = f'{kind}-{preference}-' + ('ir' if enabled else 'msl')
                process = subprocess.run(
                    [str(build / 'fma_probe'), str(build / ('fma-' + kind + '.spv'))],
                    env=dict(env, MELONX_EXPERIMENTAL_METAL_IR=str(enabled),
                             MVK_CONFIG_FAST_MATH_ENABLED=str(preference)),
                    capture_output=True, text=True, timeout=70)
                text = process.stdout + process.stderr
                (out / (label + '.log')).write_text(text)
                oracle = [json.loads(line) for line in process.stdout.splitlines() if line.startswith('{')]
                modes = re.findall(r'MetalIR compiled stage 5:.*?math mode ([012])', text)
                loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
                verified = len(loaded) == 1 and Path(loaded[0]).resolve() == native
                valid = (process.returncode == 0 and len(oracle) == 1 and
                         oracle[0].get('success') is True and verified and
                         'Metal GPU Validation Enabled' in text)
                if enabled:
                    valid = valid and modes == [str(expected)] and str(plugin) in text
                rejected = any('MetalIR' in line and any(word in line for word in
                    ('rejected', 'unavailable', 'failed', 'retrying', 'using original'))
                    for line in text.splitlines())
                row = dict(case=label, passed=bool(valid and not rejected),
                           exitCode=process.returncode, binaryVerified=verified,
                           mathModes=modes, expectedIRMode=expected if enabled else None,
                           oracle=oracle)
                results.append(row)
                (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps(row), flush=True)
                if not row['passed']:
                    raise RuntimeError('Fused fma replay failed: ' + label)
    assert all(sha(Path(path)) == digest for path, digest in identity.items()), 'Inputs changed'
    summary = dict(passed=len(results), cases=len(results), allPassed=True, gpuValidation=True,
                   scope='Synthetic scalar fused-fma bit checks; not game FPS or full Vulkan conformance.')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
