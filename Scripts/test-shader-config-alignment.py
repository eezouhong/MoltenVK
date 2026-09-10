#!/usr/bin/env python3
"""Compile actual configuration/alignment methods and compare with pinned RC6 semantics.

No GPU is required. --captures optionally replays same-build, ABI-checked local
configuration snapshots. Do not publish game-derived snapshots in the repository.
Wall-time comparisons are emitted only in the unsanitized, uncounted build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
SOURCE='MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp'
CONVERTER=ROOT/Path(SOURCE).parent
REFERENCE='3e5dd95c299f680a4ce605f67580e584594c2c22'

def methods(text):
    begin=text.index('using namespace mvk;')
    end=text.index('\n#pragma mark -\n#pragma mark SPIRVToMSLConverter\n',begin)
    return text[begin:end]

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--spirv-cross',type=Path,required=True)
    p.add_argument('--baseline',action='store_true')
    p.add_argument('--count-comparisons',action='store_true')
    p.add_argument('--sanitizer',choices=['address','undefined','thread'])
    p.add_argument('--captures',type=Path)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    old=subprocess.check_output(['git','show',REFERENCE+':'+SOURCE],cwd=ROOT,text=True)
    current=old if a.baseline else (ROOT/SOURCE).read_text()
    prod=methods(current)
    marker='MVK_PUBLIC_SYMBOL void SPIRVToMSLConversionConfiguration::alignWith(const SPIRVToMSLConversionConfiguration& srcContext) {'
    body=old.split(marker,1)[1].split('\n}\n',1)[0]
    reference='static void referenceAlign(mvk::SPIRVToMSLConversionConfiguration& target, const mvk::SPIRVToMSLConversionConfiguration& srcContext) {\n auto& shaderInputs=target.shaderInputs; auto& shaderOutputs=target.shaderOutputs; auto& resourceBindings=target.resourceBindings;\n'+body+'\n}\n'
    counted=a.count_comparisons or a.sanitizer is not None
    if counted:
        needle='MVK_PUBLIC_SYMBOL bool mvk::MSLResourceBinding::matches(const MSLResourceBinding& other) const {'
        assert prod.count(needle)==1
        prod=prod.replace(needle,needle+'\n ++comparisonCount;')
    template=(ROOT/'Scripts/test-shader-config-alignment.cpp').read_text()
    code=template.replace('// @PRODUCTION_METHODS@',prod).replace('// @REFERENCE_ALIGNMENT@',reference)
    captures=sorted(a.captures.glob('pair-*.bin')) if a.captures else []
    if a.captures and not captures:raise ValueError('No captured pairs found')
    with tempfile.TemporaryDirectory(prefix='mvk-align-') as tmp:
        src=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';src.write_text(code)
        command=['clang++','-std=c++17','-O3' if not a.sanitizer else '-O1','-g','-pthread','-DSPIRV_CROSS_NAMESPACE_OVERRIDE=MVK_spirv_cross','-I',str(CONVERTER),'-I',str(a.spirv_cross),str(src),'-o',str(exe)]
        if counted:command+=['-DALIGNMENT_COUNT_COMPARISONS=1']
        if a.sanitizer:command+=['-fsanitize='+a.sanitizer,'-fno-omit-frame-pointer']
        built=subprocess.run(command,capture_output=True,text=True,timeout=45)
        if built.returncode:raise RuntimeError(built.stdout+built.stderr)
        run=subprocess.run([str(exe),*[str(f) for f in captures]],capture_output=True,text=True,timeout=45)
    record={'baseline':a.baseline,'referenceRevision':REFERENCE,'productionSourceSha256':hashlib.sha256(current.encode()).hexdigest(),'referenceBodySha256':hashlib.sha256(body.encode()).hexdigest(),
            'sanitizer':a.sanitizer,'comparisonCounting':counted,'compileCommand':command,'exitCode':run.returncode,'stdout':run.stdout,'stderr':run.stderr,
            'captures':{f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in captures},'scope':'actual C++ config methods; no fake config comparator; algorithm/ABI-local replay, not whole-game FPS'}
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(json.dumps(record,indent=2)+'\n')
    print(run.stdout,run.stderr,flush=True)
    return run.returncode

if __name__=='__main__':raise SystemExit(main())
