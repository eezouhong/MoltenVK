"""Static IR ABI-9 / SPIRV-Cross source compilation of a validated private corpus.

First validate the guard and a representative --limit 3 run through the canonical
graphics queue. MSC 3.1.1 loads Metal providers; use --queued --workers 1 for it.
Without --queued every child requires the Metal-provider blocking guard.
This is not Metal pipeline compilation, execution, or a captured-layout replay.
"""
import argparse
from collections import Counter, deque
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('manifest', 'helper', 'guard', 'plugin', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--limit', type=int, default=0)
    parser.add_argument('--workers', type=int, choices=(1, 2), default=2)
    parser.add_argument('--queued', action='store_true', help='Allow provider loading under the canonical graphics lease')
    parser.add_argument('--case-timeout', type=int, default=60)
    args = parser.parse_args()
    if args.queued:
        if 'MELONX_DEBUG_GRAPHICS_LEASE' not in os.environ:
            raise RuntimeError('--queued requires the inherited canonical graphics lease')
        if args.workers != 1:
            raise ValueError('queued compiler work must be sequential: --workers 1')
    manifest, helper, guard, plugin = (p.resolve(strict=True) for p in
                                      (args.manifest, args.helper, args.guard, args.plugin))
    data = json.loads(manifest.read_text())
    if not data.get('complete') or data.get('errors') or data.get('validationFailures'):
        raise ValueError('require a complete, validated corpus')
    modules = data['modules']
    if args.limit:
        # Start with one entry from each represented stage before filling the limit.
        selected, stages = [], set()
        for m in modules:
            if m['stage'] not in stages:
                selected.append(m); stages.add(m['stage'])
        modules = (selected + [m for m in modules if m not in selected])[:args.limit]
    for m in modules:
        source = (manifest.parent / m['file']).resolve(strict=True)
        if source.parent != manifest.parent or m['validationExit'] != 0:
            raise ValueError('invalid manifest module')
        if source.stat().st_size != m['bytes'] or digest(source) != m['sha256']:
            raise ValueError('input identity changed')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / 'cases').mkdir()
    sources = [Path(__file__).resolve(), *[Path(__file__).with_name(n).resolve()
               for n in ('compile_module.cpp', 'no_metal_guard.cpp')]]
    paths = [manifest, helper, guard, plugin, *plugin.parent.glob('*.dylib'), *sources]
    identity = {str(p): digest(p) for p in paths}
    save(out / 'identity.json', identity)
    save(out / 'configuration.json', {
        'moduleCount': len(modules), 'cases': len(modules) * 6,
        'modes': {'0': 'Fast', '1': 'Safe', '2': 'Relaxed'},
        'ir': 'Direct ABI-9, reflected minimal layout, vertex invariance on, transforms zero; point-raster flag for active fragment PointCoord',
        'msl': 'SPIRV-Cross source only; requested math mode is not applied without Metal compilation',
        'fallback': False, 'workers': args.workers,
        'caseTimeoutSeconds': None if args.queued else args.case_timeout,
        'queued': args.queued, 'metalDeviceCreation': 'not instrumented; queued if providers load',
        'maxArtifactBytes': 2 * 1024**3,
        'private': 'Raw inputs, generated sources and binaries must not be committed',
    })
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = {k: v for k, v in os.environ.items() if not k.startswith(
        ('DYLD_', 'MTL_', 'MELONX_', 'MVK_CONFIG_', 'MESA_', 'DXIL_'))}
    env.update(DYLD_INSERT_LIBRARIES=str(guard), DYLD_LIBRARY_PATH=str(plugin.parent))
    control = subprocess.run([str(helper), 'guard-test'], env=env,
                             capture_output=True, timeout=10)
    (out / 'guard-control.log').write_bytes(control.stdout + control.stderr)
    if control.returncode != 86 or b'B4_METAL_PROVIDER_BLOCKED' not in control.stderr:
        raise RuntimeError('Metal guard positive control failed; do not run unguarded')
    if args.queued:
        env['B4_QUEUED_COMPILATION'] = '1'

    def run(case):
        module, route, mode = case
        label = f"{module['sha256']}-{route}-{mode}"
        directory = out / 'cases' / label
        directory.mkdir()
        artifact = directory / ('output.metallib' if route == 'IR' else 'output.metal')
        start = time.monotonic()
        row = dict(module=module['sha256'], stage=module['stage'], route=route, mode=mode,
                   case=label, classification='unclassified')
        with (directory / 'stdout.log').open('wb') as stdout, (directory / 'stderr.log').open('wb') as stderr:
            try:
                child = subprocess.run([str(helper), route, str(mode), str(plugin),
                        str(manifest.parent / module['file']), str(artifact)],
                        env=env, stdout=stdout, stderr=stderr,
                        timeout=None if args.queued else args.case_timeout)
                row['exitCode'] = child.returncode
            except subprocess.TimeoutExpired:
                row.update(exitCode=None, classification='cpu_timeout')
        row['wallSeconds'] = time.monotonic() - start
        lines = (directory / 'stdout.log').read_text(errors='replace').splitlines()
        logs = (directory / 'stderr.log').read_text(errors='replace')
        reports = []
        for line in lines:
            try:
                value = json.loads(line)
                if isinstance(value, dict): reports.append(value)
            except ValueError:
                pass
        row['reports'] = reports
        if row['classification'] != 'cpu_timeout':
            code = row['exitCode']
            if code == 86 or 'B4_METAL_PROVIDER_BLOCKED' in logs:
                row['classification'] = 'metal_guard_blocked'
            elif 'B4_NO_METAL_GUARD_ACTIVE' not in logs:
                row['classification'] = 'guard_missing'
            elif code < 0:
                row['classification'] = 'compiler_signal'
            elif len(reports) != 1:
                row['classification'] = 'invalid_report'
            elif str(reports[0].get('error', '')).startswith('fixture:'):
                row['classification'] = 'fixture_unsupported'
            elif code != 0 or reports[0].get('status') != 0:
                row['classification'] = 'compiler_failure'
            elif (reports[0].get('metalLoaded') is not False and not args.queued) or not artifact.is_file():
                row['classification'] = 'invalid_success'
            else:
                row.update(classification='compiled', artifactSHA256=digest(artifact),
                           artifactBytes=artifact.stat().st_size)
        save(directory / 'result.json', row)
        return row

    cases = [(m, route, mode) for m in modules for mode in range(3) for route in ('IR', 'MSL')]
    results, total_bytes = [], 0
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        # Keep only worker-count cases in flight, including on a guard/budget stop.
        pending = deque()
        iterator = iter(cases)
        for _ in range(args.workers):
            case = next(iterator, None)
            if case is not None: pending.append(executor.submit(run, case))
        with (out / 'results.jsonl').open('w') as stream:
            while pending:
                row = pending.popleft().result()
                results.append(row)
                stream.write(json.dumps(row) + '\n'); stream.flush()
                total_bytes += row.get('artifactBytes', 0)
                if len(results) % 48 == 0:
                    print(json.dumps({'completed': len(results), 'total': len(cases),
                          'counts': dict(Counter(r['classification'] for r in results))}), flush=True)
                if row['classification'] in ('metal_guard_blocked', 'guard_missing'):
                    raise RuntimeError('guard violation: stop and investigate under the queue')
                if total_bytes > 2 * 1024**3 or shutil.disk_usage(out).free < 5 * 1024**3:
                    raise RuntimeError('artifact or free-space budget reached')
                case = next(iterator, None)
                if case is not None: pending.append(executor.submit(run, case))
    failures = [r for r in results if r['classification'] != 'compiled']
    save(out / 'failures.json', failures)
    indexed = {(r['module'], r['route'], r['mode']): r for r in results}
    differences = []
    for module in modules:
        module_hash = module['sha256']
        for route in ('IR', 'MSL'):
            rows = [indexed[module_hash, route, mode] for mode in range(3)]
            keys = [(r['classification'], r.get('artifactSHA256')) for r in rows]
            if len(set(keys)) > 1:
                differences.append({'module': module_hash, 'stage': module['stage'], 'route': route,
                    'kind': 'mode_status_or_artifact_hash_difference',
                    'modes': [{'mode': r['mode'], 'classification': r['classification'],
                               'sha256': r.get('artifactSHA256')} for r in rows]})
        for mode in range(3):
            a, b = (indexed[module_hash, route, mode] for route in ('IR', 'MSL'))
            if a['classification'] != b['classification']:
                differences.append({'module': module_hash, 'stage': module['stage'], 'mode': mode,
                    'kind': 'route_status_difference', 'IR': a['classification'], 'MSL': b['classification']})
    save(out / 'differences.json', differences)
    unchanged = all(digest(Path(p)) == h for p, h in identity.items())
    unchanged &= all(digest(manifest.parent / m['file']) == m['sha256'] for m in modules)
    summary = {'moduleCount': len(modules), 'caseCount': len(results),
        'counts': dict(Counter(r['classification'] for r in results)),
        'byRouteMode': {f'{route}-{mode}': dict(Counter(r['classification'] for r in results
                        if r['route'] == route and r['mode'] == mode))
                        for route in ('IR', 'MSL') for mode in range(3)},
        'differenceCounts': dict(Counter(r['kind'] for r in differences)),
        'artifactsBytes': total_bytes, 'wallSeconds': time.monotonic() - started,
        'inputIdentityUnchanged': bool(unchanged), 'complete': len(results) == len(cases),
        'scope': 'Static compile compatibility with reflected standalone layouts; no dispatch or visual acceptance'}
    save(out / 'summary.json', summary)
    print(json.dumps(summary), flush=True)
    if not unchanged:
        raise RuntimeError('input identity changed during compilation')


if __name__ == '__main__':
    main()
