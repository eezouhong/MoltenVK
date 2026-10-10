"""Native absolute-ID output and absence of unused draw-runtime uploads.

Run through the shared graphics queue. The GPU values and untouched rows have
an independent CPU oracle; this is correctness coverage, not game acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','native','plugin','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    build,native,plugin=(p.resolve(strict=True) for p in (args.build,args.native,args.plugin))
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    files=[build/'native_ids_probe',build/'native_ids.vert.spv',build/'runtime.frag.spv',native,plugin,*plugin.parent.glob('*.dylib')]
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    identity={str(p):sha(p) for p in files}
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    base={k:v for k,v in os.environ.items() if not k.startswith(('MELONX_','MVK_CONFIG_','DYLD_','MTL_'))}
    base.update(MELONX_METAL_IR_PLUGIN=str(plugin),MELONX_METAL_IR_TELEMETRY='1',
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',MVK_CONFIG_METAL4_COMPILER='1',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',MVK_CONFIG_LOG_LEVEL='4',
                DYLD_LIBRARY_PATH=str(native.parent),DYLD_PRINT_LIBRARIES='1',
                MTL_ENABLE_DEBUG_LAYER='1',MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
    results=[]
    for label,enabled in [('msl',False),('ir',True)]:
        env=dict(base,MELONX_EXPERIMENTAL_METAL_IR=str(int(enabled)))
        with (out/(label+'.stdout.jsonl')).open('x') as stdout,(out/(label+'.stderr.log')).open('x') as stderr:
            process=subprocess.run([str(build/'native_ids_probe'),str(build/'native_ids.vert.spv'),
                                    str(build/'runtime.frag.spv')],env=env,stdout=stdout,stderr=stderr,timeout=90)
        log=(out/(label+'.stderr.log')).read_text()
        rows=[json.loads(line) for line in (out/(label+'.stdout.jsonl')).read_text().splitlines() if line.startswith('{')]
        loaded=re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$',log,re.M)
        flags=[int(x,16) for x in re.findall(r'MetalIR compiled stage 0:.*runtime flags 0x([0-9a-f]+)',log)]
        valid=process.returncode==0 and len(rows)==1 and rows[0].get('success') and rows[0].get('total')==960
        valid &= len(loaded)==1 and Path(loaded[0]).resolve()==native
        valid &= 'Metal API Validation Enabled' in log and 'Metal GPU Validation Enabled' in log
        valid &= '[mvk-error]' not in log and 'MetalIR stage 0 rejected' not in log
        if enabled:
            valid &= bool(flags) and all(flag&0x23==0 for flag in flags) and str(plugin) in log
        result={'case':label,'exitCode':process.returncode,'passed':bool(valid),'vertexRuntimeFlags':flags,'oracle':rows}
        results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(result),flush=True)
        if not valid:raise SystemExit(1)
    assert all(sha(Path(p))==digest for p,digest in identity.items()),'Input changed during replay'


if __name__=='__main__':
    main()
