import json
from pathlib import Path
import struct
import tempfile
import types
import unittest
from unittest.mock import patch
import zlib

import extract_cache as cache
import stats


def op(opcode, *args):
    return struct.pack('<' + 'I' * (len(args) + 1), ((len(args) + 1) << 16) | opcode, *args)


def shader(model=5):
    return (struct.pack('<5I', 0x07230203, 0x00010300, 0, 5, 0) + op(17, 1) + op(14, 0, 1)
            + op(15, model, 1, 0x6e69616d, 0) + op(16, 1, 17, 1, 1, 1)
            + op(19, 2) + op(33, 3, 2) + op(54, 2, 1, 0, 3)
            + op(248, 4) + op(253) + op(56))


def program(entries):
    return struct.pack('<i', len(entries)) + b''.join(struct.pack('<ii', stage, len(code)) + code for stage, code in entries)


class CacheTests(unittest.TestCase):
    def test_uncompressed_and_deflate_records(self):
        raw = program([(0, shader())])
        compressed = zlib.compress(raw)[2:-4]
        self.assertEqual(cache.decode_record(b'\0' + raw, len(raw)), raw)
        self.assertEqual(cache.decode_record(b'\1' + compressed, len(raw)), raw)

    def test_rejects_wrong_size_truncation_trailing_and_unknown_compression(self):
        raw = program([(0, shader())])
        compressed = zlib.compress(raw)[2:-4]
        for data, size in [(b'\0' + raw, len(raw) - 1), (b'\1' + compressed[:-1], len(raw)),
                           (b'\1' + compressed + b'x', len(raw)), (b'\1' + compressed, 1),
                           (b'\3' + raw, len(raw)), (b'', 1)]:
            with self.subTest(size=size, algorithm=data[:1]), self.assertRaises(ValueError):
                cache.decode_record(data, size)

    def test_stage_packet_is_not_a_magic_scan(self):
        packed = program([(0, shader())])
        self.assertEqual(cache.unpack_program(packed), [(0, shader())])
        with self.assertRaises(ValueError):
            cache.unpack_program(b'junk' + packed)
        with self.assertRaises(ValueError):
            cache.unpack_program(packed + shader())

    def test_duplicate_mismatched_and_truncated_stages(self):
        for raw in [program([(0, shader()), (0, shader())]), program([(1, shader())]),
                    program([(6, shader())]), program([(0, shader())])[:-4], struct.pack('<i', -1)]:
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                cache.unpack_program(raw)

    def test_rejects_zero_length_and_overrunning_spirv_instructions(self):
        head = shader()[:20]
        for bad in [head + struct.pack('<I', 0), head + op(1)[:-1], head + struct.pack('<I', (9 << 16) | 1)]:
            with self.assertRaises(ValueError):
                list(cache.instructions(bad))

    def test_toc_format_and_alignment(self):
        valid = cache.HEADER.pack(int.from_bytes(b'TOCH', 'little'), 0x10002, 7368, 0, 42, 0)
        header, entries = cache.read_toc(valid + cache.ENTRY.pack(8, 20, 21))
        self.assertEqual(header['codeGenVersion'], 7368)
        self.assertEqual(entries, [(8, 20, 21)])
        for bad in [valid[:31], valid + b'x', b'XXXX' + valid[4:], valid[:4] + struct.pack('<I', 0x10003) + valid[8:]]:
            with self.assertRaises(ValueError):
                cache.read_toc(bad)

    def test_extract_follows_toc_skips_tombstones_deduplicates_and_preserves_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = program([(0, shader())])
            # An unreferenced SPIR-V decoy must never be extracted.
            prefix = shader(4)
            encoded = b'\0' + raw
            data = prefix + encoded + b'\0metadata'
            header = cache.HEADER.pack(int.from_bytes(b'TOCH', 'little'), 0x10002, 7368, 0, 42, 0)
            toc = header + cache.ENTRY.pack(0, 0, 0) + cache.ENTRY.pack(2**64 - 1, 0, 0)
            toc += cache.ENTRY.pack(len(prefix), len(raw), len(encoded)) * 2
            (root / 'host.toc').write_bytes(toc)
            (root / 'host.data').write_bytes(data)
            with patch('extract_cache.subprocess.run', return_value=types.SimpleNamespace(stdout='test-validator', stderr='', returncode=0)):
                result = cache.extract(root / 'host.toc', root / 'host.data', root / 'out', 'validator',
                                       'vulkan1.2', 'synthetic', 1024 * 1024)
            self.assertTrue(result['complete'])
            self.assertEqual(result['moduleCount'], 1)
            self.assertEqual(len(result['modules'][0]['records']), 2)
            self.assertEqual(len(result['skipped']), 2)
            self.assertEqual((root / 'host.data').read_bytes(), data)
            self.assertEqual((root / 'host.toc').read_bytes(), toc)

    def test_out_of_range_toc_is_reported_without_export(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'host.toc').write_bytes(cache.HEADER.pack(int.from_bytes(b'TOCH', 'little'), 0x10002, 7368, 0, 42, 0)
                                          + cache.ENTRY.pack(999, 32, 33))
            (root / 'host.data').write_bytes(shader())
            with patch('extract_cache.subprocess.run', return_value=types.SimpleNamespace(stdout='test-validator', stderr='', returncode=0)):
                result = cache.extract(root / 'host.toc', root / 'host.data', root / 'out', 'validator',
                                       'vulkan1.2', 'synthetic', 1024)
            self.assertFalse(result['complete'])
            self.assertEqual(result['moduleCount'], 0)
            self.assertEqual(len(result['errors']), 1)

    def test_statistics_count_sites_and_explicit_modes(self):
        grammar = {'instructions': [{'opcode': x, 'opname': n} for x, n in
                    [(11, 'OpExtInstImport'), (12, 'OpExtInst'), (16, 'OpExecutionMode'), (71, 'OpDecorate')]],
                   'operand_kinds': [{'category': 'ValueEnum', 'kind': 'Decoration', 'enumerants': [{'value': 42, 'enumerant': 'NoContraction'}]},
                                     {'category': 'ValueEnum', 'kind': 'ExecutionMode', 'enumerants': [{'value': 4460, 'enumerant': 'RoundingModeRTZ'}]}]}
        glsl = {'instructions': [{'opcode': 50, 'opname': 'Fma'}]}
        name = b'GLSL.std.450\0'
        name += b'\0' * (-len(name) % 4)
        words = struct.unpack('<' + 'I' * (len(name) // 4), name)
        code = shader()[:20] + op(11, 6, *words) + op(12, 2, 7, 6, 50, 3, 4, 5)
        code += op(71, 7, 42) + op(16, 1, 4460, 32)
        found = stats.count_module(code, grammar, glsl)
        self.assertEqual(found['glsl450'], {'Fma': 1})
        self.assertEqual(found['decorations'], {'NoContraction': 1})
        self.assertEqual(found['executionModes'], {'RoundingModeRTZ:32': 1})


if __name__ == '__main__':
    unittest.main()
