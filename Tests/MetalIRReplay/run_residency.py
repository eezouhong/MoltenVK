"""Read-only snapshots and existing pressure hooks. Run via shared graphics queue."""
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
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in (args.build, args.native, args.plugin))
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    inputs = [native, plugin, build / 'runtime_probe', Path(__file__), Path(__file__).with_name('runtime_probe.cpp')]
    identity = {str(p): sha(p) for p in inputs}
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    base = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    base.update(MELONX_RESIDENCY_PROBE='1', MELONX_METAL_IR_PLUGIN=str(plugin),
                MELONX_METAL_IR_TELEMETRY='1', DYLD_LIBRARY_PATH=str(native.parent),
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1', MVK_CONFIG_METAL4_COMPILER='1',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0', MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0',
                MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1', MVK_CONFIG_LOG_LEVEL='4', MTL_SHADER_VALIDATION='1',
                MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_REPOSITORY_ENABLED='1',
                MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_RESIDENT_LIMIT='512')
    results = []
    for route in ('MSL', 'IR'):
        for mode in ('compute', 'graphics'):
            for epoch in (('cold', 'warm') if route == 'IR' else ('cold',)):
                env = dict(base, MELONX_EXPERIMENTAL_METAL_IR='1' if route == 'IR' else '0')
                names = ('runtime.comp.spv', 'unused.comp.spv') if mode == 'compute' else ('runtime.vert.spv', 'runtime.frag.spv')
                command = [str(build / 'runtime_probe'), mode, *[str(build / p) for p in names]]
                if route == 'IR': command.append(str(out / (mode + '-cache')))
                process = subprocess.run(command, env=env, capture_output=True, text=True, timeout=70)
                log = process.stdout + process.stderr
                label = '-'.join((route, mode, epoch)); (out / (label + '.log')).write_text(log)
                oracle = [json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
                snapshot = [json.loads(s.partition('MELONX_RESIDENCY ')[2]) for s in process.stderr.splitlines() if s.startswith('MELONX_RESIDENCY ')]
                valid = process.returncode == 0 and len(oracle) == len(snapshot) == 1 and oracle[0].get('success') is True
                valid &= 'Metal GPU Validation Enabled' in log and '[mvk-error]' not in log
                if snapshot:
                    valid &= snapshot[0]['limit'] == 512
                    valid &= snapshot[0]['readOnly'] and snapshot[0]['trimPreserved']
                    if route == 'IR': valid &= snapshot[0]['restores'] == (2 if epoch == 'warm' else 0)
                results.append(dict(case=label, valid=bool(valid), exitCode=process.returncode, snapshot=snapshot, oracle=oracle))
                (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
                print(json.dumps(results[-1]), flush=True)
                if not valid: raise RuntimeError('Residency probe failed: ' + label)
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Inputs changed during replay'


if __name__ == '__main__':
    main()
