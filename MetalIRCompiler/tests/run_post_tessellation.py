"""Build and run the post-tessellation vertex probe (MVK_METAL_IR_POST_TESSELLATION_*) on this Mac's GPU.

The probe compiles a synthetic SPIR-V vertex stage through the plugin as a Metal post-tessellation vertex function,
checks its patch type and the plugin's proof flag, draws two patches with drawPatches under Metal API and shader
validation, and checks every covered pixel against an affine oracle (domain coordinate and patch index). Quads and
triangles, integer and fractional factors, up to the maximum of 64. GPU work: run it through the Debug Tool queue.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


CASES = [(domain, factor) for domain in ('quad', 'triangle') for factor in ('1', '3', '7.5', '64')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plugin', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    plugin = args.plugin.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    subprocess.run(['python3', str(root / 'tests/prepare_post_tessellation.py'), '--output', str(args.output)],
                   check=True, capture_output=True)
    spirv = args.output / 'post_tessellation.spv'
    probe = args.output / 'post_tessellation_probe'
    source = root / 'tests/post_tessellation_probe.mm'
    subprocess.run([args.cxx, '-std=c++17', '-O2', '-fobjc-arc', '-I' + str(root.parent / 'MoltenVK/MoltenVK/GPUObjects'),
                    str(source), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(probe)], check=True)
    environment = dict(os.environ, MTL_DEBUG_LAYER='1', MTL_SHADER_VALIDATION='1')
    results = []
    for domain, factor in CASES:
        run = subprocess.run([str(probe), str(plugin), str(spirv), domain, factor], env=environment,
                             capture_output=True, text=True)
        line = next((l for l in reversed(run.stdout.splitlines()) if l.startswith('{')), None)
        result = json.loads(line) if line else {'passed': False, 'error': run.stderr[-500:]}
        result['returncode'] = run.returncode
        results.append(result)
        print(json.dumps(result))
    passed = all(r['passed'] and r['returncode'] == 0 for r in results)
    tracked = [source, root / 'tests/post_tessellation.vert', Path(__file__), plugin, spirv, probe]
    summary = dict(passed=passed, gpuExecuted=True, validation='MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1',
                   cases=results, sha256={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked})
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(dict(passed=passed, cases=len(results))))
    raise SystemExit(0 if passed else 1)


if __name__ == '__main__':
    main()
