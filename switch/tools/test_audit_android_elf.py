import struct
import unittest
from audit_android_elf import parse_imports, svc_candidates, compare_provider_manifest

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
