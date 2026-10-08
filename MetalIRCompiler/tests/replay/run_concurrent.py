"""32-shader single/four-worker compilation with GPU output checks. Queue only."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
from run import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'native', 'plugin', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in (args.build, args.native, args.plugin))
    source = Path(__file__).resolve().parent
    files = [native, plugin, *plugin.parent.glob('*.dylib'), build / 'concurrent_probe', build / 'unused.comp.spv',
             source / 'concurrent_probe.cpp', source / 'vulkan_context.h', Path(__file__)]
    identity = {str(p): sha(p) for p in files}
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    base = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    base.update(MELONX_METAL_IR_PLUGIN=str(plugin), MELONX_METAL_IR_TELEMETRY='1',
                DYLD_LIBRARY_PATH=str(native.parent), DYLD_PRINT_LIBRARIES='1',
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1', MVK_CONFIG_METAL4_COMPILER='1',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0', MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0',
                MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1', MVK_CONFIG_LOG_LEVEL='4', MTL_SHADER_VALIDATION='1')
    results = []
    # Distinct entry names and constants per run avoid the Apple warm-cache
    # advantage. Each shader has the same instruction shape and independent row.
    initial = 10000 + (os.getpid() % 1000000) * 1000
    for workers in (1, 4):
        for index, route in enumerate(('msl', 'ir', 'ir', 'msl')):
            seed = initial + workers * 100 + index * 32
            label = f'{workers}-{index}-{route}'
            process = subprocess.run([str(build / 'concurrent_probe'), str(build / 'unused.comp.spv'),
                                      str(workers), '32', str(seed)],
                                     env=dict(base, MELONX_EXPERIMENTAL_METAL_IR='1' if route == 'ir' else '0'),
                                     capture_output=True, text=True, timeout=125)
            text = process.stdout + process.stderr; (out / (label + '.log')).write_text(text)
            rows = [json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
            loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
            valid = process.returncode == 0 and len(rows) == 2 and all(r.get('success') is True for r in rows)
            valid &= len(loaded) == 1 and Path(loaded[0]).resolve() == native
            valid &= 'Metal GPU Validation Enabled' in text and '[mvk-error]' not in text
            valid &= 'Shader compiler selected: ' + ('Metal IR' if route == 'ir' else 'MSL') in text
            valid &= 'MetalIR shader rejected:' not in text
            if rows:
                valid &= rows[0].get('correct') == rows[0].get('total') == 64
            if len(rows) == 2:
                valid &= rows[1]['jobs'] == 32 and rows[1]['maxConcurrentRequests'] == workers
            if route == 'ir':
                valid &= text.count('MetalIR compiled stage') == 32 and text.count('MetalIR restored stage') == 0
            result = dict(case=label, route=route, workers=workers, seed=seed, valid=bool(valid), exitCode=process.returncode, output=rows)
            results.append(result); (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps(result), flush=True)
            if not valid: raise RuntimeError('Concurrent compilation failed: ' + label)
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Input changed during replay'
    medians = {f'{route}-{workers}': statistics.median(r['output'][1]['compileWallMs'] for r in results
               if r['route'] == route and r['workers'] == workers) for route in ('msl', 'ir') for workers in (1, 4)}
    summary = dict(allPassed=True, runs=len(results), jobsPerRun=32, medianCompileWallMs=medians,
                   fourWorkerIRoverMSL=medians['ir-4']/medians['msl-4'],
                   irFourWorkerSpeedup=medians['ir-1']/medians['ir-4'],
                   scope='Synthetic same-device compilation and GPU correctness; unique names/literals, two samples per configuration, no game/FPS claim.')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n'); print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
