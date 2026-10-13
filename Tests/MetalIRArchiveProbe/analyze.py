"""Descriptive archive metrics. Does not decide product acceptance or infer locks."""
import argparse
import json
from pathlib import Path


def milliseconds(values):
    values = sorted(v / 1e6 for v in values if isinstance(v, (int, float)))
    if not values:
        return {'count': 0, 'totalMs': None, 'maxMs': None}
    return {'count': len(values), 'totalMs': sum(values), 'maxMs': values[-1],
            'medianMs': values[len(values) // 2]}


def summarize(path):
    x = json.loads(path.read_text())
    rows = x.get('rows', [])
    result = {'file': path.name, 'mode': x['mode'], 'count': x['count'], 'success': x['success'],
              'archiveOpenMs': x['archiveOpenNs'] / 1e6 if 'archiveOpenNs' in x else None,
              'add': milliseconds([r.get('addNs') for r in rows]),
              'pipeline': milliseconds([r.get('pipelineNs') for r in rows]),
              'restoredLibraries': len(x['loads']),
              'libraryCreation': milliseconds([r.get('newLibraryWithDataNs') for r in x['loads']]),
              'footprintAfterLibrariesBytes': x['footprintAfterLibrariesBytes'],
              'footprintAfterBytes': x['footprintAfterBytes']}
    samples = [r['footprintBytes'] for r in x.get('memory', []) if isinstance(r['footprintBytes'], int)]
    result['sampledPeakFootprintBytes'] = max(samples) if samples else None
    if 'flush' in x:
        flush = x['flush']
        result['flushMs'] = flush['durationNs'] / 1e6
        result['archiveBytes'] = flush['archiveBytes']
        result['flushOnMainThread'] = flush['mainThread']
    if 'secondFlush' in x:
        result['secondFlushMs'] = x['secondFlush']['durationNs'] / 1e6
    if 'workerRows' in x:
        workers = x['workerRows']
        result['worker'] = milliseconds([r['pipelineNs'] for r in workers])
        result['workerFailures'] = sum(not r['success'] for r in workers)
        if 'flush' in x:
            overlapping = [r for r in workers if r['startNs'] < x['flush']['endNs'] and r['endNs'] > x['flush']['startNs']]
            result['overlappingWorker'] = milliseconds([r['pipelineNs'] for r in overlapping])
            result['overlapCount'] = len(overlapping)
    if x['mode'] == 'lookup':
        result['strictArchiveHits'] = sum(r['pipelineSuccess'] for r in rows)
    if x['mode'] == 'miss':
        result['missDetected'] = rows[0]['missDetected']
        result['fallbackSuccess'] = rows[0]['fallbackSuccess']
    if x['mode'] == 'corrupt':
        result['archiveRejected'] = not x['archiveOpened']
        result['fallbackSuccess'] = x['fallbackSuccess']
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument('directory', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    rows = []
    for path in sorted(a.directory.rglob('*.json')):
        if path.name.endswith('-config.json') or path.name.endswith('-artifacts.json'):
            continue
        x = json.loads(path.read_text())
        if isinstance(x, dict) and x.get('schemaVersion') == 1 and 'mode' in x and 'loads' in x:
            rows.append(summarize(path))
    a.output.write_text(json.dumps(rows, indent=2) + '\n')
    print(json.dumps({'samples': len(rows), 'output': str(a.output)}))


if __name__ == '__main__':
    main()
