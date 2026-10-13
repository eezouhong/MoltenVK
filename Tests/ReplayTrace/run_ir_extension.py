"""CPU-only coverage for the IR replay extension and compile-off erasure."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    root = Path(__file__).resolve().parent
    include = root.parents[1] / 'MoltenVK/MoltenVK/GPUObjects'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = root / 'ir_extension_test.cpp'
    base = ['clang++', '-std=c++17', '-O2', '-pthread', '-I' + str(include)]
    results = []
    for enabled in (0, 1):
        binary = out / ('extension-' + str(enabled))
        with (out / ('compile-' + str(enabled) + '.log')).open('w') as log:
            subprocess.run([*base, '-DMVK_REPLAY_TRACE=' + str(enabled), str(source),
                            '-o', str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(binary)], check=True)
        results.append(dict(trace=bool(enabled), passed=True))
    obj = out / 'disabled.o'
    subprocess.run([*base, '-DMVK_REPLAY_TRACE=0', '-DTEST_DISABLED_OBJECT=1',
                    '-c', str(source), '-o', str(obj)], check=True)
    undefined = subprocess.check_output(['nm', '-u', str(obj)], text=True)
    (out / 'disabled-undefined.txt').write_text(undefined)
    if undefined.strip():
        raise RuntimeError('Disabled IR hook has external dependencies: ' + undefined)
    assembly = subprocess.check_output(['otool', '-tvV', str(obj)], text=True)
    (out / 'disabled-assembly.txt').write_text(assembly)
    result = dict(cases=results, disabledUndefinedSymbols=0, scope='CPU counters and compile gate')
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
