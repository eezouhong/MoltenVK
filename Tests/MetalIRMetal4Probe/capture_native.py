"""Replay one existing frame through an opt-in legacy IR descriptor collector."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser()
    p.add_argument('--replayer',type=Path,required=True)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--plugin',type=Path,required=True)
    p.add_argument('--capture-manifest',type=Path,required=True)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--remove-unsupported',action='store_true',
                   help='Diagnostic normalization only; invalidates unmodified-capture acceptance')
    a=p.parse_args()
    if not os.environ.get('MELONX_DEBUG_GRAPHICS_LEASE'):
        raise RuntimeError('canonical graphics queue required')
    a.output.mkdir(exist_ok=False,parents=True)
    inputs=a.output/'inputs'; inputs.mkdir()
    manifest=json.loads(a.capture_manifest.read_text())
    capture=Path(manifest['absolutePath']); before=capture.stat()
    if before.st_size!=manifest['bytes'] or before.st_mtime_ns!=manifest['mtimeNs']:
        raise RuntimeError('capture identity changed')
    libraries=[a.native,a.plugin,*sorted(a.plugin.parent.glob('*.dylib'))]
    frozen={str(path.resolve()):sha(path) for path in libraries}
    frozen[str(a.replayer)]=sha(a.replayer)
    diff=subprocess.check_output(['git','-C',str(a.source),'diff','HEAD'],text=True)
    (a.output/'source.patch').write_text(diff)
    identity={'binaries':frozen,'sourceRevision':subprocess.check_output(['git','-C',str(a.source),'rev-parse','HEAD'],text=True).strip(),
              'sourceDiffSHA256':hashlib.sha256(diff.encode()).hexdigest(),
              'probeSources':{str(path.relative_to(a.source)):sha(path) for path in (a.source/'Tests/MetalIRMetal4Probe').iterdir() if path.is_file()},
              'captureSHA256FromExistingManifest':manifest['sha256'],
              'removeUnsupportedDeviceFeaturesAndExtensions':a.remove_unsupported,
              'scope':'instrumented legacy IR replay: exact base-key cardinality/descriptor evidence; timing invalidated by capture IO'}
    (a.output/'identity.json').write_text(json.dumps(identity,indent=2)+'\n')
    icd={'file_format_version':'1.0.0','ICD':{'library_path':str(a.native.resolve()),'api_version':'1.4.0'}}
    (a.output/'icd.json').write_text(json.dumps(icd)+'\n')
    env=dict(os.environ)
    for name in list(env):
        if name.startswith('GFXRECON_') or name in ('VK_INSTANCE_LAYERS','VK_LAYER_PATH','VK_LOADER_DEBUG','VK_DRIVER_FILES'):
            env.pop(name)
    env.update(VK_ICD_FILENAMES=str(a.output/'icd.json'),
        GFXRECON_VULKAN_LIBRARY_PATH='/opt/homebrew/Cellar/vulkan-loader/1.4.357.0/lib/libvulkan.1.4.357.dylib',
        MELONX_EXPERIMENTAL_METAL_IR='1',MELONX_METAL_IR_PLUGIN=str(a.plugin),
        MELONX_METAL_IR_TELEMETRY='1',MELONX_IR_PHASE0_DUMP=str(inputs),
        MVK_CONFIG_LOG_LEVEL='4',MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS='1',
        MVK_CONFIG_FAST_MATH_ENABLED='2',MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE='0',
        MVK_CONFIG_RESUME_LOST_DEVICE='1',MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION='1',
        MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_REPOSITORY_ENABLED='1',
        MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_RESIDENT_LIMIT='512',
        MVK_CONFIG_METAL4_TEXTURE_VIEW_POOL='1',MVK_CONFIG_METAL4_COMMAND_BACKEND='0',
        MVK_CONFIG_METAL4_COMPILER='1',MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES='1',
        MVK_CONFIG_METAL4_FLEXIBLE_ASYNC='1',MVK_CONFIG_METAL4_FLEXIBLE_ASYNC_MAX='3',
        MVK_CONFIG_METAL4_FLEXIBLE_CACHE_POLICY='1',MVK_CONFIG_METAL4_FLEXIBLE_PIPELINE_CACHE_MAX='1024',
        DYLD_PRINT_LIBRARIES='1',MTL_ENABLE_DEBUG_LAYER='0',MTL_DEBUG_LAYER='0',MTL_SHADER_VALIDATION='0')
    args=[str(a.replayer),'--swapchain','offscreen','--screenshot-all',
          '--screenshot-format','png','--screenshot-dir',str(a.output),
          '--omit-pipeline-cache-data','--measurement-file',str(a.output/'measurement.json'),str(capture)]
    if a.remove_unsupported:args.insert(1,'--remove-unsupported')
    started=time.monotonic()
    timed_out=False
    with (a.output/'replay.local.log').open('x') as log:
        child=subprocess.Popen(args,env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            code=child.wait(timeout=180)
        except subprocess.TimeoutExpired:
            timed_out=True
            child.terminate()
            try: code=child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill();code=child.wait(timeout=10)
    text=(a.output/'replay.local.log').read_text(errors='replace')
    process_prefix=f'dyld[{child.pid}]:'
    loaded_all={str(Path(line.rsplit(' ',1)[-1]).resolve()) for line in text.splitlines()
                if line.startswith(process_prefix) and line.rsplit(' ',1)[-1].startswith('/')}
    loaded={path for path in loaded_all if re.search(r'/libMoltenVK[^/]*\.dylib$',path)}
    actual_native=loaded=={str(a.native.resolve())}
    actual_compiler=all(str(path.resolve()) in loaded_all for path in a.plugin.parent.glob('*.dylib'))
    records=[]
    for name in ('render-descriptors.jsonl','compute-descriptors.jsonl'):
        path=inputs/name
        records.append({'file':name,'count':sum(1 for _ in path.open()) if path.exists() else 0})
    result={'exitCode':code,'normalExit':code==0,'timeout':timed_out,'durationSeconds':time.monotonic()-started,
            'actualNativeLoadedVerified':actual_native,'descriptorRecords':records,
            'actualCompilerLoadedVerified':actual_compiler,'recordedPid':child.pid,
            'metallibFiles':len(list(inputs.glob('*.metallib'))),
            'errorMarkers':{s:text.count(s) for s in ('MSL fallback disabled','[gfxrecon] ERROR','[gfxrecon] FATAL','IR phase0 library dump failed','IR phase0 render descriptor dump failed','IR phase0 compute descriptor dump failed')},
            'screenshots':len(list(a.output.glob('*.png')))}
    if (before.st_size,before.st_mtime_ns)!=(capture.stat().st_size,capture.stat().st_mtime_ns):
        raise RuntimeError('capture changed during replay')
    for path,digest in frozen.items():
        if sha(path)!=digest:raise RuntimeError('binary changed during replay')
    (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result))
    return 0 if code==0 and actual_native and actual_compiler and records[0]['count']>0 and not any(result['errorMarkers'].values()) else 1


if __name__=='__main__':
    raise SystemExit(main())
