"""Vertex/fragment precision oracles; execute only through the host graphics queue."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from run import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'native', 'plugin', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in (args.build, args.native, args.plugin))
    source = Path(__file__).resolve().parent
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    inputs = [build / 'graphics_math_probe', *build.glob('math_graphics-*.spv'),
              build / 'math_plain.vert.spv', build / 'math_plain.frag.spv']
    sources = [source / name for name in ('graphics_math_probe.cpp', 'vulkan_context.h',
               'math_graphics.vert', 'math_graphics.frag', 'math_plain.vert', 'math_plain.frag',
               'prepare_graphics_math.py', 'prepare_math.py', 'run_graphics_math.py')]
    identity = {str(p): sha(p) for p in (native, plugin, *plugin.parent.glob('*.dylib'), *inputs, *sources)}
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    env.update(DYLD_LIBRARY_PATH=str(native.parent), DYLD_PRINT_LIBRARIES='1',
               MELONX_METAL_IR_PLUGIN=str(plugin), MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',
               MVK_CONFIG_FAST_MATH_ENABLED='2', MVK_CONFIG_LOG_LEVEL='4', MTL_SHADER_VALIDATION='1')
    results = []
    for variant in ('no-transform', 'transform'):
        for stage in ('vertex', 'fragment'):
            vs = build / (f'math_graphics-{variant}.vert.spv' if stage == 'vertex' else 'math_plain.vert.spv')
            fs = build / (f'math_graphics-{variant}.frag.spv' if stage == 'fragment' else 'math_plain.frag.spv')
            expected = [(0, 2), (4, 0)] if stage == 'vertex' else [(0, 0), (4, 2)]
            total = 64 if stage == 'vertex' else 256
            for enabled in (0, 1):
                label = variant + '-' + stage + ('-ir' if enabled else '-msl')
                process = subprocess.run([str(build / 'graphics_math_probe'), stage, str(vs), str(fs)],
                    env=dict(env, MELONX_EXPERIMENTAL_METAL_IR=str(enabled)),
                    capture_output=True, text=True, timeout=70)
                text = process.stdout + process.stderr; (out / (label + '.log')).write_text(text)
                oracle = [json.loads(line) for line in process.stdout.splitlines() if line.startswith('{')]
                modes = [(int(a), int(b)) for a, b in re.findall(r'MetalIR compiled stage ([04]):.*?math mode ([012])', text)]
                loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
                verified = len(loaded) == 1 and Path(loaded[0]).resolve() == native
                valid = (process.returncode == 0 and len(oracle) == 1 and
                         oracle[0].get('correct') == total and oracle[0].get('precise') == total and
                         oracle[0].get('success') is True and verified and 'Metal GPU Validation Enabled' in text)
                if enabled:
                    valid = valid and sorted(modes) == expected and str(plugin) in text
                errors = '[mvk-error]' in text or bool(re.search(r'(?:shader|gpu)[^\n]{0,80}validation[^\n]{0,30}(?:error|failed|out.of.bounds)', text, re.I))
                row = dict(case=label, passed=bool(valid and not errors), exitCode=process.returncode,
                           binaryVerified=verified, mathModes=modes, expectedIRModes=expected if enabled else None, oracle=oracle)
                results.append(row); (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps(row), flush=True)
                if not row['passed']:
                    raise RuntimeError('Graphics math replay failed: ' + label)
    assert all(sha(Path(path)) == digest for path, digest in identity.items()), 'Inputs changed'
    summary = dict(passed=len(results), cases=len(results), allPassed=True, gpuValidation=True,
                   scope='Synthetic vertex/fragment scalar precision and nonfinite checks; not whole-game visual or performance acceptance.')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n'); print(json.dumps(summary))


if __name__ == '__main__':
    main()
