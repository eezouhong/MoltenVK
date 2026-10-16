"""Run mixed NoContraction vector arithmetic on both routes and all math policies via the shared queue."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','native','plugin','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    if 'MELONX_DEBUG_GRAPHICS_LEASE' not in os.environ:
        parser.error('launch through the canonical Debug Tool queue')
    build,native,plugin=(p.resolve(strict=True) for p in (args.build,args.native,args.plugin))
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    files=[native,plugin,*plugin.parent.glob('*.dylib'),build/'nocontraction_vector_probe',build/'nocontraction_vector.spv',
           Path(__file__),*[Path(__file__).with_name(n) for n in ('nocontraction_vector_probe.cpp','nocontraction_vector.spvasm','vulkan_context.h')]]
    identity={str(p):sha(p) for p in files}
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    env={k:v for k,v in os.environ.items() if not k.startswith(('MELONX_','MVK_CONFIG_','DYLD_','MTL_','DXIL_'))}
    env.update(DYLD_LIBRARY_PATH=str(native.parent),DYLD_PRINT_LIBRARIES='1',
               MELONX_METAL_IR_PLUGIN=str(plugin),MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',
               MVK_CONFIG_LOG_LEVEL='4',MTL_SHADER_VALIDATION='1')
    results=[]
    for expected,preference in ((0,1),(1,0),(2,2)):
        for route in ('MSL','IR'):
            label=f'{route}-{expected}'
            # No subprocess timeout/kill for queued GPU work. A forced stop must
            # use queue cancel --stop so the supervisor owns process cleanup.
            process=subprocess.run([str(build/'nocontraction_vector_probe'),str(build/'nocontraction_vector.spv')],
                env=dict(env,MELONX_EXPERIMENTAL_METAL_IR=str(int(route=='IR')),
                         MVK_CONFIG_FAST_MATH_ENABLED=str(preference)),capture_output=True,text=True)
            text=process.stdout+process.stderr;(out/(label+'.log')).write_text(text)
            oracle=[json.loads(line) for line in process.stdout.splitlines() if line.startswith('{')]
            modes=re.findall(r'MetalIR compiled stage 5:.*?math mode ([012])',text)
            loaded=re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$',text,re.M)
            verified=len(loaded)==1 and Path(loaded[0]).resolve()==native
            rejected=any('MetalIR' in line and any(w in line for w in ('rejected','unavailable','failed','retrying','using original')) for line in text.splitlines())
            valid=verified and 'Metal GPU Validation Enabled' in text and len(oracle)==1 and not rejected
            if route=='IR': valid=valid and modes==[str(expected)] and str(plugin) in text
            else: valid=valid and not modes
            row=dict(route=route,mode=expected,exitCode=process.returncode,executionValid=bool(valid),
                     oracle=oracle,passed=bool(valid and process.returncode==0 and oracle[0].get('success')))
            results.append(row);print(json.dumps(row),flush=True)
            (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    unchanged=all(sha(Path(p))==h for p,h in identity.items())
    summary=dict(cases=len(results),passed=sum(r['passed'] for r in results),
                 valid=sum(r['executionValid'] for r in results),identityUnchanged=unchanged)
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
    if not unchanged or not all(r['executionValid'] for r in results): raise SystemExit(2)
    if not all(r['passed'] for r in results): raise SystemExit(1)


if __name__=='__main__': main()
