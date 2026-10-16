"""Run one pinned CTS slice and an explicitly verified math mode via the GPU queue."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import time
import xml.etree.ElementTree as ET


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','native','plugin','control','cases','archive','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--route',choices=('MSL','IR'),required=True)
    parser.add_argument('--mode',type=int,choices=(0,1,2),required=True)
    parser.add_argument('--budget-seconds',type=int,default=240)
    args=parser.parse_args()
    lease=os.environ.get('MELONX_DEBUG_GRAPHICS_LEASE')
    if not lease:parser.error('use the canonical graphics queue')
    paths={n:getattr(args,n).resolve(strict=True) for n in ('binary','native','plugin','control','cases','archive')}
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    identities={str(p):sha(p) for p in [paths[n] for n in ('binary','native','plugin','control','cases')]+list(paths['plugin'].parent.glob('*.dylib'))+[Path(__file__),Path(__file__).with_name('cts_math_control.mm')]}
    (out/'identity.json').write_text(json.dumps(identities,indent=2)+'\n')
    names=[x.strip() for x in paths['cases'].read_text().splitlines() if x.strip()]
    if len(names)>256 or any(not x.startswith('dEQP-VK.') or '*' in x for x in names):
        raise ValueError('require a bounded explicit case list')
    env={k:v for k,v in os.environ.items() if k=='MELONX_DEBUG_GRAPHICS_LEASE' or not k.startswith(('MELONX_','MVK_','DYLD_','MTL_','IR_AUDIT_','VK_'))}
    env.update(DYLD_PRINT_LIBRARIES='1',DYLD_LIBRARY_PATH=str(paths['native'].parent),
               MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',MVK_CONFIG_LOG_LEVEL='4',
               MTL_SHADER_VALIDATION='1',MELONX_EXPERIMENTAL_METAL_IR=str(int(args.route=='IR')),
               MVK_CONFIG_FAST_MATH_ENABLED='2')
    if args.route=='IR':
        env.update(MELONX_METAL_IR_PLUGIN=str(paths['control']),IR_AUDIT_REAL_PLUGIN=str(paths['plugin']),IR_AUDIT_FORCE_MODE=str(args.mode))
    else:
        env.update(DYLD_INSERT_LIBRARIES=str(paths['control']),IR_AUDIT_MSL_MODE=str(args.mode))
    command=[str(paths['binary']),'--deqp-vk-library-path='+str(paths['native']),
             '--deqp-caselist-file='+str(paths['cases']),'--deqp-archive-dir='+str(paths['archive']),
             '--deqp-log-filename='+str(out/'results.qpa'),'--deqp-log-images=disable',
             '--deqp-watchdog=disable','--deqp-base-seed=1']
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    started=time.monotonic()
    with (out/'stdout.log').open('w') as stdout,(out/'stderr.log').open('w') as stderr:
        child=subprocess.Popen(command,cwd=out,env=env,stdout=stdout,stderr=stderr)
        try:
            code=child.wait(timeout=args.budget_seconds)
        except subprocess.TimeoutExpired:
            (out/'budget-stop.json').write_text(json.dumps({'seconds':args.budget_seconds,'queueTicket':json.loads(lease)['ticket']})+'\n')
            # Never signal the GPU child directly; let its canonical supervisor
            # verify process identity and stop/reap the complete owned tree.
            subprocess.run(['python3','/Users/kyle/Documents/MeloNX-debug/hogwarts-sky/tool/tool.py','queue','cancel',str(json.loads(lease)['ticket']),'--stop'],check=True)
            raise RuntimeError('queue stop requested after slice budget')
    text=(out/'stdout.log').read_text(errors='replace')+(out/'stderr.log').read_text(errors='replace')
    qpa=(out/'results.qpa').read_text(errors='replace') if (out/'results.qpa').exists() else ''
    results=[]
    for block in re.findall(r'<TestCaseResult\b.*?</TestCaseResult>',qpa,re.S):
        node=ET.fromstring(block);result=node.find('.//Result')
        results.append({'case':node.get('CasePath'),'status':result.get('StatusCode') if result is not None else 'Incomplete',
                        'details':''.join(result.itertext()).strip() if result is not None else ''})
    observed=re.findall('CTS_'+args.route+'_MODE requested=[0-9]+ effective=([012])',text)
    native=re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$',text,re.M)
    native_verified=len(native)==1 and Path(native[0]).resolve()==paths['native']
    mode_verified=bool(observed) and set(observed)=={str(args.mode)}
    plugin_verified=args.route!='IR' or (str(paths['plugin']) in text and str(paths['control']) in text)
    unchanged=all(sha(Path(p))==h for p,h in identities.items())
    record={'route':args.route,'mode':args.mode,'exitCode':code,'seconds':time.monotonic()-started,
            'requestedCases':len(names),'completedCases':len(results),'nativeVerified':native_verified,
            'modeVerified':mode_verified,'compilerCalls':len(observed),'pluginVerified':plugin_verified,
            'identitiesUnchanged':unchanged,'results':results,'missingCases':sorted(set(names)-{x['case'] for x in results})}
    (out/'summary.json').write_text(json.dumps(record,indent=2)+'\n')
    from collections import Counter
    print(json.dumps({k:v for k,v in record.items() if k not in ('results','missingCases')}|{'counts':dict(Counter(x['status'] for x in results))}),flush=True)
    if not native_verified or not plugin_verified or not unchanged or not results:
        raise SystemExit(2)
    if not mode_verified and any(x['status']=='Pass' for x in results):
        raise SystemExit(2)


if __name__=='__main__':main()
