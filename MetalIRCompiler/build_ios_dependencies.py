#!/usr/bin/env python3
"""Build iOS arm64 Mesa/LLVM prerequisites; does not install or execute on a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def output(args):
    return subprocess.check_output([str(a) for a in args], text=True).strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependencies-only', action='store_true', required=True,
                        help='framework linking awaits the unified MSC version')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--llvm-source', type=Path, required=True,
                        help='LLVM 17.0.6 source root containing llvm/ and cmake/')
    parser.add_argument('--mesa-repository', default='https://gitlab.freedesktop.org/has207/mesa.git')
    parser.add_argument('--meson', default='meson')
    parser.add_argument('--ninja', default='ninja')
    parser.add_argument('--host-tablegen', type=Path,
                        help='reuse a previously source-built LLVM 17.0.6 host tablegen')
    parser.add_argument('--llvm-build', type=Path,
                        help='reuse verified iOS LLVM static libraries from an earlier dependency build')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    out = args.output.resolve()
    checkout = ROOT.parent
    if out == checkout or checkout in out.parents or out in checkout.parents:
        parser.error('output must be outside the source checkout')
    if out.exists() and any(out.iterdir()):
        parser.error('use a new empty output directory')
    if args.jobs < 1:
        parser.error('jobs must be positive')
    llvm_source = args.llvm_source.resolve()
    llvm_cmake = llvm_source / 'llvm/CMakeLists.txt'
    if not llvm_cmake.is_file() or 'set(LLVM_VERSION_MAJOR 17)' not in llvm_cmake.read_text():
        parser.error('LLVM 17.0.6 source is required')
    for name, value in (('MINOR', 0), ('PATCH', 6)):
        if f'set(LLVM_VERSION_{name} {value})' not in llvm_cmake.read_text():
            parser.error('LLVM 17.0.6 source is required')
    if not output(['xcrun', 'xcodebuild', '-version']).startswith('Xcode 27'):
        parser.error('select Xcode 27 with DEVELOPER_DIR')
    sdk = output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'])
    host_sdk = output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'])
    cc = output(['xcrun', '--sdk', 'iphoneos', '--find', 'clang'])
    cxx = output(['xcrun', '--sdk', 'iphoneos', '--find', 'clang++'])
    meson = shutil.which(args.meson)
    ninja = shutil.which(args.ninja)
    if not meson or not ninja:
        parser.error('Meson and Ninja must be available')
    env = dict(os.environ, PATH=os.pathsep.join([
        str(Path(meson).parent), str(Path(ninja).parent), os.environ['PATH']]))
    out.mkdir(parents=True, exist_ok=True)
    host = out / 'llvm-host-tools'
    llvm = out / 'llvm-ios'
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DLLVM_TARGETS_TO_BUILD=',
              '-DLLVM_ENABLE_PROJECTS=', '-DLLVM_ENABLE_RUNTIMES=',
              '-DLLVM_ENABLE_ZSTD=OFF', '-DLLVM_ENABLE_ZLIB=OFF',
              '-DLLVM_ENABLE_TERMINFO=OFF', '-DLLVM_ENABLE_LIBXML2=OFF',
              '-DLLVM_INCLUDE_TOOLS=OFF', '-DLLVM_INCLUDE_TESTS=OFF',
              '-DLLVM_INCLUDE_EXAMPLES=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF',
              '-DLLVM_ENABLE_BINDINGS=OFF', '-DLLVM_BUILD_LLVM_DYLIB=OFF',
              '-DLLVM_LINK_LLVM_DYLIB=OFF', '-DLLVM_ENABLE_RTTI=OFF',
              '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DCMAKE_C_COMPILER=' + cc,
              '-DCMAKE_CXX_COMPILER=' + cxx]
    # Never trust a misleading llvm@17 symlink. Bootstrap tablegen from the
    # same source revision used for the target libraries.
    if args.host_tablegen:
        tblgen = args.host_tablegen.resolve()
    else:
        run(['cmake', '-S', llvm_source / 'llvm', '-B', host, *common,
             '-DCMAKE_OSX_SYSROOT=' + host_sdk, '-DCMAKE_OSX_ARCHITECTURES=arm64'], env=env)
        run(['cmake', '--build', host, '--target', 'llvm-tblgen', '--parallel', args.jobs], env=env)
        tblgen = host / 'bin/llvm-tblgen'
    if 'LLVM version 17.0.6' not in output([tblgen, '--version']):
        raise RuntimeError('bootstrapped tablegen version mismatch')
    if args.llvm_build:
        llvm = args.llvm_build.resolve()
        cache = {}
        for line in (llvm / 'CMakeCache.txt').read_text().splitlines():
            if line and not line.startswith(('#', '//')) and '=' in line:
                key, value = line.split('=', 1)
                cache[key.split(':', 1)[0]] = value
        expected = {'CMAKE_SYSTEM_NAME': 'iOS', 'CMAKE_OSX_SYSROOT': sdk,
                    'CMAKE_OSX_ARCHITECTURES': 'arm64', 'CMAKE_OSX_DEPLOYMENT_TARGET': '17.0',
                    'CMAKE_HOME_DIRECTORY': str(llvm_source / 'llvm'),
                    **{f'LLVM_ENABLE_{name}': 'OFF' for name in ('ZSTD', 'ZLIB', 'TERMINFO', 'LIBXML2')}}
        if any(cache.get(k) != v for k, v in expected.items()):
            raise RuntimeError('reused LLVM build has incompatible source or target configuration')
        for name in ('LLVMCore', 'LLVMBitReader', 'LLVMBitWriter'):
            if not (llvm / 'lib' / ('lib' + name + '.a')).is_file():
                raise RuntimeError('reused LLVM build is incomplete')
    else:
        run(['cmake', '-S', llvm_source / 'llvm', '-B', llvm, *common,
             '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_OSX_SYSROOT=' + sdk,
             '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=17.0',
             '-DCMAKE_MACOSX_BUNDLE=OFF', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
             '-DLLVM_TABLEGEN=' + str(tblgen)], env=env)
        run(['cmake', '--build', llvm, '--target', 'LLVMCore', 'LLVMBitReader', 'LLVMBitWriter',
             '--parallel', args.jobs], env=env)
    source = out / 'mesa-source'
    build = out / 'mesa-ios'
    revision = (ROOT / 'mesa/MESA_COMMIT').read_text().strip()
    run(['git', 'init', '-q', source])
    run(['git', '-C', source, 'fetch', '--depth=1', args.mesa_repository, revision])
    run(['git', '-C', source, 'checkout', '--detach', '-q', 'FETCH_HEAD'])
    for patch in sorted((ROOT / 'mesa/patches').glob('*.patch')):
        run(['git', '-C', source, 'apply', '--check', patch])
        run(['git', '-C', source, 'apply', patch])
    cross = out / 'ios-arm64.ini'
    compiler_flags = ['-isysroot', sdk, '-arch', 'arm64', '-miphoneos-version-min=17.0']
    cross.write_text('[binaries]\n' +
        'c = ' + repr([cc, *compiler_flags]) + '\n' +
        'cpp = ' + repr([cxx, *compiler_flags]) + '\n' +
        'objc = ' + repr([cc, *compiler_flags]) + '\n' +
        'objcpp = ' + repr([cxx, *compiler_flags]) + '\n' +
        'ar = ' + repr(output(['xcrun', '--find', 'ar'])) + '\n' +
        'strip = ' + repr(output(['xcrun', '--find', 'strip'])) + '\n' +
        "[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n" +
        '[properties]\nneeds_exe_wrapper = true\n')
    options = ['-D' + line.replace('default_library=shared', 'default_library=static')
               for line in (ROOT / 'mesa/meson-options.txt').read_text().splitlines()
               if line and not line.startswith('#')]
    run([meson, 'setup', build, source, '--cross-file', cross, *options], env=env)
    targets = json.loads(output([meson, 'introspect', '--targets', build]))
    libraries = [p for target in targets if target['type'] == 'static library'
                 for p in target['filename'] if p.endswith('.a')]
    # Build all static prerequisites only. No target binary is executed.
    run([ninja, '-C', build, '-j', args.jobs,
         *[str(Path(p).relative_to(build)) for p in libraries]], env=env)
    archives = [*llvm.glob('lib/*.a'), *[Path(p) for p in libraries]]
    manifest = {'llvmVersion': '17.0.6', 'mesaRevision': revision,
                'sdk': sdk, 'deploymentTarget': '17.0', 'architecture': 'arm64',
                'tablegen': {'sha256': sha(tblgen), 'version': output([tblgen, '--version'])},
                'llvmSource': str(llvm_source), 'llvmBuild': str(llvm), 'mesaBuild': str(build),
                'archives': {str(p): {'sha256': sha(p), 'bytes': p.stat().st_size}
                             for p in archives},
                'sourceCommit': output(['git', '-C', ROOT, 'rev-parse', 'HEAD']),
                'status': 'dependencies only; plugin loading and device performance unverified'}
    (out / 'dependency-identity.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'status': manifest['status'], 'archiveCount': len(archives)}))


if __name__ == '__main__':
    main()
