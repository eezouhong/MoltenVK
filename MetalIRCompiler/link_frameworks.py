#!/usr/bin/env python3
"""Link verified compiler dependencies into host/device framework slices."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
EXPORTS = ['MeloNXCompileMetalIR', 'MeloNXReleaseMetalIR',
           'MeloNXMetalIRABIVersion', 'MeloNXMetalIRDependencyIdentity']


def output(args):
    return subprocess.check_output([str(p) for p in args], text=True).strip()


def run(args):
    subprocess.run([str(p) for p in args], check=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def framework(directory, platform):
    path = directory / 'MeloNXMetalIR.framework'
    path.mkdir(parents=True)
    info = {'CFBundleExecutable': 'MeloNXMetalIR', 'CFBundleIdentifier': 'org.melonx.experimental.metalir',
            'CFBundleName': 'MeloNXMetalIR', 'CFBundlePackageType': 'FMWK',
            'CFBundleVersion': '1', 'CFBundleShortVersionString': '1.0',
            'CFBundleSupportedPlatforms': [platform]}
    if platform == 'iPhoneOS':
        info['MinimumOSVersion'] = '17.0'
    else:
        info['LSMinimumSystemVersion'] = '26.0'
    (path / 'Info.plist').write_bytes(plistlib.dumps(info))
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dependencies', type=Path, required=True,
                        help='iOS dependency-identity.json produced by build-ios.sh')
    parser.add_argument('--host-build', type=Path, required=True)
    parser.add_argument('--msc-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if out == ROOT.parent or ROOT.parent in out.parents or out in ROOT.parent.parents:
        parser.error('output must be outside the checkout')
    if out.exists() and any(out.iterdir()):
        parser.error('use an empty output directory')
    if not output(['xcrun', 'xcodebuild', '-version']).startswith('Xcode 27'):
        parser.error('select Xcode 27')
    deps = json.loads(args.dependencies.read_text())
    pin = json.loads((ROOT / 'msc/VERSION').read_text())
    host_identity = json.loads((args.host_build / 'identity.json').read_text())
    if deps['llvmVersion'] != '17.0.6' or deps['deploymentTarget'] != '17.0':
        parser.error('unexpected iOS dependencies')
    if host_identity['mscVersion'] != pin['version']:
        parser.error('host and device MSC versions must match')
    for file, identity in deps['archives'].items():
        if sha(Path(file)) != identity['sha256']:
            parser.error('iOS dependency changed: ' + file)
    for name, identity in host_identity['binaries'].items():
        if sha(args.host_build / 'lib' / name) != identity['sha256']:
            parser.error('host dependency changed: ' + name)
    msc = args.msc_dir / pin['iOSLibrary']
    if sha(msc) != pin['iOSLibrarySHA256']:
        parser.error('iOS MSC does not match the pin')
    for header in pin['headers']:
        if sha(args.msc_dir / header['path']) != header['sha256']:
            parser.error('MSC header changed: ' + header['path'])
    llvm_source = Path(deps['llvmSource']) / 'llvm'
    llvm = Path(deps['llvmBuild'])
    mesa = Path(deps['mesaBuild'])
    mesa_source = mesa.parent / 'mesa-source'
    sdk = output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'])
    cxx = output(['xcrun', '--sdk', 'iphoneos', '--find', 'clang++'])
    out.mkdir(parents=True, exist_ok=True)
    ios = framework(out / 'ios-arm64', 'iPhoneOS')
    mac = framework(out / 'macos-arm64', 'MacOSX')
    objects = out / 'objects'
    objects.mkdir()
    flags = ['-std=c++17', '-O2', '-fno-rtti', '-fvisibility=hidden', '-DNDEBUG',
             '-arch', 'arm64', '-isysroot', sdk, '-miphoneos-version-min=17.0',
             '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/air'),
             '-I' + str(llvm_source / 'include'), '-I' + str(llvm / 'include')]
    sources = [ROOT / 'src/air/BitcodeWriter.cpp', ROOT / 'src/native_raster_adapter.cpp', ROOT / 'src/air_math_adapter.cpp',
               ROOT / 'src/compiler_plugin.mm']
    identity_data = {'msc': sha(msc), 'archives': deps['archives'],
                     'sources': {str(p.relative_to(ROOT)): sha(p) for p in sources},
                     'target': ['iOS', 'arm64', '17.0']}
    # Hash contents and stable archive names, independent of dependency paths.
    identity_data['archives'] = {Path(p).name: item['sha256'] for p, item in deps['archives'].items()}
    identity = hashlib.sha256(json.dumps(identity_data, sort_keys=True).encode()).hexdigest()
    flags += ['-I' + str(ROOT.parent / 'MoltenVK/MoltenVK/GPUObjects'),
              '-I' + str(mesa_source / 'src/microsoft/spirv_to_dxil'),
              '-I' + str(mesa_source / 'src/microsoft/compiler'),
              '-I' + str(args.msc_dir / 'include'),
              '-DMELONX_METAL_IR_DEPENDENCY_IDENTITY="' + identity + '"']
    compiled = []
    for source in sources:
        target = objects / (source.stem + '.o')
        run([cxx, *flags, '-c', source, '-o', target])
        compiled.append(target)
    archives = [Path(p) for p in deps['archives']]
    run([cxx, '-dynamiclib', '-arch', 'arm64', '-isysroot', sdk,
         '-miphoneos-version-min=17.0', *compiled, *archives, msc,
         '-Wl,-dead_strip', '-Wl,-install_name,@rpath/MeloNXMetalIR.framework/MeloNXMetalIR',
         '-Wl,-rpath,@executable_path/Frameworks',
         *['-Wl,-exported_symbol,_' + name for name in EXPORTS],
         '-o', ios / 'MeloNXMetalIR'])
    for name in ['libMeloNXMetalIR.dylib', 'libspirv_to_dxil.dylib', 'libmetalirconverter.dylib']:
        target = mac / ('MeloNXMetalIR' if name == 'libMeloNXMetalIR.dylib' else name)
        shutil.copyfile(args.host_build / 'lib' / name, target)
    run(['xcrun', 'install_name_tool', '-id', '@rpath/MeloNXMetalIR.framework/MeloNXMetalIR', mac / 'MeloNXMetalIR'])
    run(['codesign', '--force', '--sign', '-', mac / 'MeloNXMetalIR'])
    for slice in [ios, mac]:
        notices = slice / 'Resources'
        notices.mkdir()
        for name in ['LICENSE.txt', 'Acknowledgements.rtf']:
            shutil.copyfile(args.msc_dir / 'include/metal_irconverter' / name, notices / ('MSC-' + name))
        shutil.copyfile(ROOT / 'src/air/LICENSE.TXT', notices / 'LLVM-LICENSE.TXT')
        binary = slice / 'MeloNXMetalIR'
        exported = [line.split()[-1][1:] for line in output(['nm', '-gU', binary]).splitlines()
                    if line.split() and line.split()[-1].startswith('_')]
        if sorted(exported) != sorted(EXPORTS):
            raise RuntimeError('unexpected compiler exports')
    # Keep the Apple-signed MSC input unchanged; Xcode will Embed & Sign this
    # standalone iOS dylib into the app's Frameworks directory, as XeniOS does.
    vendor = out / 'ios-runtime'
    vendor.mkdir()
    shutil.copyfile(msc, vendor / 'libmetalirconverter.dylib')
    run(['xcrun', 'xcodebuild', '-create-xcframework', '-framework', ios,
         '-framework', mac, '-output', out / 'MeloNXMetalIR.xcframework'])
    report = {'version': pin['version'], 'iOSDependencyIdentity': identity,
              'binaries': {str(p.relative_to(out)): {'sha256': sha(p), 'bytes': p.stat().st_size}
                           for p in [ios / 'MeloNXMetalIR', mac / 'MeloNXMetalIR', vendor / 'libmetalirconverter.dylib']},
              'iOSLinkedLibraries': output(['otool', '-L', ios / 'MeloNXMetalIR']),
              'iOSBuildVersion': output(['xcrun', 'vtool', '-show-build', ios / 'MeloNXMetalIR']),
              'status': 'linked; device loading and performance unverified'}
    (out / 'framework-identity.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
