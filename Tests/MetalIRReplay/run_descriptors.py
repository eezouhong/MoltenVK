"""CPU-oracle descriptor edge cases. Run through the host graphics queue."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from run import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--route', choices=('ir', 'msl'), default='ir')
    parser.add_argument('--pool', action='store_true', help='Require a real Metal 4 pooled view assignment')
    parser.add_argument('--shader-validation', action='store_true')
    args = parser.parse_args()
    build, native, plugin = (p.resolve(strict=True) for p in (args.build, args.native, args.plugin))
    source = Path(__file__).resolve().parent
    files = [native, plugin, build / 'descriptor_probe', build / 'descriptors.comp.spv',
             source / 'descriptor_probe.cpp', source / 'vulkan_context.h', source / 'descriptors.comp',
             Path(__file__), source / 'run.py']
    identity = {str(p): sha(p) for p in files}
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    (out / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('MELONX_', 'MVK_CONFIG_', 'DYLD_', 'MTL_'))}
    env.update(MELONX_EXPERIMENTAL_METAL_IR='1' if args.route == 'ir' else '0',
               MELONX_METAL_IR_PLUGIN=str(plugin), DYLD_LIBRARY_PATH=str(native.parent), DYLD_PRINT_LIBRARIES='1',
               MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1', MVK_CONFIG_LOG_LEVEL='4',
               MVK_CONFIG_METAL4_COMPILER='1', MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',
               MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL=str(int(args.pool)), MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL_TELEMETRY='1',
               MTL_SHADER_VALIDATION=str(int(args.shader_validation)))
    process = subprocess.run([str(build / 'descriptor_probe'), str(build / 'descriptors.comp.spv')],
                             env=env, capture_output=True, text=True, timeout=65)
    text = process.stdout + process.stderr
    (out / 'execution.log').write_text(text)
    rows = [json.loads(line) for line in process.stdout.splitlines() if line.startswith('{')]
    loaded = re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$', text, re.M)
    binary_ok = len(loaded) == 1 and Path(loaded[0]).resolve() == native
    choice = 'Shader compiler selected: ' + ('Metal IR' if args.route == 'ir' else 'MSL')
    pool_active = 'Metal 4 texture view pool enabled' in text and bool(re.search(r'assignments=[1-9][0-9]*', text))
    validation_active = 'Metal GPU Validation Enabled' in text
    validation_errors = '[mvk-error]' in text or bool(re.search(r'(?:shader|gpu)[^\n]{0,80}validation[^\n]{0,30}(?:error|failed|out.of.bounds)', text, re.I))
    passed = not validation_errors and process.returncode == 0 and bool(rows) and rows[-1].get('success') is True and binary_ok and choice in text
    if args.route == 'ir':
        passed = passed and str(plugin) in text and 'MetalIR compiled stage' in text and 'MetalIR shader rejected:' not in text
    passed = passed and (not args.pool or pool_active) and (not args.shader_validation or validation_active)
    result = dict(passed=bool(passed), exitCode=process.returncode, route=args.route, binaryVerified=binary_ok,
                  poolActive=pool_active, shaderValidationActive=validation_active, validationErrors=validation_errors, oracle=rows)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    assert all(sha(Path(p)) == digest for p, digest in identity.items()), 'Replay input changed'
    print(json.dumps(result))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
