"""Per-device cache cold/restore/corruption GPU oracle. Run via graphics queue."""
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
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parent
    files = [native, plugin, *plugin.parent.glob('*.dylib'), build / 'runtime_probe',
             *[build / p for p in ('runtime.vert.spv', 'runtime.frag.spv', 'runtime.comp.spv', 'unused.comp.spv')],
             source / 'runtime_probe.cpp', source / 'vulkan_context.h', Path(__file__)]
    identity = {str(p): sha(p) for p in files}
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    base = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    base.update(MELONX_EXPERIMENTAL_METAL_IR='1', MELONX_METAL_IR_PLUGIN=str(plugin),
                MELONX_METAL_IR_TELEMETRY='1', MELONX_PIPELINE_REPLAY_TRACE='coarse',
                DYLD_LIBRARY_PATH=str(native.parent), DYLD_PRINT_LIBRARIES='1',
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1', MVK_CONFIG_METAL4_COMPILER='1',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0', MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0',
                MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1', MVK_CONFIG_LOG_LEVEL='4', MTL_SHADER_VALIDATION='1')
    results = []
    # A helper compiled before cache configuration must not prevent persistence
    # for title shaders. Its retained artifact itself is not a disk-cache hit.
    process = subprocess.run([str(build / 'runtime_probe'), 'compute-late',
                              str(build / 'runtime.comp.spv'), str(build / 'unused.comp.spv'),
                              str(out / 'late-configuration-cache')],
                             env=base, capture_output=True, text=True, timeout=70)
    text = process.stdout + process.stderr
    (out / 'late-configuration.log').write_text(text)
    oracle = [json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
    valid = process.returncode == 0 and len(oracle) == 1 and oracle[0].get('success') is True
    valid &= len(list((out / 'late-configuration-cache').glob('*.mir'))) == 1
    valid &= '[mvk-error]' not in text and 'Metal GPU Validation Enabled' in text
    result = dict(case='late-configuration', valid=bool(valid), exitCode=process.returncode, oracle=oracle)
    results.append(result); (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(result), flush=True)
    if not valid: raise RuntimeError('Cache configuration after internal helper failed')
    for mode in ('graphics', 'compute'):
        cache = out / (mode + '-cache')
        names = ('runtime.comp.spv', 'unused.comp.spv') if mode == 'compute' else ('runtime.vert.spv', 'runtime.frag.spv')
        for stage in ('cold', 'restored', 'corrupt'):
            if stage == 'corrupt':
                records = sorted(cache.glob('*.mir')); assert len(records) == 2
                for path in records:
                    payload = bytearray(path.read_bytes()); payload[-1] ^= 1; path.write_bytes(payload)
            process = subprocess.run([str(build / 'runtime_probe'), mode, *[str(build / p) for p in names], str(cache)],
                                     env=base, capture_output=True, text=True, timeout=70)
            text = process.stdout + process.stderr
            label = mode + '-' + stage; (out / (label + '.log')).write_text(text)
            oracle = [json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
            stats = [json.loads(s.partition('MELONX_REPLAY_CACHE ')[2])
                     for s in process.stderr.splitlines() if s.startswith('MELONX_REPLAY_CACHE ')]
            loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
            valid = process.returncode == 0 and len(oracle) == len(stats) == 1 and oracle[0].get('success') is True
            valid &= len(loaded) == 1 and Path(loaded[0]).resolve() == native
            valid &= 'Metal GPU Validation Enabled' in text and '[mvk-error]' not in text
            valid &= 'Shader compiler selected: Metal IR' in text and 'MetalIR shader rejected:' not in text
            valid &= 'MELONX_METAL_IR_CACHE' not in base and len(list(cache.glob('*.mir'))) == 2
            if stats:
                expected = {'compiled': 0, 'restored': 2} if stage == 'restored' else {'compiled': 2, 'restored': 0}
                valid &= all(stats[0][key] == value for key, value in expected.items())
                if stage == 'restored':
                    valid &= stats[0]['mesaNs'] == stats[0]['mscNs'] == 0
            result = dict(case=label, valid=bool(valid), exitCode=process.returncode, statistics=stats, oracle=oracle)
            results.append(result); (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(json.dumps(result), flush=True)
            if not valid: raise RuntimeError('Cache replay failed: ' + label)
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Input changed during replay'
    summary = dict(allPassed=True, runs=len(results), scope='Per-device API with no cache environment override; restored GPU output and zero Mesa/MSC work; corruption recompiles through IR.')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    main()
