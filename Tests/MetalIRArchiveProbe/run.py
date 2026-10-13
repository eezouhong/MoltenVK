"""Bounded, sequential legacy archive diagnostics. Canonical graphics lease required."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def ownership(probe, metadata):
    info = plistlib.loads((probe.parents[1] / 'Info.plist').read_bytes())
    identifier = info['CFBundleIdentifier']
    if not identifier.startswith('com.eezouhong.ir-pso-archive-phase2-'):
        raise RuntimeError('unexpected disposable sandbox identity')
    if info['CFBundleExecutable'] != probe.name:
        raise RuntimeError('bundle executable mismatch')
    subprocess.run(['codesign', '--verify', '--strict', str(probe.parents[2])], check=True, capture_output=True)
    signed = subprocess.run(['codesign', '-d', '--entitlements', '-', '--xml', str(probe.parents[2])], check=True, capture_output=True)
    if plistlib.loads(signed.stdout).get('com.apple.security.app-sandbox') is not True:
        raise RuntimeError('sandbox entitlement missing')
    ordinary = Path(subprocess.check_output(['getconf', 'DARWIN_USER_CACHE_DIR'], text=True).strip()).resolve()
    effective = Path(metadata['darwinUserCacheDirectory']).resolve()
    if effective != ordinary / identifier or metadata['uid'] != os.getuid():
        raise RuntimeError('cache namespace mismatch')
    marker = json.loads((effective / 'phase2-owner.json').read_text())
    if marker['ownerId'] != info['MeloNXPhase2CacheOwnerId'] or marker['ownerId'] != metadata['ownerId']:
        raise RuntimeError('cache owner mismatch')
    targets = [effective / identifier / name for name in ('com.apple.metal', 'com.apple.gpuarchiver')]
    for path in (effective, targets[0].parent, *targets):
        if path.is_symlink() or path.resolve() != path:
            raise RuntimeError('symlink in disposable cache path')
    return identifier, effective, targets


def clear(target, output, label):
    entries = []
    if target.exists():
        for p in [target, *target.rglob('*')]:
            if p.is_symlink() or p.lstat().st_uid != os.getuid():
                raise RuntimeError('unsafe disposable cache ownership')
            if p.is_file():
                entries.append({'path': str(p), 'bytes': p.stat().st_size})
    with (output / 'cache-clearances.jsonl').open('a') as f:
        f.write(json.dumps({'label': label, 'target': str(target), 'files': entries}) + '\n')
        f.flush()
        os.fsync(f.fileno())
    if target.exists():
        shutil.rmtree(target)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--owner', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--counts', type=int, nargs='+', default=[200, 1000, 2000])
    p.add_argument('--stage', choices=['smoke', 'timing', 'concurrency'], required=True)
    a = p.parse_args()
    if not os.environ.get('MELONX_DEBUG_GRAPHICS_LEASE'):
        raise RuntimeError('canonical graphics queue required')
    meta = json.loads(a.owner.read_text())
    identifier, effective, targets = ownership(a.probe, meta)
    a.output.mkdir(parents=True, exist_ok=False)
    home = Path(meta['homeDirectory']).resolve()
    if home not in a.output.resolve().parents:
        raise RuntimeError('sandbox output must be inside dedicated container')
    inputs = a.probe.parents[1] / 'Resources/inputs.json'
    rows = json.loads(inputs.read_text())
    hashes = {str(a.probe): sha(a.probe), str(inputs): sha(inputs), str(Path(__file__)): sha(__file__)}
    for row in rows:
        for role in ('vertex', 'fragment'):
            if row.get(role):
                actual = sha(row[role])
                if actual != row[role + 'SHA256']:
                    raise RuntimeError('immutable stage identity mismatch')
                hashes[row[role]] = actual
    (a.output / 'identity.json').write_text(json.dumps({'hashes': hashes, 'systemCache': 'dedicated signed sandbox Metal and GPUArchiver disk caches cleared; driver-memory coldness unknown',
        'cohort': json.loads((inputs.parent / 'cohort.json').read_text()), 'nativeAndPluginLoaded': False,
        'framework': 'Apple system Metal', 'profile': 'offline fixture; not a Debug Tool effective game profile'}, indent=2) + '\n')
    results = []

    def run(label, mode, count, archive, clean=True):
        if clean:
            for target in targets:
                clear(target, a.output, label)
        config = {'inputs': str(inputs), 'mode': mode, 'count': count, 'archive': str(archive),
                  'secondArchive': str(a.output / (label + '-second.metallib'))}
        cfg = a.output / (label + '-config.json')
        cfg.write_text(json.dumps(config) + '\n')
        result_path = a.output / (label + '.json')
        env = dict(os.environ, MTL_ENABLE_DEBUG_LAYER='0', MTL_DEBUG_LAYER='0', MTL_SHADER_VALIDATION='0')
        with (a.output / (label + '.stdout')).open('x') as out, (a.output / (label + '.stderr')).open('x') as err:
            process = subprocess.run([str(a.probe), str(cfg), str(result_path)], stdout=out, stderr=err, env=env, timeout=190)
        status = {'label': label, 'mode': mode, 'count': count, 'exitCode': process.returncode}
        results.append(status)
        (a.output / 'batch.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(status), flush=True)
        if process.returncode:
            raise RuntimeError('probe failed; preserve raw result before changing experiment')
        observed = json.loads(result_path.read_text())
        if observed['bundleIdentifier'] != identifier or Path(observed['darwinUserCacheDirectory']).resolve() != effective:
            raise RuntimeError('actual cache namespace mismatch')
        artifacts = {'cacheFiles': {str(target): [
            {'relativePath': str(f.relative_to(target)), 'bytes': f.stat().st_size}
            for f in target.rglob('*') if f.is_file()] for target in targets}}
        if mode in ('capture', 'lookup', 'miss'):
            if not archive.is_file():
                raise RuntimeError('archive disappeared; invalid publication fixture')
            artifacts['archiveSHA256'] = sha(archive)
            artifacts['archiveBytes'] = archive.stat().st_size
        (a.output / (label + '-artifacts.json')).write_text(json.dumps(artifacts, indent=2) + '\n')
        return observed

    for n in a.counts:
        if a.stage == 'smoke':
            archive = a.output / f'{n}-archive.metallib'
            run(f'{n}-capture', 'capture', n, archive)
            run(f'{n}-lookup-clear', 'lookup', n, archive)
            run(f'{n}-miss', 'miss', n, archive)
            corrupt = a.output / 'corrupt.metallib'
            corrupt.write_bytes(b'deliberately invalid disposable archive')
            run(f'{n}-corrupt', 'corrupt', n, corrupt)
        elif a.stage == 'timing':
            for pair in (1, 2):
                archive = a.output / f'{n}-{pair}-archive.metallib'
                run(f'{n}-{pair}-baseline-clear', 'baseline', n, archive)
                run(f'{n}-{pair}-capture-clear', 'capture', n, archive)
                run(f'{n}-{pair}-baseline-noarchive-clear', 'baseline', n, archive)
                run(f'{n}-{pair}-lookup-clear', 'lookup', n, archive)
                run(f'{n}-{pair}-baseline-retain', 'baseline', n, archive, clean=False)
                run(f'{n}-{pair}-lookup-retain', 'lookup', n, archive, clean=False)
        else:
            for order, mode in enumerate(('concurrent-control', 'concurrent-flush', 'concurrent-control', 'concurrent-flush'), 1):
                run(f'{n}-{order}-{mode}', mode, n, a.output / f'{n}-{order}-archive.metallib')
    for path, frozen in hashes.items():
        if sha(path) != frozen:
            raise RuntimeError('input/binary changed during batch')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
