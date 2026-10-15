"""Build/sign an independent disposable sandbox. The paths helper uses no Metal API."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import subprocess
import uuid


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--build', type=Path, required=True, help='new dedicated output directory')
    p.add_argument('--source-manifest', type=Path, required=True)
    p.add_argument('--developer-dir', type=Path, required=True)
    a = p.parse_args()
    a.build.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parent
    owner = str(uuid.uuid4())
    identifier = 'com.eezouhong.ir-pso-archive-phase2-' + owner
    app = a.build / 'IRPSOArchiveProbe.app'
    contents = app / 'Contents'
    (contents / 'MacOS').mkdir(parents=True)
    executable = contents / 'MacOS/ir-pso-archive-probe'
    compiler = a.developer_dir / 'Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++'
    sdk = a.developer_dir / 'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk'
    command = [str(compiler), '-isysroot', str(sdk), '-std=c++17', '-O2', '-fobjc-arc', '-fblocks', '-mmacosx-version-min=26.0',
               '-framework', 'Foundation', '-framework', 'Metal', str(source / 'probe.mm'), '-o', str(executable)]
    env = dict(os.environ, DEVELOPER_DIR=str(a.developer_dir))
    with (a.build / 'build.log').open('w') as log:
        subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(['python3', str(source / 'make_inputs.py'), '--source', str(a.source_manifest),
                    '--resources', str(contents / 'Resources')], check=True)
    (contents / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleIdentifier': identifier, 'CFBundleExecutable': executable.name, 'CFBundlePackageType': 'APPL',
        'CFBundleVersion': '1', 'MeloNXPhase2CacheOwnerId': owner}))
    entitlements = a.build / 'entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({'com.apple.security.app-sandbox': True}))
    subprocess.run(['codesign', '--force', '--sign', '-', '--entitlements', str(entitlements), str(app)], check=True)
    metadata = json.loads(subprocess.check_output([str(executable), 'paths'], text=True))
    metadata.update(ownerId=owner, uid=os.getuid())
    namespace = Path(metadata['darwinUserCacheDirectory']).resolve()
    namespace.mkdir(parents=True, exist_ok=True)
    marker = namespace / 'phase2-owner.json'
    if marker.exists():
        raise RuntimeError('owner marker already exists; never replace another sandbox owner')
    marker.write_text(json.dumps({'ownerId': owner}) + '\n')
    (a.build / 'owned-cache.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (a.build / 'build-command.json').write_text(json.dumps(command, indent=2) + '\n')
    print(json.dumps({'probe': str(executable), 'metadata': str(a.build / 'owned-cache.json'),
                      'outputRoot': str(Path(metadata['homeDirectory']) / 'tmp')}))


if __name__ == '__main__':
    main()
