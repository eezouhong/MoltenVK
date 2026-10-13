"""Freeze the task-3 prefix; explicit synthetic masks provide a size stress cohort."""
import argparse
import copy
import hashlib
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def key(row):
    return json.dumps({k: row.get(k) for k in (
        'kind', 'vertexSHA256', 'fragmentSHA256', 'vertexEntry', 'fragmentEntry',
        'vertexDescriptor', 'state')}, sort_keys=True, separators=(',', ':'))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--resources', type=Path, required=True)
    a = p.parse_args()
    a.resources.mkdir(parents=True, exist_ok=False)
    source = json.loads(a.source.read_text())
    distinct, seen = [], set()
    for row in source:
        if row['kind'] != 'render':
            raise ValueError('this cohort supports render only')
        for role in ('vertex', 'fragment'):
            if row.get(role) and sha(row[role]) != row[role + 'SHA256']:
                raise ValueError('source stage identity mismatch')
        k = key(row)
        if k not in seen:
            seen.add(k)
            distinct.append(copy.deepcopy(row))
    rows = copy.deepcopy(distinct)
    for row in rows:
        row['cohort'] = 'captured exact descriptor; incomplete source replay'
    for mask in range(16):
        for original in distinct:
            active = [i for i, c in enumerate(original['state']['colors']) if c['pixelFormat']]
            if not active:
                continue
            row = copy.deepcopy(original)
            row['state']['colors'][active[0]]['writeMask'] = mask
            k = key(row)
            if k in seen:
                continue
            seen.add(k)
            row['cohort'] = 'synthetic write-mask extension of captured descriptor'
            rows.append(row)
            if len(rows) >= 2032:
                break
        if len(rows) >= 2032:
            break
    if len(rows) < 2032:
        raise ValueError('insufficient distinct descriptors; never pad with duplicates')
    rows = rows[:2032]
    for i, row in enumerate(rows):
        row['descriptorId'] = hashlib.sha256(key(row).encode()).hexdigest()
        row['label'] = f'archive-{i:04d}'
        for role in ('vertex', 'fragment'):
            if row.get(role):
                dest = a.resources / (row[role + 'SHA256'] + '.metallib')
                if not dest.exists():
                    dest.write_bytes(Path(row[role]).read_bytes())
                row[role] = str(dest.resolve())
    (a.resources / 'inputs.json').write_text(json.dumps(rows, indent=2) + '\n')
    summary = {'sourceManifestSHA256': sha(a.source), 'sourceCaptureComplete': False,
               'sourceRequests': len(source), 'exactDistinctDescriptors': len(distinct),
               'outputDescriptors': len(rows), 'outputManifestSHA256': sha(a.resources / 'inputs.json'),
               'scope': 'first 200 exact; 1000/2000 include synthetic masks; render only; no pixel acceptance'}
    (a.resources / 'cohort.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
