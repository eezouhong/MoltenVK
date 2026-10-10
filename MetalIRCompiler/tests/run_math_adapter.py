"""Build and run the CPU-only AIR permission contract with the pinned LLVM SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess


def output(command):
    return subprocess.check_output(command, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-config', type=Path, required=True)
    parser.add_argument('--zstd-library', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--fixture', type=Path)
    args = parser.parse_args()
    llvm = str(args.llvm_config.resolve(strict=True))
    if output([llvm, '--version']) != '17.0.6':
        parser.error('LLVM 17.0.6 is required')
    root = Path(__file__).resolve().parents[1]
    include = output([llvm, '--includedir'])
    lib = Path(output([llvm, '--libdir']))
    components = ['--link-static', 'core', 'bitreader', 'bitwriter']
    libraries = [lib / name for name in shlex.split(output([llvm, '--libnames', *components]))]
    system = shlex.split(output([llvm, '--system-libs', *components]))
    if '-lzstd' in system:
        if not args.zstd_library:
            parser.error('this LLVM SDK requires --zstd-library')
        system = [str(args.zstd_library.resolve(strict=True)) if name == '-lzstd' else name
                  for name in system]
    sources = [root / 'src/air_math_adapter.cpp', root / 'src/air_math_adapter.h',
               root / 'tests/math_adapter_test.cpp', Path(__file__)]
    args.output.mkdir(parents=True, exist_ok=False)
    binary = args.output / 'math_adapter_test'
    subprocess.run([args.cxx, '-std=c++17', '-O2', '-fno-rtti', '-I' + include,
                    '-I' + str(root / 'src'), str(sources[0]), str(sources[2]),
                    *map(str, libraries), *system, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    if args.fixture:
        subprocess.run([str(binary), str(args.fixture.resolve(strict=True))], check=True)
    tracked = [*sources, *libraries, binary]
    if args.fixture:
        tracked.append(args.fixture.resolve())
    result = dict(passed=True, llvmVersion='17.0.6', assertionsEnabled=True,
                  gpuExecuted=False, scope='Structural AIR permission mutation; GPU execution is a separate gate.',
                  sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in tracked})
    (args.output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'sha256'}))


if __name__ == '__main__':
    main()
