"""Run a bounded sequential Phase 0 batch inside the canonical graphics queue."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import plistlib
import shutil


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def caches(extra_root=None):
    root = Path(subprocess.check_output(['getconf', 'DARWIN_USER_CACHE_DIR'], text=True).strip())
    entries = {}
    directories=list(root.glob('com.apple.metal*'))
    if extra_root and extra_root.exists():
        directories.extend(extra_root.glob('com.apple.metal*'))
    for directory in directories:
        if not directory.is_dir() or directory.is_symlink():
            continue
        for path in directory.rglob('*'):
            if path.is_file() and not path.is_symlink():
                s = path.stat()
                entries[str(path)] = [s.st_size, s.st_mtime_ns]
    return entries


def owned_cache(probe, metadata_path):
    metadata=json.loads(metadata_path.read_text())
    identifier=metadata['bundleIdentifier']
    if not identifier.startswith('com.eezouhong.ir-metal4-phase01-'):
        raise RuntimeError('not a dedicated task sandbox identifier')
    bundle=probe.resolve().parents[2]
    info=plistlib.loads((bundle/'Contents/Info.plist').read_bytes())
    if info['CFBundleIdentifier'] != identifier or info['CFBundleExecutable'] != probe.name:
        raise RuntimeError('probe bundle identity mismatch')
    subprocess.run(['codesign','--verify','--strict',str(bundle)],check=True,capture_output=True)
    signing=subprocess.run(['codesign','-d','--entitlements','-','--xml',str(bundle)],check=True,capture_output=True)
    entitlements=plistlib.loads(signing.stdout)
    if entitlements.get('com.apple.security.app-sandbox') is not True:
        raise RuntimeError('probe is not sandboxed')
    ordinary=Path(subprocess.check_output(['getconf','DARWIN_USER_CACHE_DIR'],text=True).strip()).resolve()
    effective=Path(metadata['darwinUserCacheDirectory']).resolve()
    if effective != ordinary/identifier or metadata['uid'] != os.getuid():
        raise RuntimeError('owned cache namespace mismatch')
    marker=json.loads((effective/'phase01-owner.json').read_text())
    owner=metadata['ownerId']
    if info.get('MeloNXPhase01CacheOwnerId') != owner or marker.get('ownerId') != owner:
        raise RuntimeError('dedicated cache ownership marker mismatch')
    # Actual Metal files were observed one bundle-ID level below confstr's root.
    target=effective/identifier/'com.apple.metal'
    if target.resolve() != target or any(p.is_symlink() for p in (effective,target.parent,target)):
        raise RuntimeError('owned cache has a symlink')
    return identifier,effective,target


def clear_cache(target, output, label):
    entries=[]
    if target.exists():
        for path in [target,*target.rglob('*')]:
            stat=path.lstat()
            if path.is_symlink() or stat.st_uid != os.getuid():
                raise RuntimeError('cache cleanup ownership check failed')
            if path.is_file():
                entries.append({'path':str(path),'bytes':stat.st_size})
    # Persist the deletion manifest before touching only the disposable cache.
    with (output/'cache-clearances.jsonl').open('a') as stream:
        stream.write(json.dumps({'label':label,'target':str(target),'files':entries})+'\n')
        stream.flush()
        os.fsync(stream.fileno())
    if target.exists():
        shutil.rmtree(target)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--inputs', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--variants', type=int, default=1, choices=range(1, 17))
    p.add_argument('--cache-observation-root', type=Path)
    p.add_argument('--owned-cache-metadata',type=Path)
    p.add_argument('--system-cache',choices=('retain','clear-owned'),default='retain')
    p.add_argument('--captured',action='store_true',help='Use captured_probe instead of synthetic variants')
    args = p.parse_args()
    if not os.environ.get('MELONX_DEBUG_GRAPHICS_LEASE'):
        raise RuntimeError('canonical graphics queue lease required')
    args.output.mkdir(parents=True, exist_ok=False)
    cache_owner=None
    if args.owned_cache_metadata:
        cache_owner=owned_cache(args.probe,args.owned_cache_metadata)
        args.cache_observation_root=cache_owner[2].parent
    if args.system_cache=='clear-owned' and cache_owner is None:
        raise RuntimeError('clearing requires proven owned sandbox cache metadata')
    rows = json.loads(args.inputs.read_text())
    identity = {'probeSHA256': sha(args.probe), 'inputManifestSHA256': sha(args.inputs),
                'runnerSHA256':sha(__file__),
                'inputs': [], 'systemCacheState': 'retained; cold status unknown' if args.system_cache=='retain'
                    else 'owned sandbox Metal disk cache cleared before each subprocess; driver-memory coldness not assumed',
                'scope': 'offline compatibility diagnostic; not Debug Tool game acceptance'}
    for row in rows:
        for role in ('vertex', 'fragment', 'compute'):
            if row.get(role):
                actual = sha(row[role])
                if actual != row[role + 'SHA256']:
                    raise RuntimeError('input identity mismatch')
                identity['inputs'].append({'label': row['label'], 'stage': role, 'sha256': actual})
    before = caches(args.cache_observation_root)
    (args.output / 'identity.json').write_text(json.dumps(identity, indent=2) + '\n')
    summary = []
    env = dict(os.environ, MTL_ENABLE_DEBUG_LAYER='0', MTL_DEBUG_LAYER='0', MTL_SHADER_VALIDATION='0')
    for number, mode in enumerate(('legacy', 'metal4', 'legacy', 'metal4'), 1):
        label = f'{number:02d}-{mode}'
        if args.system_cache=='clear-owned':
            clear_cache(cache_owner[2],args.output,label)
        with (args.output / f'{label}.stdout').open('x') as out, (args.output / f'{label}.stderr').open('x') as err:
            command = [str(args.probe), mode, str(args.inputs), str(args.output / f'{label}.json')]
            if not args.captured:command.append(str(args.variants))
            try:
                run = subprocess.run(command, stdout=out, stderr=err, env=env, timeout=190)
                code = run.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        summary.append({'label': label, 'exitCode': code})
        print(json.dumps(summary[-1]), flush=True)
        if cache_owner and code==0:
            effective=json.loads((args.output/f'{label}.json').read_text())
            if effective.get('bundleIdentifier') != cache_owner[0] or Path(effective.get('darwinUserCacheDirectory','')).resolve() != cache_owner[1]:
                raise RuntimeError('actual probe cache namespace differs from ownership proof')
    after = caches(args.cache_observation_root)
    changed = {key: value for key, value in after.items() if before.get(key) != value}
    # No deletion: changed cache files may still belong to shared system state.
    (args.output / 'cache-observation.local.json').write_text(json.dumps({
        'before': before, 'after': after, 'changed': changed,
        'deletedManifest':'cache-clearances.jsonl' if args.system_cache=='clear-owned' else None,
        'ownership': 'dedicated signed sandbox namespace' if cache_owner else 'not established solely by file changes'}, indent=2) + '\n')
    if sha(args.probe) != identity['probeSHA256'] or sha(args.inputs) != identity['inputManifestSHA256']:
        raise RuntimeError('binary or input changed during batch')
    if sha(__file__) != identity['runnerSHA256']:
        raise RuntimeError('runner changed during batch')
    for row in rows:
        for role in ('vertex', 'fragment', 'compute'):
            if row.get(role) and sha(row[role]) != row[role + 'SHA256']:
                raise RuntimeError('stage changed during batch')
    (args.output / 'batch.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'output': str(args.output), 'changedSystemCacheFiles': len(changed),
                      'cacheCleanup':args.system_cache}), flush=True)
    return 0 if all(row['exitCode'] == 0 for row in summary) else 1


if __name__ == '__main__':
    raise SystemExit(main())
