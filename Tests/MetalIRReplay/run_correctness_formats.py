"""Attachment CPU oracles. Invoke through the canonical graphics queue."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from run import sha


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('build','native','plugin','output'):
        p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args()
    build,native,plugin=(v.resolve(strict=True) for v in (args.build,args.native,args.plugin))
    source=Path(__file__).resolve().parent
    names=('correctness_formats.vert.spv','correctness_formats.frag.spv','correctness_formats_uint.frag.spv')
    files=[native,plugin,*plugin.parent.glob('*.dylib'),build/'correctness_formats_probe',
           *[build/n for n in names],source/'correctness_formats_probe.cpp',source/'vulkan_context.h',
           source/'correctness_formats.cmake',source/'CMakeLists.txt',source/'run.py',Path(__file__),
           *[source/n.removesuffix('.spv') for n in names]]
    identity={str(f):sha(f) for f in files}
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    (out/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    base={k:v for k,v in os.environ.items() if not k.startswith(('MELONX_','MVK_CONFIG_','DYLD_','MTL_'))}
    base.update(MELONX_METAL_IR_PLUGIN=str(plugin),DYLD_LIBRARY_PATH=str(native.parent),DYLD_PRINT_LIBRARIES='1',
                MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',MVK_CONFIG_METAL4_COMPILER='1',
                MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='0',MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS='0',
                MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS='1',MVK_CONFIG_LOG_LEVEL='4',
                MTL_ENABLE_DEBUG_LAYER='1',MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1')
    results=[]
    for kind in ('unorm','unorm-mask','unorm-no-write','unorm-blend','srgb','srgb-blend','uint','uint-mask'):
        for route in ('msl','ir'):
            label=route+'-'+kind
            env=dict(base,MELONX_EXPERIMENTAL_METAL_IR='1' if route=='ir' else '0')
            process=subprocess.run([str(build/'correctness_formats_probe'),kind,*[str(build/n) for n in names]],
                                   env=env,capture_output=True,text=True,timeout=65)
            text=process.stdout+process.stderr;(out/(label+'.log')).write_text(text)
            rows=[json.loads(s) for s in process.stdout.splitlines() if s.startswith('{')]
            loaded=re.findall(r'^dyld\[\d+\]: <[^>]+> (.+/libMoltenVK[^/]*\.dylib)$',text,re.M)
            native_ok=len(loaded)==1 and Path(loaded[0]).resolve()==native
            selected='Shader compiler selected: '+('Metal IR' if route=='ir' else 'MSL') in text
            validation='Metal GPU Validation Enabled' in text and 'Metal API Validation Enabled' in text
            plugin_ok=route=='msl' or (str(plugin) in text and text.count('MetalIR compiled stage')>=2)
            valid=(process.returncode==0 and len(rows)==1 and rows[0].get('success') is True
                   and native_ok and selected and validation and plugin_ok
                   and '[mvk-error]' not in text and 'MetalIR shader rejected:' not in text)
            result={'case':label,'passed':valid,'exitCode':process.returncode,
                    'nativeVerified':native_ok,'routeVerified':selected,'validationVerified':validation,
                    'pluginVerified':plugin_ok,'oracle':rows}
            results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
            print(json.dumps(result),flush=True)
    unchanged=all(sha(Path(f))==digest for f,digest in identity.items())
    summary={'allPassed':unchanged and all(x['passed'] for x in results),'passed':sum(x['passed'] for x in results),
             'cases':len(results),'inputsUnchanged':unchanged,
             'scope':'Independent 256-pixel CPU oracle per case: UNORM/UINT/sRGB, alpha blending, RB and zero masks, readback guards; not full format/sample conformance.'}
    (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary),flush=True)
    return 0 if summary['allPassed'] else 1


if __name__=='__main__':
    raise SystemExit(main())
