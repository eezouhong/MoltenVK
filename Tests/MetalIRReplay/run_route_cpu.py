#!/usr/bin/env python3
"""Exercise production route selection with CPU-only mocked external probes.

No Metal framework/device, Vulkan instance, compiler plugin or emulator is used.
This is not dynamic acceptance of the complete native dylib.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

STUB = r'''#pragma once
#import <Foundation/Foundation.h>
typedef NSUInteger MTLGPUFamily;
typedef NSUInteger MTLArgumentBuffersTier;
static constexpr MTLGPUFamily MTLGPUFamilyMetal3 = 7;
static constexpr MTLArgumentBuffersTier MTLArgumentBuffersTier2 = 2;
@protocol MTLDevice <NSObject>
- (BOOL)supportsFamily:(MTLGPUFamily)family;
@property(readonly) MTLArgumentBuffersTier argumentBuffersSupport;
@end
id<MTLDevice> MTLCreateSystemDefaultDevice();
bool mvkMetalIRCompilerAvailable();
'''
PROBE = r'''#include "MVKShaderRouteConfig.h"
#include "MVKMetalIR.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
static int compilerProbes=0, hardwareProbes=0;
static bool plugin=false;
static int hardware=0;
@interface FakeDevice : NSObject <MTLDevice>
@end
@implementation FakeDevice
- (BOOL)supportsFamily:(MTLGPUFamily)family { return hardware != 2; }
- (MTLArgumentBuffersTier)argumentBuffersSupport { return hardware == 3 ? 1 : MTLArgumentBuffersTier2; }
@end
id<MTLDevice> MTLCreateSystemDefaultDevice() {
    ++hardwareProbes;
    return hardware == 0 ? nil : [[FakeDevice alloc] init];
}
bool mvkMetalIRCompilerAvailable() { ++compilerProbes; return plugin; }
int main(int argc,char**argv) {
    if(argc!=4) return 64;
    @autoreleasepool {
        setenv("MELONX_EXPERIMENTAL_METAL_IR",argv[1],1);
        setenv("MELONX_METAL_IR_TELEMETRY","1",1);
        plugin=strcmp(argv[2],"0")!=0; hardware=atoi(argv[3]);
        auto before=mvkShaderRouteConfig();
        int compilerBefore=compilerProbes, hardwareBefore=hardwareProbes;
        setenv("MELONX_EXPERIMENTAL_METAL_IR",before.requested ? "0" : "1",1);
        plugin=!plugin;hardware=hardware==1 ? 0 : 1;
        auto after=mvkShaderRouteConfig();
        bool immutable=before.requested==after.requested && before.available==after.available &&
            before.selected==after.selected && before.failureReason==after.failureReason &&
            compilerBefore==compilerProbes && hardwareBefore==hardwareProbes;
        std::cout << "{\"requested\":" << before.requested << ",\"available\":" << unsigned(before.available)
          << ",\"selected\":" << unsigned(before.selected) << ",\"reason\":" << before.failureReason
          << ",\"usesIRPath\":" << before.usesIRPath() << ",\"compilerProbes\":" << compilerProbes
          << ",\"hardwareProbes\":" << hardwareProbes << ",\"immutable\":" << immutable
          << ",\"telemetry\":" << mvkShaderInitialTelemetryEnabled() << "}\n";
        return immutable ? 0 : 1;
    }
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    source=Path(__file__).resolve().parents[2]/'MoltenVK/MoltenVK/GPUObjects'
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    identity={name:hashlib.sha256((source/name).read_bytes()).hexdigest()
              for name in ('MVKShaderRouteConfig.mm','MVKShaderRouteConfig.h')}
    results=[]
    # hardware: 0 absent, 1 supported, 2 wrong family, 3 wrong argument tier.
    cases=[('msl',0,1,1),('ir',1,1,1),('missing-or-wrong-abi',1,0,1),
           ('absent-device',1,1,0),('wrong-family',1,1,2),('wrong-tier',1,1,3)]
    with tempfile.TemporaryDirectory(prefix='melonx-route-cpu-') as temp:
        directory=Path(temp)
        for name in identity:
            (directory/name).write_bytes((source/name).read_bytes())
        (directory/'MVKMetalIR.h').write_text(STUB)
        (directory/'probe.mm').write_text(PROBE)
        exe=directory/'route'
        command=['clang++','-std=c++17','-O2','-fno-objc-arc','-framework','Foundation',
                 str(directory/'MVKShaderRouteConfig.mm'),str(directory/'probe.mm'),'-o',str(exe)]
        built=subprocess.run(command,capture_output=True,text=True)
        (output/'build.log').write_text(built.stdout+built.stderr)
        built.check_returncode()
        linked=subprocess.check_output(['otool','-L',str(exe)],text=True)
        assert '/Metal.framework/' not in linked and 'MoltenVK' not in linked
        for name,request,plugin,hardware in cases:
            run=subprocess.run([str(exe),str(request),str(plugin),str(hardware)],capture_output=True,text=True,timeout=10)
            observed=json.loads(run.stdout)
            if not request:
                expected=dict(requested=0,available=2,selected=0,reason=0,usesIRPath=0,
                              compilerProbes=0,hardwareProbes=0,immutable=1,telemetry=1)
            else:
                ok=plugin and hardware==1
                expected=dict(requested=1,available=1 if ok else 0,selected=1 if ok else 2,
                              reason=0 if ok else (2 if plugin else 1),usesIRPath=1,
                              compilerProbes=1,hardwareProbes=1 if plugin else 0,immutable=1,telemetry=1)
            passed=run.returncode==0 and observed==expected
            result=dict(case=name,passed=passed,observed=observed)
            results.append(result)
            (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
            print(json.dumps(result),flush=True)
            if not passed:raise RuntimeError('CPU route fixture failed: '+name)
    assert all(hashlib.sha256((source/name).read_bytes()).hexdigest()==digest for name,digest in identity.items())
    (output/'identity.json').write_text(json.dumps({'sourceSHA256':identity,
        'scope':'byte-identical production route source; Foundation-only fake device/plugin probes; not full dylib loading acceptance'},indent=2)+'\n')
    print(json.dumps({'passed':len(results),'scope':'CPU-only mocked external probes; no game or Metal device'}))

if __name__=='__main__':main()
