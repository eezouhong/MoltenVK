"""PointSize/PointCoord, topology exclusion and persistence. Graphics queue only."""
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
    files = [native, plugin, *plugin.parent.glob('*.dylib'), build / 'point_probe',
             *[build / p for p in ('points.vert.spv', 'points.frag.spv', 'points_plain.frag.spv')],
             source / 'point_probe.cpp', source / 'vulkan_context.h', Path(__file__)]
    identity = {str(p): sha(p) for p in files}
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    base = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    base.update(MELONX_METAL_IR_PLUGIN=str(plugin), DYLD_LIBRARY_PATH=str(native.parent), DYLD_PRINT_LIBRARIES='1',
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1', MVK_CONFIG_METAL4_COMPILER='1',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0', MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0',
                MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1', MVK_CONFIG_LOG_LEVEL='4', MTL_SHADER_VALIDATION='1')
    results = []

    def run(label, route, size, cache=None, restored=False):
        fragment = 'points_plain.frag.spv' if size == 'triangle' else 'points.frag.spv'
        command = [str(build / 'point_probe'), str(build / 'points.vert.spv'), str(build / fragment), size]
        if cache is not None: command.append(str(cache))
        process = subprocess.run(command, env=dict(base, MELONX_EXPERIMENTAL_METAL_IR='1' if route == 'ir' else '0'),
                                 capture_output=True, text=True, timeout=65)
        text = process.stdout + process.stderr; (out / (label + '.log')).write_text(text)
        rows = [json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
        loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
        valid = process.returncode == 0 and len(rows) == 1 and rows[0].get('success') is True
        valid &= len(loaded) == 1 and Path(loaded[0]).resolve() == native
        valid &= 'Metal GPU Validation Enabled' in text and '[mvk-error]' not in text
        valid &= 'Shader compiler selected: ' + ('Metal IR' if route == 'ir' else 'MSL') in text
        valid &= 'MetalIR shader rejected:' not in text
        if rows:
            valid &= rows[0]['correctPixels'] == rows[0]['totalPixels'] == 256
            valid &= rows[0]['coveredPixels'] == (256 if size == 'triangle' else int(size)**2)
        compiled, hits = text.count('MetalIR compiled stage'), text.count('MetalIR restored stage')
        if route == 'ir': valid &= (compiled, hits) == ((0, 2) if restored else (2, 0))
        result = dict(case=label, valid=bool(valid), exitCode=process.returncode, compiled=compiled, restored=hits, output=rows)
        results.append(result); (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(result), flush=True)
        if not valid: raise RuntimeError('Point replay failed: ' + label)

    for size in ('1', '2', '4', '8', 'triangle'):
        for route in ('msl', 'ir'): run(route + '-' + size, route, size)
    for size in ('4', 'triangle'):
        cache = out / (size + '-cache')
        run('ir-' + size + '-cold', 'ir', size, cache)
        run('ir-' + size + '-restored', 'ir', size, cache, restored=True)
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Input changed during replay'
    summary = dict(allPassed=True, runs=len(results),
                   scope='Synthetic CPU point coverage/coordinate oracle plus triangle topology and cache restoration. Full game loading-screen visual proof remains separate.')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n');print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
