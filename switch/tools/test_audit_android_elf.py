import struct
import unittest
from audit_android_elf import (parse_imports, svc_candidates,
                               text_section_ranges, section_svc_candidates,
                               compare_provider_manifest)

class AuditTests(unittest.TestCase):
    def test_versions_and_weak(self):
        symbols = ' 1: 00000000 0 FUNC GLOBAL DEFAULT UND pread@LIBC (2)\n 2: 00000000 0 NOTYPE WEAK DEFAULT UND optional\n'
        versions = 'Version needs section x:\n File: libc.so Cnt: 1\n Name: LIBC Flags: none Version: 2\n'
        result = parse_imports(symbols, versions)
        self.assertEqual(result[0]['library'], 'libc.so')
        self.assertTrue(result[0]['required'])
        self.assertFalse(result[1]['required'])
        with self.assertRaises(ValueError):
            parse_imports(symbols, '')

    def test_svc_only_executable_loads(self):
        data = bytearray(256)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 183)
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 1)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 128, 4096, 0, 8, 8, 4096)
        struct.pack_into('<II', data, 128, 0xd4000001, 0xd4000021)
        self.assertEqual(svc_candidates(data), [{'virtual_address':'0x1000','file_offset':'0x80'}])
        struct.pack_into('<I', data, 68, 4)
        self.assertEqual(svc_candidates(data), [])
        with self.assertRaises(ValueError):
            svc_candidates(data[:80])

    def test_direct_syscall_hints_are_not_execution_proof(self):
        data = bytearray(256)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 183)
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 1)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 128, 4096, 0, 32, 32, 4096)
        words = (
            0xd2800008 | (98 << 5), 0xd4000001,   # MOVZ X8,#futex; SVC #0
            0x52800008 | (172 << 5), 0xd4000001,  # MOVZ W8,#getpid; SVC #0
            0xd2800008 | (178 << 5), 0xd4000001,  # MOVZ X8,#gettid; SVC #0
            0xd2800009 | (98 << 5), 0xd4000001,   # X9 is not syscall register
        )
        struct.pack_into('<8I', data, 128, *words)
        entries = svc_candidates(data)
        self.assertEqual(len(entries), 4)
        self.assertEqual([item.get('syscall_hint', {}).get('name') for item in entries],
                         ['futex', 'getpid', 'gettid', None])
        self.assertEqual(entries[0]['syscall_hint']['number'], 98)
        self.assertEqual(entries[0]['syscall_hint']['basis'],
                         'immediately_preceding_movz_x8_or_w8')
        # No instruction-sequence identification is a runtime reachability claim.
        self.assertNotIn('executed', entries[0])

    def test_text_candidates_exclude_other_executable_segment_bytes(self):
        data = bytearray(512)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 183)
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 1)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 128, 4096, 0, 64, 64, 4096)
        # The first SVC is at the start of .text. Its preceding MOVZ lies
        # outside that section and must not be attributed to .text code.
        struct.pack_into('<5I', data, 128,
                         0xd2800008 | (172 << 5), 0xd4000001,
                         0xd503201f, 0xd2800008 | (178 << 5), 0xd4000001)
        struct.pack_into('<I', data, 168, 0xd4000001)  # Outside .text.
        names = b'\0.text\0.shstrtab\0'
        data[448:448 + len(names)] = names
        struct.pack_into('<Q', data, 40, 256)
        struct.pack_into('<HHH', data, 58, 64, 3, 2)
        struct.pack_into('<IIQQQQIIQQ', data, 320, 1, 1, 6,
                         0x1004, 132, 28, 0, 0, 4, 0)
        struct.pack_into('<IIQQQQIIQQ', data, 384, 7, 3, 0,
                         0, 448, len(names), 0, 0, 1, 0)
        all_candidates = svc_candidates(data)
        self.assertEqual(len(all_candidates), 3)
        ranges = text_section_ranges(data)
        self.assertEqual(ranges, [(132, 160)])
        text_candidates = section_svc_candidates(all_candidates, ranges)
        self.assertEqual(len(text_candidates), 2)
        self.assertNotIn('syscall_hint', text_candidates[0])
        self.assertEqual(text_candidates[1]['syscall_hint']['name'], 'gettid')
        self.assertIsNone(section_svc_candidates(all_candidates, None))
        self.assertEqual(len(section_svc_candidates(all_candidates, [(132, 146)])), 1)
        self.assertEqual(section_svc_candidates(all_candidates, []), [])

        # An ELF with no section table is not proof of zero .text syscalls.
        struct.pack_into('<Q', data, 40, 0)
        struct.pack_into('<HHH', data, 58, 0, 0, 0)
        self.assertIsNone(text_section_ranges(data))
        # Malformed section-name metadata must not claim reliable counts.
        struct.pack_into('<Q', data, 40, 256)
        struct.pack_into('<HHH', data, 58, 64, 3, 4)
        with self.assertRaises(ValueError):
            text_section_ranges(data)
        struct.pack_into('<Q', data, 40, 0)
        struct.pack_into('<HHH', data, 58, 64, 3, 2)
        with self.assertRaises(ValueError):
            text_section_ranges(data)

    def test_manifest_preserves_import_entries_and_distinct_missing_abi_keys(self):
        imports = [
            {'name': 'pread', 'version': 'LIBC', 'library': 'libc.so',
             'binding': 'GLOBAL', 'required': True},
            {'name': 'futex', 'version': 'LIBC', 'library': 'libc.so',
             'binding': 'GLOBAL', 'required': True},
            {'name': 'futex', 'version': 'LIBC', 'library': 'libc.so',
             'binding': 'GLOBAL', 'required': True},
            {'name': 'optional', 'version': None, 'library': None,
             'binding': 'WEAK', 'required': False},
        ]
        manifest = [{'name': 'pread', 'version': 'LIBC', 'library': 'libc.so'}]
        missing, distinct = compare_provider_manifest(imports, manifest)
        self.assertEqual(len(missing), 2)
        self.assertEqual(distinct, [{'name': 'futex', 'version': 'LIBC',
                                     'library': 'libc.so'}])
        # Same name but different provider/version must not count as implemented.
        missing, distinct = compare_provider_manifest(
            imports, [{'name': 'futex', 'version': None, 'library': None}])
        self.assertEqual(len(missing), 3)
        self.assertEqual(len(distinct), 2)
        with self.assertRaises(ValueError):
            compare_provider_manifest(imports, [{'name': 'pread'}])

if __name__ == '__main__':
    unittest.main()
