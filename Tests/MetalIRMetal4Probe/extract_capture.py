"""CPU-only extraction of existing Vulkan creation records; no replay or GPU."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--converter', type=Path, required=True)
    p.add_argument('--capture-manifest', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    manifest = json.loads(a.capture_manifest.read_text())
    capture = Path(manifest['absolutePath'])
    before = capture.stat()
    if before.st_size != manifest['bytes'] or before.st_mtime_ns != manifest['mtimeNs']:
        raise RuntimeError('capture identity changed')
    a.output.mkdir(exist_ok=False, parents=True)
    names = (b'vkCreateGraphicsPipelines', b'vkCreateComputePipelines', b'vkCreateShaderModule',
             b'vkCreateRenderPass', b'vkCreatePipelineLayout', b'vkCreateDescriptorSetLayout',
             b'vkCreateSampler', b'vkCmdPushDescriptorSet')
    counts = {name.decode(): 0 for name in names}
    lines = 0
    selected = 0
    with (a.output / 'convert.stderr').open('x') as err, (a.output / 'creation-records.local.jsonl').open('xb') as out:
        proc = subprocess.Popen([str(a.converter), '--format', 'jsonl', '--output', 'stdout', str(capture)],
                                stdout=subprocess.PIPE, stderr=err)
        try:
            for line in proc.stdout:
                lines += 1
                hits = [name for name in names if name in line]
                if hits:
                    if len(line) > 16 * 1024 * 1024:
                        raise RuntimeError('creation record exceeds bounded extraction size')
                    json.loads(line)
                    out.write(line)
                    selected += 1
                    for name in hits:
                        counts[name.decode()] += 1
                if out.tell() > 128 * 1024 * 1024:
                    raise RuntimeError('bounded extraction exceeded')
            code = proc.wait(timeout=60)
        finally:
            proc.stdout.close()
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=30)
    after = capture.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise RuntimeError('capture changed while reading')
    summary = {'exitCode': code, 'captureSHA256FromExistingManifest': manifest['sha256'],
               'captureSizeAndMtimeVerified': True, 'converterSHA256': hashlib.sha256(a.converter.read_bytes()).hexdigest(),
               'totalRecords': lines, 'selectedRecords': selected, 'recordCounts': counts,
               'scope': 'existing capture, offline parse only; no Metal API or GPU execution'}
    (a.output / 'extraction.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))
    return code


if __name__ == '__main__':
    raise SystemExit(main())
