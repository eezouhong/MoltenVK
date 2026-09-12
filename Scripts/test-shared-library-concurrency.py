#!/usr/bin/env python3
"""Compile production scheduling bodies against controlled dependency doubles.

--baseline <git-ref> substitutes that revision's actual cache wrapper, rather
than making an absent new API the reason a regression test fails.
Native Vulkan/Metal integration is still required separately.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'MoltenVK/MoltenVK/GPUObjects'

def function(text, begin, end):
    return text[text.index(begin):text.index(end, text.index(begin))].strip()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline')
    parser.add_argument('--sanitizer', choices=['address', 'thread', 'undefined'])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    pipeline_path = 'MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm'
    pipeline = (ROOT / pipeline_path).read_text()
    if args.baseline:
        pipeline = subprocess.check_output(['git', 'show', args.baseline + ':' + pipeline_path], cwd=ROOT, text=True)
    wrapper = function(pipeline, 'MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(', '\nMVKShaderLibrary* MVKPipelineCache::getShaderLibraryImpl(')
    shader = (SRC / 'MVKShaderModule.mm').read_text()
    body = function(shader, 'MVKShaderLibrary* MVKShaderLibraryCache::getShaderLibraryConcurrent(', '// Finds and returns a shader library matching')
    template = (ROOT / 'Scripts/test-shared-library-concurrency.cpp').read_text()
    code = template.replace('// @PRODUCTION_CONCURRENT@', body).replace('// @PRODUCTION_WRAPPER@', wrapper)
    with tempfile.TemporaryDirectory(prefix='mvk-library-tests-') as tmp:
        cpp, exe = Path(tmp) / 'tests.cpp', Path(tmp) / 'tests'
        cpp.write_text(code)
        cmd = ['clang++', '-std=c++17', '-O1', '-g', '-pthread', '-I', str(SRC), str(cpp), '-o', str(exe)]
        if args.sanitizer:
            cmd += ['-fsanitize=' + args.sanitizer, '-fno-omit-frame-pointer']
        build = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if build.returncode:
            raise RuntimeError(build.stdout + build.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
    record = {'baseline': args.baseline, 'sanitizer': args.sanitizer,
              'wrapperSha256': hashlib.sha256(wrapper.encode()).hexdigest(),
              'concurrentBodySha256': hashlib.sha256(body.encode()).hexdigest(),
              'unmodifiedProductionBodies': True, 'buildExit': build.returncode,
              'exitCode': run.returncode, 'stdout': run.stdout, 'stderr': run.stderr,
              'scope': 'native C++ production control flow with fake compilation and logical views; not GPU validation'}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    return run.returncode

if __name__ == '__main__':
    raise SystemExit(main())
