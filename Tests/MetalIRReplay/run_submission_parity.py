"""Exact submission/encoder parity for fixed inputs. Run via the graphics queue."""
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
    tracked = [native, plugin, *plugin.parent.glob('*.dylib'), build / 'runtime_probe',
               *[build / p for p in ('runtime.vert.spv', 'runtime.frag.spv', 'runtime.comp.spv', 'unused.comp.spv')],
               source / 'runtime_probe.cpp', source / 'vulkan_context.h', source / 'run.py', Path(__file__)]
    identity = {str(p): sha(p) for p in tracked}
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    base = {k: v for k, v in os.environ.items()
            if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    base.update(MELONX_METAL_IR_PLUGIN=str(plugin), DYLD_LIBRARY_PATH=str(native.parent),
                DYLD_PRINT_LIBRARIES='1', MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',
                MVK_CONFIG_METAL4_COMPILER='1', MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',
                MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0', MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1',
                MVK_CONFIG_LOG_LEVEL='4', MELONX_PIPELINE_REPLAY_TRACE='coarse', MTL_SHADER_VALIDATION='1')
    results, comparisons = [], []
    # Same SPIR-V and command arguments in each process, with ABBA ordering.
    # Include raw addresses at the buffer tail and fan conversion's compute path.
    for mode in ('graphics', 'tail', 'compute', 'compute-tail', 'fan-indirect'):
        names = ('runtime.comp.spv', 'unused.comp.spv') if mode.startswith('compute') else ('runtime.vert.spv', 'runtime.frag.spv')
        counts = []
        for index, route in enumerate(('msl', 'ir', 'ir', 'msl')):
            label = f'{mode}-{index}-{route}'
            env = dict(base, MELONX_EXPERIMENTAL_METAL_IR='1' if route == 'ir' else '0')
            process = subprocess.run([str(build / 'runtime_probe'), mode, *[str(build / p) for p in names]],
                                     env=env, capture_output=True, text=True, timeout=70)
            text = process.stdout + process.stderr
            (out / (label + '.log')).write_text(text)
            oracle = [json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
            frames = [json.loads(s.partition('MELONX_REPLAY_FRAME ')[2])
                      for s in process.stderr.splitlines() if s.startswith('MELONX_REPLAY_FRAME ')]
            loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
            valid = process.returncode == 0 and len(oracle) == 1 and oracle[0].get('success') is True
            valid &= len(loaded) == 1 and Path(loaded[0]).resolve() == native
            valid &= len(frames) == 1 and frames[0].get('v') == 2 and len(frames[0].get('f', [])) == 24
            valid &= 'Shader compiler selected: ' + ('Metal IR' if route == 'ir' else 'MSL') in text
            valid &= 'Metal GPU Validation Enabled' in text and '[mvk-error]' not in text
            valid &= 'MetalIR shader rejected:' not in text
            if route == 'ir':
                valid &= str(plugin) in text and text.count('MetalIR compiled stage') >= 2
            values = frames[0]['f'] if valid else None
            if values:
                # Every fixture submits exactly one VkCommandBuffer. Require the
                # native count too, so equal extra submits cannot pass unnoticed.
                valid &= values[0] == 1 and values[4] == 1 and values[5] == values[6] == values[19] == values[21] == 0
                valid &= values[14] == values[17] == 0
            observed = dict(zip(('commandBuffers', 'renderEncoders', 'blitEncoders', 'computeEncoders', 'passBreaks'),
                                [values[i] for i in (4, 10, 11, 12, 13)])) if values else None
            result = dict(case=label, valid=bool(valid), exitCode=process.returncode, counts=observed, oracle=oracle)
            results.append(result)
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps(result), flush=True)
            if not valid:
                raise RuntimeError('Invalid submission replay: ' + label)
            counts.append(observed)
        comparison = dict(mode=mode, counts=counts, exactParity=all(c == counts[0] for c in counts))
        comparisons.append(comparison)
        (out / 'comparison.json').write_text(json.dumps(comparisons, indent=2) + '\n')
        if not comparison['exactParity']:
            raise RuntimeError('Fixed-input submission mismatch: ' + mode)
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Input changed during replay'
    summary = dict(allPassed=True, runs=len(results), modes=len(comparisons),
                   scope='Fixed synthetic inputs, exact counts and GPU correctness. Game static/FPS acceptance remains separate.')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
