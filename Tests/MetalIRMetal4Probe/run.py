"""Run a bounded sequential Phase 0 batch inside the canonical graphics queue."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


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


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--inputs', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--variants', type=int, default=1, choices=range(1, 17))
    p.add_argument('--cache-observation-root', type=Path)
    args = p.parse_args()
    if not os.environ.get('MELONX_DEBUG_GRAPHICS_LEASE'):
        raise RuntimeError('canonical graphics queue lease required')
    args.output.mkdir(parents=True, exist_ok=False)
    rows = json.loads(args.inputs.read_text())
    identity = {'probeSHA256': sha(args.probe), 'inputManifestSHA256': sha(args.inputs),
                'inputs': [], 'systemCacheState': 'retained; cold status unknown',
                'scope': 'offline compatibility diagnostic; not Debug Tool game acceptance'}
    for row in rows:
        for role in ('vertex', 'fragment', 'compute'):
            if role in row:
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
        with (args.output / f'{label}.stdout').open('x') as out, (args.output / f'{label}.stderr').open('x') as err:
            command = [str(args.probe), mode, str(args.inputs), str(args.output / f'{label}.json'), str(args.variants)]
            try:
                run = subprocess.run(command, stdout=out, stderr=err, env=env, timeout=190)
                code = run.returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        summary.append({'label': label, 'exitCode': code})
        print(json.dumps(summary[-1]), flush=True)
    after = caches(args.cache_observation_root)
    changed = {key: value for key, value in after.items() if before.get(key) != value}
    # No deletion: changed cache files may still belong to shared system state.
    (args.output / 'cache-observation.local.json').write_text(json.dumps({
        'before': before, 'after': after, 'changed': changed,
        'deleted': [], 'ownership': 'not established solely by file changes'}, indent=2) + '\n')
    if sha(args.probe) != identity['probeSHA256'] or sha(args.inputs) != identity['inputManifestSHA256']:
        raise RuntimeError('binary or input changed during batch')
    for row in rows:
        for role in ('vertex', 'fragment', 'compute'):
            if role in row and sha(row[role]) != row[role + 'SHA256']:
                raise RuntimeError('stage changed during batch')
    (args.output / 'batch.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'output': str(args.output), 'changedSystemCacheFiles': len(changed),
                      'cacheFilesDeleted': 0}), flush=True)
    return 0 if all(row['exitCode'] == 0 for row in summary) else 1


if __name__ == '__main__':
    raise SystemExit(main())
