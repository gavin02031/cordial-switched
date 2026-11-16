import struct
import unittest
from audit_android_elf import parse_imports, svc_candidates

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

if __name__ == '__main__':
    unittest.main()
