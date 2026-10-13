"""Summarize all probe samples; never turn synthetic diagnostics into a Go."""
import argparse
import json
from pathlib import Path


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('batches',type=Path,nargs='+')
    a=p.parse_args();result=[]
    for batch in a.batches:
        for path in sorted(batch.glob('[0-9][0-9]-*.json')):
            x=json.loads(path.read_text());rows=x['rows'];row={'batch':batch.name,'run':path.name,
                'mode':x['mode'],'requestRows':len(rows),'failedRows':sum(not r['success'] for r in rows),
                'baseMs':sum(r.get('baseNs',0) for r in rows)/1e6,
                'specializationMs':sum(r.get('specializationNs',0) for r in rows)/1e6,
                'peakFootprintMiB':max((r['footprintBytes'] for r in rows if r.get('footprintBytes') is not None),default=0)/2**20}
            for repeat,name in [(0,'first'),(1,'repeat')]:
                selected=[r for r in rows if r['repeat']==repeat]
                row[name+'RenderMs']=sum(r['totalNs'] for r in selected if r.get('kind','render')=='render')/1e6
                row[name+'ComputeMs']=sum(r['totalNs'] for r in selected if r.get('kind')=='compute')/1e6
                row[name+'TotalMs']=sum(r['totalNs'] for r in selected)/1e6
            result.append(row)
    with a.output.open('x') as out:json.dump(result,out,indent=2);out.write('\n')
    for row in result:print(json.dumps(row))


if __name__=='__main__':main()
