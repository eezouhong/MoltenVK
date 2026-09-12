#!/usr/bin/env python3
"""Run real Vulkan compute/cache/lifetime validation against an explicit dylib.
Requires Apple Silicon macOS, Vulkan-Headers and glslc. Touches only a temporary
harness directory and the requested evidence output; never clears driver caches.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SHADER = '''#version 450
layout(local_size_x=1) in;
layout(set=0,binding=0,std430) buffer Output { uint data[]; } outData;
layout(push_constant) uniform Push { uint base; } pushData;
layout(constant_id=0) const uint seed=17;
void main(){uint i=gl_GlobalInvocationID.x; outData.data[pushData.base+i]=MODULE_ID*10000u+seed+i;}
'''

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--library',type=Path,required=True)
    p.add_argument('--headers',type=Path,required=True,help='Vulkan-Headers include directory')
    p.add_argument('--glslc',default='glslc')
    p.add_argument('--repository',choices=['on','off'],default='on')
    p.add_argument('--resident-limit',type=int,default=0)
    p.add_argument('--cacheless-reuse',action='store_true')
    p.add_argument('--check-timings',action='store_true')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    library=a.library.resolve(strict=True)
    env=os.environ.copy()
    settings={'MVK_CONFIG_METAL4_COMPILER':'1','MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES':'1',
              'MVK_CONFIG_METAL4_FLEXIBLE_ASYNC':'1','MVK_CONFIG_METAL4_FLEXIBLE_ASYNC_MAX':'3',
              'MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_REPOSITORY_ENABLED':'1' if a.repository=='on' else '0',
              'MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_RESIDENT_LIMIT':str(a.resident_limit)}
    env.update(settings)
    if a.check_timings:
        settings.update({'MVK_CONFIG_PERFORMANCE_TRACKING':'1',
                         'MVK_CONFIG_ACTIVITY_PERFORMANCE_LOGGING_STYLE':'2'})
        env.update(settings)
    with tempfile.TemporaryDirectory(prefix='mvk-native-integration-') as temp:
        root=Path(temp);shader=root/'compute.comp';shader.write_text(SHADER);shaders=[]
        for i in range(4):
            output=root/('module'+str(i)+'.spv')
            subprocess.run([a.glslc,'--target-env=vulkan1.1','-DMODULE_ID='+str(i),str(shader),'-o',str(output)],check=True,timeout=15)
            shaders.append(output)
        executable=root/'native-tests'
        cmd=['clang++','-std=c++17','-O1','-g','-pthread','-I',str(a.headers),str(ROOT/'Scripts/test-shared-library-vulkan.cpp'),str(library),'-Wl,-rpath,'+str(library.parent),'-o',str(executable)]
        build=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
        if build.returncode:raise RuntimeError(build.stdout+build.stderr)
        argv=[str(executable),*[str(s) for s in shaders]]
        if a.cacheless_reuse:
            argv.append('cacheless-repository-'+a.repository)
        started=time.monotonic()
        run=subprocess.run(argv,env=env,capture_output=True,text=True,timeout=60)
        run_ms=(time.monotonic()-started)*1000
    result={'library':str(library),'librarySha256':hashlib.sha256(library.read_bytes()).hexdigest(),
            'cachelessReuse':a.cacheless_reuse,'argv':argv,
            'harnessSha256':hashlib.sha256((ROOT/'Scripts/test-shared-library-vulkan.cpp').read_bytes()).hexdigest(),
            'settings':settings,'exitCode':run.returncode,'stdout':run.stdout,'stderr':run.stderr,
            'scope':'real Vulkan/Metal parallel compute pipeline creation, cache roundtrip and GPU readback on macOS'}
    if a.cacheless_reuse:
        summaries=[l for l in run.stderr.splitlines() if 'Metal 4 unified compiler summary:' in l]
        values=re.findall(r'library_attempts=(\d+)',summaries[-1]) if summaries else []
        actual=int(values[0]) if values else None
        expected=4 if a.repository=='on' else 8
        result['libraryCompiles']={'actual':actual,'expected':expected,'passed':actual==expected}
        if actual!=expected and result['exitCode']==0:
            result['exitCode']=1
    if a.check_timings:
        matches=re.findall(r'Retrieve shader library from the cache.*?max:\s*([\d.]+) ms, count:\s*(\d+)',run.stderr)
        maximum=float(matches[-1][0]) if matches else None
        count=int(matches[-1][1]) if matches else 0
        valid=maximum is not None and count>0 and maximum<=run_ms
        result['cacheLookupTiming']={'maxMs':maximum,'count':count,'processWallMs':run_ms,'passed':valid}
        if not valid and result['exitCode']==0: result['exitCode']=1
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({**result,'stderr':'\n'.join(run.stderr.splitlines()[-8:])},indent=2))
    return result['exitCode']

if __name__=='__main__':raise SystemExit(main())
