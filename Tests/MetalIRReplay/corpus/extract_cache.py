#!/usr/bin/env python3
"""Read Ryujinx TOCH v1.2 records; never scan data for SPIR-V magic.

Format references (MeloNX 46996b2e, unchanged from 65ed3e2c):
  Shader/DiskCache/{DiskCacheHostStorage,BinarySerializer,ShaderBinarySerializer}.cs
Inputs must be stopped, read-only snapshots. Modules/manifests are private assets.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zlib

HEADER = struct.Struct('<4I2Q')
ENTRY = struct.Struct('<QII')
STAGES = ('compute', 'vertex', 'tess-control', 'tess-evaluation', 'geometry', 'fragment')
EXECUTION_MODELS = (5, 0, 1, 2, 3, 4)
MAX_RECORD = 16 * 1024 * 1024


def digest(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def instructions(code):
    if len(code) < 20 or len(code) % 4:
        raise ValueError('invalid SPIR-V byte length')
    words = struct.unpack('<' + 'I' * (len(code) // 4), code)
    if words[0] != 0x07230203 or words[4] != 0:
        raise ValueError('invalid SPIR-V header')
    offset = 5
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xffff
        if count == 0 or offset + count > len(words):
            raise ValueError('invalid SPIR-V instruction extent')
        yield opcode, words[offset + 1:offset + count]
        offset += count


def decode_record(encoded, expected):
    if not encoded or not 0 < expected <= MAX_RECORD:
        raise ValueError('invalid compressed record size')
    algorithm, payload = encoded[0], encoded[1:]
    if algorithm == 0:
        decoded = payload
    elif algorithm == 1:
        decoder = zlib.decompressobj(-15)  # .NET DeflateStream uses RFC 1951.
        decoded = decoder.decompress(payload, expected + 1)
        if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
            raise ValueError('truncated, oversized or trailing deflate record')
    elif algorithm == 2:
        try:
            import brotli
        except ImportError as error:
            raise ValueError('Brotli record requires the optional brotli package') from error
        decoder = brotli.Decompressor()
        chunks, size = [], 0
        for offset in range(0, len(payload), 256):
            chunk = decoder.process(payload[offset:offset + 256])
            size += len(chunk)
            if size > expected:
                raise ValueError('oversized Brotli record')
            chunks.append(chunk)
        if not decoder.is_finished():
            raise ValueError('truncated Brotli record')
        decoded = b''.join(chunks)
    else:
        raise ValueError(f'unknown compression algorithm {algorithm}')
    if len(decoded) != expected:
        raise ValueError('decoded length differs from TOC')
    return decoded


def unpack_program(code):
    if len(code) < 4:
        raise ValueError('truncated program header')
    count, = struct.unpack_from('<i', code)
    if not 1 <= count <= len(STAGES):
        raise ValueError('invalid stage count')
    offset, seen, modules = 4, set(), []
    for _ in range(count):
        if offset + 8 > len(code):
            raise ValueError('truncated stage header')
        stage, size = struct.unpack_from('<ii', code, offset)
        offset += 8
        if stage not in range(len(STAGES)) or stage in seen:
            raise ValueError('invalid or duplicate stage')
        if size <= 0 or size % 4 or offset + size > len(code):
            raise ValueError('invalid stage byte extent')
        module = code[offset:offset + size]
        models = {operands[0] for op, operands in instructions(module) if op == 15 and len(operands) >= 3}
        if models != {EXECUTION_MODELS[stage]}:
            raise ValueError('cache stage differs from SPIR-V entry point')
        modules.append((stage, module))
        seen.add(stage)
        offset += size
    if offset != len(code):
        raise ValueError('trailing program data')
    return modules


def read_toc(data):
    if len(data) < HEADER.size or (len(data) - HEADER.size) % ENTRY.size:
        raise ValueError('truncated TOCH header or entry')
    magic, version, codegen, padding, timestamp, reserved = HEADER.unpack_from(data)
    if magic != int.from_bytes(b'TOCH', 'little') or version != 0x00010002:
        raise ValueError('unsupported TOCH magic/version (requires v1.2)')
    return {'formatVersion': version, 'codeGenVersion': codegen, 'timestamp': timestamp,
            'padding': padding, 'reserved': reserved}, list(ENTRY.iter_unpack(data[HEADER.size:]))


def extract(toc, data, output, validator, target_env, source_label, max_output_bytes):
    toc, data, output = Path(toc), Path(data), Path(output)
    if toc.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('TOC exceeds 16 MiB budget')
    header, entries = read_toc(toc.read_bytes())
    if len(entries) > 100000:
        raise ValueError('TOC exceeds 100000 program budget')
    before = {'toc': digest(toc), 'data': digest(data)}
    validator_version = subprocess.run([str(validator), '--version'], check=True,
                                     capture_output=True, text=True).stdout.strip()
    output.mkdir(parents=True, exist_ok=False)
    (output / 'validation').mkdir()
    modules, errors, skipped, written = {}, [], [], 0
    with data.open('rb') as stream:
        length = data.stat().st_size
        for index, (offset, uncompressed, compressed) in enumerate(entries):
            if offset in (0, 0xffffffffffffffff) and uncompressed == compressed == 0:
                skipped.append({'programIndex': index, 'reason': 'empty' if offset == 0 else 'invalidated'})
                continue
            try:
                # Metadata follows each compressed payload; require at least one remaining byte.
                if not 0 < uncompressed <= MAX_RECORD or compressed <= 0 or compressed > MAX_RECORD + 1024 * 1024:
                    raise ValueError('invalid or over-budget TOC length')
                if offset > 0x7fffffffffffffff or offset + compressed >= length:
                    raise ValueError('TOC extent outside host data or missing following metadata')
                stream.seek(offset)
                program = decode_record(stream.read(compressed), uncompressed)
                for stage, code in unpack_program(program):
                    sha = hashlib.sha256(code).hexdigest()
                    key = f'{sha}-{STAGES[stage]}'
                    if key not in modules:
                        written += len(code)
                        if written > max_output_bytes:
                            raise ValueError('corpus output byte budget exceeded')
                        name = key + '.spv'
                        path = output / name
                        path.write_bytes(code)
                        result = subprocess.run([str(validator), '--target-env', target_env, str(path)],
                                                capture_output=True, text=True, timeout=30)
                        (output / 'validation' / (key + '.log')).write_text(result.stdout + result.stderr)
                        modules[key] = {'sha256': sha, 'stage': STAGES[stage], 'stageIndex': stage,
                                        'bytes': len(code), 'file': name, 'validationExit': result.returncode,
                                        'sourceLabel': source_label, 'records': []}
                    modules[key]['records'].append({'programIndex': index, 'offset': offset,
                                                   'compressedSize': compressed, 'uncompressedSize': uncompressed})
            except (ValueError, zlib.error, subprocess.TimeoutExpired) as error:
                errors.append({'programIndex': index, 'error': str(error)})
    unchanged = before == {'toc': digest(toc), 'data': digest(data)}
    if not unchanged:
        errors.append({'error': 'source snapshot changed during extraction'})
    manifest = {'schema': 'melonx-ir-corpus-v1', 'sourceLabel': source_label,
                'format': header, 'sourceSHA256': before, 'sourceUnchanged': unchanged,
                'validator': {'version': validator_version, 'targetEnv': target_env},
                'programCount': len(entries), 'skipped': skipped, 'errors': errors,
                'moduleCount': len(modules), 'moduleBytes': sum(x['bytes'] for x in modules.values()),
                'validationFailures': sum(x['validationExit'] != 0 for x in modules.values()),
                'modules': list(modules.values())}
    manifest['complete'] = unchanged and not errors and manifest['validationFailures'] == 0
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toc', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--validator', type=Path, required=True)
    parser.add_argument('--source-label', required=True)
    parser.add_argument('--target-env', default='vulkan1.2')
    parser.add_argument('--max-output-mib', type=int, default=512)
    args = parser.parse_args()
    if not 1 <= args.max_output_mib <= 4096:
        parser.error('output budget must be 1..4096 MiB')
    manifest = extract(args.toc, args.data, args.output, args.validator, args.target_env,
                       args.source_label, args.max_output_mib * 1024 * 1024)
    print(json.dumps({k: manifest[k] for k in ('programCount', 'moduleCount', 'moduleBytes', 'validationFailures', 'complete')}, sort_keys=True))
    return 0 if manifest['complete'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
