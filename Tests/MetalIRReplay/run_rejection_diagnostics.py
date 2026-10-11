"""Public rejection counter and reason fixtures; run through the shared GPU queue."""
import argparse,json,os,subprocess
from pathlib import Path
from run import sha

p=argparse.ArgumentParser(description=__doc__)
for name in ('build','native','plugin','output'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--baseline-native',type=Path)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
files=[a.build/'rejection_probe',a.build/'runtime.vert.spv',a.build/'runtime.frag.spv',a.native,a.plugin,*a.plugin.parent.glob('*.dylib')]
if a.baseline_native:files.append(a.baseline_native)
(out/'identity.json').write_text(json.dumps({str(x.resolve()):sha(x) for x in files},indent=2))
env={k:v for k,v in os.environ.items() if not k.startswith(('MELONX_','MVK_CONFIG_','DYLD_','MTL_'))}
env.update(MELONX_EXPERIMENTAL_METAL_IR='1',MELONX_METAL_IR_PLUGIN=str(a.plugin.resolve()),MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',MVK_CONFIG_METAL4_COMPILER='0',MVK_CONFIG_LOG_LEVEL='4',MTL_ENABLE_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1',DYLD_PRINT_LIBRARIES='1')
results=[]
cases=[('layout-candidate',a.native,'layout-only',0),('discard-candidate',a.native,None,1)]
if a.baseline_native:cases.insert(0,('layout-baseline',a.baseline_native,'layout-only',1))
for label,native,mode,expected in cases:
 command=[str(a.build/'rejection_probe'),str(a.build/'runtime.vert.spv'),str(a.build/'runtime.frag.spv')]
 if mode:command.append(mode)
 z=subprocess.run(command,env=dict(env,DYLD_LIBRARY_PATH=str(native.resolve().parent)),capture_output=True,text=True,timeout=45)
 log=z.stdout+z.stderr;(out/(label+'.log')).write_text(log)
 rows=[json.loads(s) for s in z.stdout.splitlines() if s.startswith('{')]
 valid=z.returncode==0 and len(rows)==1 and rows[0].get('counter')==expected and str(native.resolve()) in log and 'Metal GPU Validation Enabled' in log
 if mode:valid &= rows[0].get('layoutRejected') is True and 'reason=descriptor_layout_capability' in log
 else:valid &= rows[0].get('rejected') is True and 'reason=rasterizer_discard' in log
 result={'case':label,'passed':bool(valid),'exitCode':z.returncode,'expectedCounter':expected,'oracle':rows};results.append(result)
 (out/'results.json').write_text(json.dumps(results,indent=2));print(json.dumps(result),flush=True)
 if not valid:raise RuntimeError('Rejection fixture failed: '+label)
