"""Independent vertex Layer/ViewportIndex pixel oracles; shared queue required."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from run import sha

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','native','plugin','output'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--baseline-plugin',type=Path)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    native=a.native.resolve(strict=True);plugin=a.plugin.resolve(strict=True);build=a.build.resolve(strict=True)
    files=[native,plugin,build/'layer_probe',build/'layer.vert.spv',build/'viewport.vert.spv',build/'layer.frag.spv']
    if a.baseline_plugin:files.append(a.baseline_plugin.resolve(strict=True))
    identities={str(x):sha(x) for x in files};(out/'identity.json').write_text(json.dumps(identities,indent=2)+'\n')
    env={k:v for k,v in os.environ.items() if not k.startswith(('MELONX_','MVK_CONFIG_','DYLD_','MTL_'))}
    env.update(DYLD_LIBRARY_PATH=str(native.parent),MTL_SHADER_VALIDATION='1',MVK_CONFIG_METAL4_COMPILER='1',MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1',MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0',MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',MVK_CONFIG_LOG_LEVEL='4')
    variants=[('MSL',plugin,True),('IR',plugin,True)]
    if a.baseline_plugin:variants.append(('IR-baseline',a.baseline_plugin.resolve(strict=True),False))
    results=[]
    for label,compiler,expect in variants:
        for mode in ('layer','viewport'):
            current=dict(env,MELONX_EXPERIMENTAL_METAL_IR='0' if label=='MSL' else '1',MELONX_METAL_IR_PLUGIN=str(compiler))
            proc=subprocess.run([str(build/'layer_probe'),mode,str(build/(mode+'.vert.spv')),str(build/'layer.frag.spv')],env=current,capture_output=True,text=True,timeout=70)
            log=proc.stdout+proc.stderr;(out/(label+'-'+mode+'.log')).write_text(log)
            oracle=[json.loads(s) for s in proc.stdout.splitlines() if s.startswith('{')]
            valid=proc.returncode==0 and len(oracle)==1 and oracle[0]['success'] and '[mvk-error]' not in log
            if not expect:valid=proc.returncode==1 and 'VK_ERROR_FEATURE_NOT_PRESENT' in log and 'MetalIR stage 0 rejected' in log
            valid=bool(valid and 'Metal GPU Validation Enabled' in log)
            results.append(dict(route=label,mode=mode,valid=valid,expectedSuccess=expect,exitCode=proc.returncode,oracle=oracle))
            (out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results[-1]),flush=True)
            if not valid:raise RuntimeError('Layer oracle failed: '+label+'-'+mode)
    assert all(sha(Path(x))==digest for x,digest in identities.items()),'Inputs changed'
if __name__=='__main__':main()
