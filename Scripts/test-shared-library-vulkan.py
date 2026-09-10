#!/usr/bin/env python3
"""Run real Vulkan compute/cache/lifetime validation against an explicit dylib.
Requires Apple Silicon macOS, Vulkan-Headers and glslc. Touches only a temporary
harness directory and the requested evidence output; never clears driver caches.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

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
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    library=a.library.resolve(strict=True)
    env=os.environ.copy()
    settings={'MVK_CONFIG_METAL4_COMPILER':'1','MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES':'1',
              'MVK_CONFIG_METAL4_FLEXIBLE_ASYNC':'1','MVK_CONFIG_METAL4_FLEXIBLE_ASYNC_MAX':'3',
              'MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_REPOSITORY_ENABLED':'1' if a.repository=='on' else '0',
              'MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_RESIDENT_LIMIT':str(a.resident_limit)}
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
        run=subprocess.run([str(executable),*[str(s) for s in shaders]],env=env,capture_output=True,text=True,timeout=60)
    result={'library':str(library),'librarySha256':hashlib.sha256(library.read_bytes()).hexdigest(),
            'settings':settings,'exitCode':run.returncode,'stdout':run.stdout,'stderr':run.stderr,
            'scope':'real Vulkan/Metal parallel compute pipeline creation, cache roundtrip and GPU readback on macOS'}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({**result,'stderr':'\n'.join(run.stderr.splitlines()[-8:])},indent=2))
    return run.returncode

if __name__=='__main__':raise SystemExit(main())
