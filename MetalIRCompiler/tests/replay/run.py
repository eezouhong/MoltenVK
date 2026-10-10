"""Synthetic system-value and fail-closed replay. Use the host graphics queue."""
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
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in (args.build, args.native, args.plugin))
    out = args.output.resolve()
    inputs = [build / p for p in ('runtime_probe', 'runtime.vert.spv', 'runtime.frag.spv', 'runtime.comp.spv', 'unused.comp.spv')]
    sources = [p for p in Path(__file__).resolve().parent.iterdir() if p.suffix in ('.cpp', '.vert', '.frag', '.comp', '.py', '.txt', '.h')]
    tracked = [native, plugin, *plugin.parent.glob('*.dylib'), *inputs, *sources]
    identity = {str(p): sha(p) for p in tracked}
    out.mkdir(parents=True, exist_ok=False)
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    environment = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_'))}
    environment.update(MELONX_METAL_IR_PLUGIN=str(plugin), DYLD_LIBRARY_PATH=str(native.parent),
                       DYLD_PRINT_LIBRARIES='1', MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',
                       MVK_CONFIG_METAL4_COMPILER='1', MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',
                       MVK_CONFIG_LOG_LEVEL='4', MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0',
                       MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1')
    results = []

    def run(label, kind, enabled, cache=None, missing=False):
        env = dict(environment, MELONX_EXPERIMENTAL_METAL_IR=str(int(enabled)))
        if cache is not None:
            env['MELONX_METAL_IR_CACHE'] = str(cache)
        if missing:
            env['MELONX_METAL_IR_PLUGIN'] = str(out / 'missing-plugin.dylib')
        names = ('runtime.comp.spv', 'unused.comp.spv') if kind in ('compute', 'empty', 'compute-tail') else ('runtime.vert.spv', 'runtime.frag.spv')
        process = subprocess.run([str(build / 'runtime_probe'), kind, *(str(build / p) for p in names)],
                                 env=env, capture_output=True, text=True, timeout=70)
        text = process.stdout + process.stderr
        (out / (label + '.log')).write_text(text)
        rows = [json.loads(line) for line in process.stdout.splitlines() if line.startswith('{')]
        loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
        binary_ok = len(loaded) == 1 and Path(loaded[0]).resolve() == native
        rejected = [line for line in text.splitlines() if 'MetalIR' in line and any(
            word in line for word in ('rejected', 'unavailable', 'failed', 'retrying', 'using original'))]
        expected_failure = missing and enabled
        compiled = text.count('MetalIR compiled stage')
        restored = text.count('MetalIR restored stage')
        if expected_failure:
            valid = process.returncode != 0 and not rows and bool(rejected) and 'MSL fallback disabled' in text
        else:
            valid = process.returncode == 0 and len(rows) == (3 if kind == 'toggle' else 1)
            valid = valid and all(r.get('success') is True for r in rows) and not rejected
        if kind == 'toggle':
            valid = valid and text.count('Shader compiler selected: MSL') == 2 and text.count('Shader compiler selected: Metal IR') == 1
        elif not expected_failure:
            valid = valid and ('Shader compiler selected: ' + ('Metal IR' if enabled else 'MSL')) in text
        if enabled and not expected_failure:
            valid = valid and str(plugin) in text and compiled + restored >= 2
        if label.endswith('-restored'):
            valid = valid and restored >= 2 and compiled == 0
        result = dict(case=label, passed=bool(valid and binary_ok), exitCode=process.returncode,
                      binaryVerified=binary_ok, expectedFailure=expected_failure,
                      compiled=compiled, restored=restored, oracle=rows)
        results.append(result)
        (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(result), flush=True)
        if not result['passed']:
            raise RuntimeError('Replay failed: ' + label)

    for kind in ('graphics', 'compute', 'fan', 'empty', 'tail', 'compute-tail', 'fan-indirect'):
        run('msl-' + kind, kind, False)
        run('ir-' + kind, kind, True)
    for kind in ('graphics', 'compute'):
        cache = out / (kind + '-cache')
        run('ir-' + kind + '-cold', kind, True, cache=cache)
        run('ir-' + kind + '-restored', kind, True, cache=cache)
        run('ir-' + kind + '-missing-compiler', kind, True, missing=True)
        run('msl-' + kind + '-missing-ir-compiler', kind, False, missing=True)
    run('manual-device-selection', 'toggle', True)
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Input changed during replay'
    summary = {'passed': len(results), 'cases': len(results), 'allPassed': True,
               'scope': 'Synthetic system values, cache restore and explicit compiler selection. Not a game FPS or full Vulkan conformance gate.'}
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
