"""Read existing Perf logs without interpreting missing PSO state as zero reuse."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def analyze(path):
    counts = Counter()
    fields = set()
    malformed = 0
    samples = 0
    totals = Counter()
    pso = None
    last_ir = None
    digest = hashlib.sha256()
    for line in path.open('rb'):
        digest.update(line)
        try:
            record = json.loads(line)
        except (ValueError, UnicodeDecodeError):
            malformed += 1
            continue
        counts[record.get('type', 'untyped')] += 1
        fields.update(record)
        if record.get('type') != 'sample':
            continue
        samples += 1
        for name in ('pipelineCreateMs', 'pipelineCreateCountDelta',
                     'pipelineDrawRequestBlockingJoinCountDelta', 'pipelineLegacyCompilerTaskMs'):
            if name in record:
                totals[name] += record[name]
        ir = record.get('metalIR')
        if isinstance(ir, list) and len(ir) >= 7:
            last_ir = ir
            if ir[0] == 1:
                pso = max(pso or 0, ir[6])
    return {'file': str(path), 'bytes': path.stat().st_size, 'sha256': digest.hexdigest(),
            'recordTypes': dict(counts), 'samples': samples, 'malformed': malformed,
            'sampleTotals': dict(totals), 'irPsoMsCumulativeMax': pso, 'lastMetalIR': last_ir,
            'stateFieldCandidates': sorted(k for k in fields if any(s in k.lower() for s in
                ('functionkey', 'shaderhash', 'basekey', 'vertexdescriptor', 'colormask', 'blendstate'))),
            'baseReuseRatio': None,
            'baseReuseStatus': 'unavailable: logs contain aggregate counters, no complete per-PSO base descriptor'}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('logs', type=Path, nargs='+')
    a = p.parse_args()
    result = [analyze(path) for path in a.logs]
    with a.output.open('x') as out:
        json.dump(result, out, indent=2)
        out.write('\n')
    for row in result:
        print(json.dumps({k: row[k] for k in ('file', 'samples', 'malformed', 'sampleTotals',
                        'irPsoMsCumulativeMax', 'stateFieldCandidates', 'baseReuseRatio')}))


if __name__ == '__main__':
    main()
