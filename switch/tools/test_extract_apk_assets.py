#!/usr/bin/env python3
"""Tests for extract_apk_assets: synthetic zips only, no real APK material."""

import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import extract_apk_assets as tool


def make_apk(path, entries):
    with zipfile.ZipFile(path, "w") as archive:
        for name, payload in entries:
            archive.writestr(name, payload)


class ExtractApkAssetsTest(unittest.TestCase):
    def setUp(self):
        self.workdir = tempfile.mkdtemp(prefix="apk-assets-test-")
        self.addCleanup(shutil.rmtree, self.workdir, ignore_errors=True)
        self.apk = os.path.join(self.workdir, "test.apk")
        self.out = os.path.join(self.workdir, "assets")

    def test_extracts_assets_tree_and_manifest(self):
        make_apk(self.apk, [
            ("assets/", b""),
            ("assets/shaders/a.frag", b"frag-bytes"),
            ("assets/ui/b.png", b"png-bytes"),
            ("classes.dex", b"dex"),
            ("lib/arm64-v8a/libroblox.so", b"not-an-asset"),
        ])
        manifest = tool.run(self.apk, self.out)
        self.assertEqual(manifest["file_count"], 2)
        self.assertEqual(manifest["total_bytes"], len(b"frag-bytes") + len(b"png-bytes"))
        self.assertTrue(manifest["classes_dex_present"])
        self.assertEqual(manifest["classes_dex_names"], ["classes.dex"])
        with open(os.path.join(self.out, "shaders", "a.frag"), "rb") as handle:
            self.assertEqual(handle.read(), b"frag-bytes")
        with open(os.path.join(self.out, tool.MANIFEST_NAME), encoding="utf-8") as handle:
            on_disk = json.load(handle)
        self.assertEqual(on_disk["combined_sha256"], manifest["combined_sha256"])
        text_path = os.path.join(self.out, tool.MANIFEST_TEXT_NAME)
        with open(text_path, encoding="utf-8") as handle:
            fields = dict(line.split() for line in handle.read().splitlines())
        self.assertEqual(fields["format"], "1")
        self.assertEqual(fields["file_count"], "2")
        self.assertEqual(fields["total_bytes"], str(manifest["total_bytes"]))
        self.assertEqual(fields["combined_sha256"], manifest["combined_sha256"])
        entry = next(f for f in on_disk["files"] if f["path"] == "ui/b.png")
        self.assertEqual(entry["sha256"], hashlib.sha256(b"png-bytes").hexdigest())

    def test_manifest_is_deterministic_regardless_of_member_order(self):
        entries = [
            ("assets/z.bin", b"z"),
            ("assets/a.bin", b"a"),
            ("assets/m.bin", b"m"),
        ]
        make_apk(self.apk, entries)
        first = tool.run(self.apk, self.out)["combined_sha256"]
        other = os.path.join(self.workdir, "other.apk")
        make_apk(other, list(reversed(entries)))
        second_out = os.path.join(self.workdir, "assets2")
        second = tool.run(other, second_out)["combined_sha256"]
        self.assertEqual(first, second)

    def test_text_manifest_absent_in_list_mode(self):
        make_apk(self.apk, [("assets/x.bin", b"x")])
        tool.run(self.apk, self.out, list_only=True)
        self.assertFalse(os.path.exists(os.path.join(self.out, tool.MANIFEST_TEXT_NAME)))

    def test_list_mode_writes_nothing(self):
        make_apk(self.apk, [("assets/x.bin", b"x")])
        manifest = tool.run(self.apk, self.out, list_only=True)
        self.assertEqual(manifest["file_count"], 1)
        self.assertFalse(os.path.exists(self.out))

    def test_rejects_traversal_entry(self):
        make_apk(self.apk, [("assets/../evil.bin", b"e")])
        with self.assertRaises(tool.ExtractError) as caught:
            tool.run(self.apk, self.out)
        self.assertEqual(caught.exception.code, 3)
        self.assertFalse(os.path.exists(os.path.join(self.workdir, "evil.bin")))

    def test_rejects_backslash_and_absolute_names(self):
        make_apk(self.apk, [("assets/..\\evil", b"e")])
        with self.assertRaises(tool.ExtractError) as caught:
            tool.run(self.apk, self.out)
        self.assertEqual(caught.exception.code, 3)

    def test_no_assets_is_exit_4(self):
        make_apk(self.apk, [("classes.dex", b"dex")])
        with self.assertRaises(tool.ExtractError) as caught:
            tool.run(self.apk, self.out)
        self.assertEqual(caught.exception.code, 4)

    def test_missing_apk_is_exit_2(self):
        with self.assertRaises(tool.ExtractError) as caught:
            tool.run(os.path.join(self.workdir, "missing.apk"), self.out)
        self.assertEqual(caught.exception.code, 2)

    def test_non_zip_is_exit_2(self):
        with open(self.apk, "wb") as handle:
            handle.write(b"not a zip")
        with self.assertRaises(tool.ExtractError) as caught:
            tool.run(self.apk, self.out)
        self.assertEqual(caught.exception.code, 2)

    def test_multiple_dex_names_reported(self):
        make_apk(self.apk, [
            ("assets/x.bin", b"x"),
            ("classes2.dex", b"d2"),
            ("classes.dex", b"d1"),
        ])
        manifest = tool.run(self.apk, self.out, list_only=True)
        self.assertEqual(manifest["classes_dex_names"], ["classes.dex", "classes2.dex"])

    def test_main_cli_success_and_errors(self):
        make_apk(self.apk, [("assets/x.bin", b"x")])
        self.assertEqual(tool.main([self.apk, self.out]), 0)
        self.assertTrue(os.path.isfile(os.path.join(self.out, tool.MANIFEST_NAME)))
        self.assertEqual(tool.main([os.path.join(self.workdir, "nope.apk"), self.out]), 2)


if __name__ == "__main__":
    unittest.main()
