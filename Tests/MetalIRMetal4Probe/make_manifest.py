"""Freeze captured Metal PSO descriptors and library hashes without GPU work."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--inputs',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--summary',type=Path,required=True)
    p.add_argument('--capture-result',type=Path,
                   help='Preserve source replay failures and incomplete coverage')
    a=p.parse_args();rows=[];bases=Counter();missing=[]
    for name,kind,roles in [('render-descriptors.jsonl','render',('vertex','fragment')),
                            ('compute-descriptors.jsonl','compute',('compute',))]:
        source=a.inputs/name
        if not source.exists():continue
        for n,line in enumerate(source.open()):
            row=json.loads(line);row['kind']=kind;row['label']=f'{kind}-{n:04d}'
            for role in roles:
                if not row.get(role):continue
                path=Path(row[role])
                if not path.is_file():missing.append(str(path));continue
                row[role+'SHA256']=hashlib.sha256(path.read_bytes()).hexdigest()
            if kind=='render':bases[row['baseKeyBase64']]+=1
            rows.append(row)
    capture=json.loads(a.capture_result.read_text()) if a.capture_result else None
    capture_complete=bool(capture and capture.get('normalExit') and
                          capture.get('actualNativeLoadedVerified') and
                          capture.get('actualCompilerLoadedVerified') and
                          not any(capture.get('errorMarkers',{}).values()))
    summary={'renderRequests':sum(r['kind']=='render' for r in rows),
             'computeRequests':sum(r['kind']=='compute' for r in rows),
             'actualNativeUniqueBaseKeys':len(bases),'baseRequestHistogram':dict(Counter(bases.values())),
             'missingLibraries':sorted(set(missing)),'nativeKeyMeasured':True,
             'sourceCaptureComplete':capture_complete,
             'sourceCaptureResult':capture,
             'scope':'exact native capture cardinality; replay normalization and rejection coverage still require review'}
    with a.summary.open('x') as out:json.dump(summary,out,indent=2);out.write('\n')
    if missing or not rows:raise RuntimeError('capture cohort incomplete')
    with a.output.open('x') as out:json.dump(rows,out,indent=2);out.write('\n')
    print(json.dumps(summary,indent=2))


if __name__=='__main__':main()
