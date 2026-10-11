"""Process route snapshot matrix; invoke via the shared graphics queue."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','native','plugin','wrong-abi','output'):
        p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    paths=[a.build/'route_probe',a.native,a.plugin,a.wrong_abi]
    identity={str(x.resolve()):hashlib.sha256(x.read_bytes()).hexdigest() for x in paths}
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    env={k:v for k,v in os.environ.items() if not k.startswith(('MELONX_','MVK_CONFIG_','DYLD_','MTL_'))}
    env.update(DYLD_LIBRARY_PATH=str(a.native.resolve().parent),DYLD_PRINT_LIBRARIES='1')
    results=[]
    cases=[('msl',0,a.plugin,2,0,'plain'),('ir',1,a.plugin,1,1,'plain'),
           ('missing',1,out/'missing.dylib',0,2,'plain'),('wrong-abi',1,a.wrong_abi,0,2,'plain'),
           ('swift-setting-over-env',1,a.plugin,1,1,'prewarm')]
    for name,requested,plugin,available,selected,mode in cases:
        runenv=dict(env,MELONX_METAL_IR_PLUGIN=str(plugin.resolve()),MELONX_EXPERIMENTAL_METAL_IR='0')
        process=subprocess.run([str(a.build/'route_probe'),str(requested),mode],env=runenv,capture_output=True,text=True,timeout=60)
        text=process.stdout+process.stderr;(out/(name+'.log')).write_text(text)
        rows=[json.loads(x) for x in process.stdout.splitlines() if x.startswith('{')]
        loaded=re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$',text,re.M)
        verified=len(loaded)==1 and Path(loaded[0]).resolve()==a.native.resolve()
        passed=process.returncode==0 and len(rows)==1 and rows[0]['immutable'] and rows[0]['requested']==requested and rows[0]['available']==available and rows[0]['selected']==selected and verified
        result=dict(case=name,passed=bool(passed),exitCode=process.returncode,binaryVerified=verified,observed=rows)
        results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(result),flush=True)
        if not passed:raise RuntimeError('Route matrix failed: '+name)
    assert all(hashlib.sha256(Path(x).read_bytes()).hexdigest()==h for x,h in identity.items())
    print(json.dumps({'passed':len(results),'scope':'Route freeze before/after physical enumeration; not game/device acceptance'}))

if __name__=='__main__':main()
