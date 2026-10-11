"""Static no-raster vertex writes, later pixel consumption and cache restore.

Run through the shared graphics queue. Inputs are public synthetic shaders.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
from run import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('build', 'native', 'plugin', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--baseline-plugin', type=Path)
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in (args.build, args.native, args.plugin))
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parent
    names = ('discard_producer.vert.spv', 'discard_positionless.vert.spv',
             'discard_consumer.vert.spv', 'discard_guard.frag.spv', 'discard_consumer.frag.spv')
    files = [native, plugin, *plugin.parent.glob('*.dylib'), build / 'discard_probe',
             *[build / name for name in names], source / 'discard_probe.cpp',
             source / 'vulkan_context.h', Path(__file__)]
    if args.baseline_plugin: files.append(args.baseline_plugin.resolve(strict=True))
    identity = {str(p): sha(p) for p in files}
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    base = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    base.update(MELONX_METAL_IR_PLUGIN=str(plugin), DYLD_LIBRARY_PATH=str(native.parent),
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1', MVK_CONFIG_METAL4_COMPILER='0',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0', MVK_CONFIG_LOG_LEVEL='4',
                MTL_ENABLE_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1')
    results = []

    def run(route, position, fragment, draw, phase, cache, selected_plugin=plugin):
        label = '-'.join((route, position, fragment, draw, phase))
        producer = 'discard_positionless.vert.spv' if position == 'absent' else 'discard_producer.vert.spv'
        env = dict(base, MELONX_EXPERIMENTAL_METAL_IR='1' if route == 'ir' else '0',
                   MELONX_METAL_IR_PLUGIN=str(selected_plugin), MELONX_METAL_IR_CACHE=str(cache))
        command = [str(build / 'discard_probe'), fragment, draw,
                   *[str(build / n) for n in (producer, *names[2:])]]
        process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=55)
        text = process.stdout + process.stderr; (out / (label + '.log')).write_text(text)
        rows = [json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
        compiled, restored = text.count('MetalIR compiled stage'), text.count('MetalIR restored stage')
        mismatch = phase == 'old-compiler'
        if mismatch:
            valid = process.returncode != 0 and 'compiler lacks the rasterization-disabled vertex variant' in text
            valid &= 'MSL fallback disabled' in text and not rows
        else:
            valid = process.returncode == 0 and len(rows) == 3
            valid &= len(rows) == 3 and rows[0].get('correct') == rows[0].get('total') == 16
            valid &= len(rows) == 3 and rows[1].get('pixels') == rows[1].get('total') == 256
            valid &= len(rows) == 3 and Path(rows[2].get('loadedNative', '')).resolve() == native
            valid &= 'Metal GPU Validation Enabled' in text and '[mvk-error]' not in text
            valid &= 'Shader compiler selected: ' + ('Metal IR' if route == 'ir' else 'MSL') in text
            if route == 'ir': valid &= (compiled, restored) == ((0, 3) if phase == 'restored' else (3, 0))
        result = dict(case=label, passed=bool(valid), exitCode=process.returncode,
                      compiled=compiled, restored=restored, expectedCompilerMismatch=mismatch, oracle=rows)
        results.append(result); (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(result), flush=True)
        if not valid: raise RuntimeError('Discard oracle failed: ' + label)

    for position in ('outside', 'absent'):
        for fragment in ('none', 'fragment'):
            for draw in ('direct', 'indexed'):
                cache = out / ('cache-' + '-'.join((position, fragment, draw)))
                run('msl', position, fragment, draw, 'control', cache)
                run('ir', position, fragment, draw, 'cold', cache)
                run('ir', position, fragment, draw, 'restored', cache)
    if args.baseline_plugin:
        run('ir', 'outside', 'none', 'direct', 'old-compiler', out / 'old-cache', args.baseline_plugin.resolve())
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Input changed during replay'
    summary = dict(allPassed=True, cases=len(results),
                   scope='Independent vertex-store, fragment nonexecution and consumer-pixel oracle; static discard only.')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
