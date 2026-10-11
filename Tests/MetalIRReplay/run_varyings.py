"""Execute public sparse varying pixel oracles through the shared graphics queue."""
import argparse
import json
import os
from pathlib import Path
import subprocess
from run import sha
from prepare_varyings import VARIANTS


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('build','native','plugin','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--baseline-plugin',type=Path)
    args=parser.parse_args()
    build,native,plugin=(p.resolve(strict=True) for p in (args.build,args.native,args.plugin))
    out=args.output.resolve(); out.mkdir(parents=True,exist_ok=False)
    source=Path(__file__).resolve().parent
    files=[native,plugin,*plugin.parent.glob('*.dylib'),build/'varying_probe',
           source/'varying_probe.cpp',source/'prepare_varyings.py',source/'vulkan_context.h',Path(__file__),
           *[build/('varying-'+name+'.'+stage+'.spv') for name in VARIANTS for stage in ('vert','frag')]]
    if args.baseline_plugin: files.extend(args.baseline_plugin.parent.glob('*.dylib'))
    identity={str(p):sha(p) for p in files}
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    base={k:v for k,v in os.environ.items() if not k.startswith(('MELONX_','MVK_CONFIG_','DYLD_','MTL_'))}
    base.update(DYLD_LIBRARY_PATH=str(native.parent),DYLD_PRINT_LIBRARIES='1',
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',MVK_CONFIG_METAL4_COMPILER='0',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',MVK_CONFIG_LOG_LEVEL='4',
                MTL_ENABLE_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
    results=[]

    def run(name,route,phase,selected):
        label='-'.join((name,route,phase)); cache=out/('cache-'+name)
        env=dict(base,MELONX_EXPERIMENTAL_METAL_IR='1' if route=='ir' else '0',
                 MELONX_METAL_IR_PLUGIN=str(selected),MELONX_METAL_IR_CACHE=str(cache))
        command=[str(build/'varying_probe'),*[str(build/('varying-'+name+'.'+s+'.spv')) for s in ('vert','frag')],name]
        process=subprocess.run(command,env=env,capture_output=True,text=True,timeout=40)
        text=process.stdout+process.stderr;(out/(label+'.log')).write_text(text)
        rows=[json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
        compiled,restored=text.count('MetalIR compiled stage'),text.count('MetalIR restored stage')
        expected_failure=phase=='baseline'
        pixel_ok=process.returncode==0 and len(rows)==1 and rows[0].get('correct')==rows[0].get('total')==256
        valid=(process.returncode==1 and len(rows)==1 and rows[0].get('correct')==0) if expected_failure else pixel_ok
        valid &= '[mvk-error]' not in text
        valid &= 'Shader compiler selected: '+('Metal IR' if route=='ir' else 'MSL') in text
        valid &= 'Metal GPU Validation Enabled' in text and str(native) in text
        if route=='ir':
            valid &= 'automatic MSL fallback disabled' in text
            valid &= (compiled,restored)==((0,2) if phase=='restored' else (2,0))
        result=dict(case=label,passed=bool(valid),exitCode=process.returncode,
                    expectedBaselineMismatch=expected_failure,compiled=compiled,restored=restored,oracle=rows)
        results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(json.dumps(result),flush=True)
        if not valid: raise RuntimeError('Varying oracle failed: '+label)

    if args.baseline_plugin:
        run('sparse','ir','baseline',args.baseline_plugin.resolve(strict=True))
    for name in VARIANTS:
        run(name,'msl','control',plugin)
        run(name,'ir','cold',plugin)
        run(name,'ir','restored',plugin)
    assert all(sha(Path(p))==digest for p,digest in identity.items()),'Input changed during replay'
    summary=dict(allPassed=True,cases=len(results),scope='256 CPU-expected fragment results per case; constant and noperspective gradient interfaces.')
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary),flush=True)


if __name__=='__main__': main()
