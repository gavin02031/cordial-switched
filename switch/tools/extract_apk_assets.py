#!/usr/bin/env python3
"""Extract the assets/ tree of a Roblox APK into the Switch report directory
layout the runtime binds (sdmc:/switch/cordial-switched/assets).

The APK is proprietary user material: it stays on the operator's machine, is
never read from or written to the repository, and this tool performs no
network access. Only entries under the APK's top-level ``assets/`` directory
are extracted. ``classes*.dex`` is reported for information only - the
classpath is not installed on Horizon, so dex content is never extracted.

Exit codes: 0 success, 2 usage/input error, 3 unsafe archive entry,
4 archive has no assets/ tree.
"""

import argparse
import hashlib
import json
import os
import posixpath
import shutil
import sys
import zipfile

ASSET_PREFIX = "assets/"
MANIFEST_NAME = "asset-manifest.json"
MANIFEST_TEXT_NAME = "asset-manifest.txt"


class ExtractError(Exception):
    def __init__(self, message, code):
        super().__init__(message)
        self.code = code


def _safe_relpath(name):
    """Return the path below assets/ for a zip entry, or None to skip.

    Absolute paths, drive letters, backslashes and traversal segments raise
    ExtractError instead of being silently skipped so an operator notices a
    malformed archive.
    """
    if not name.startswith(ASSET_PREFIX):
        return None
    rel = name[len(ASSET_PREFIX):]
    if not rel:
        return None  # the assets/ directory entry itself
    if "\\" in rel or rel.startswith("/") or (len(rel) > 1 and rel[1] == ":"):
        raise ExtractError("unsafe archive entry name", 3)
    normalized = posixpath.normpath(rel)
    if normalized.startswith("../") or normalized == ".." or "/../" in normalized:
        raise ExtractError("archive entry escapes the asset root", 3)
    return normalized


def open_scan(apk_path):
    """Open an APK and return (archive, asset member plan, dex names).

    The caller owns closing the archive. The plan is a list of
    (ZipInfo, relative path) for regular files under assets/.
    """
    if not os.path.isfile(apk_path):
        raise ExtractError("APK not found: " + apk_path, 2)
    try:
        archive = zipfile.ZipFile(apk_path)
    except zipfile.BadZipFile:
        raise ExtractError("not a readable APK/zip: " + apk_path, 2)
    try:
        plan = []
        dex = []
        for info in archive.infolist():
            name = info.filename
            if name.startswith("classes") and name.endswith(".dex"):
                dex.append(name)
                continue
            rel = _safe_relpath(name)
            if rel is None or info.is_dir():
                continue
            plan.append((info, rel))
        if not plan:
            raise ExtractError("archive contains no assets/ files", 4)
        return archive, plan, dex
    except Exception:
        archive.close()
        raise


def _hash_stream(source, hasher):
    for chunk in iter(lambda: source.read(1 << 20), b""):
        hasher.update(chunk)


def run(apk_path, output_root, list_only=False):
    archive, plan, dex = open_scan(apk_path)
    manifest_files = []
    total_bytes = 0
    combined = hashlib.sha256()
    with archive:
        for info, rel in sorted(plan, key=lambda item: item[1]):
            digest = hashlib.sha256()
            if list_only:
                with archive.open(info) as source:
                    _hash_stream(source, digest)
            else:
                destination = os.path.join(output_root, *rel.split("/"))
                os.makedirs(os.path.dirname(destination), exist_ok=True)
                with archive.open(info) as source, open(destination, "wb") as out:
                    shutil.copyfileobj(source, out, 1 << 20)
                with open(destination, "rb") as written:
                    _hash_stream(written, digest)
            file_sha = digest.hexdigest()
            combined.update(rel.encode("utf-8"))
            combined.update(b"\0")
            combined.update(file_sha.encode("ascii"))
            combined.update(b"\0")
            total_bytes += info.file_size
            manifest_files.append({
                "path": rel,
                "size": info.file_size,
                "sha256": file_sha,
            })
    manifest = {
        "tool": "extract_apk_assets",
        "format": 1,
        "file_count": len(manifest_files),
        "total_bytes": total_bytes,
        "combined_sha256": combined.hexdigest(),
        "classes_dex_present": bool(dex),
        "classes_dex_names": sorted(dex),
        "files": manifest_files,
    }
    if not list_only:
        manifest_path = os.path.join(output_root, MANIFEST_NAME)
        with open(manifest_path, "w", encoding="utf-8") as handle:
            json.dump(manifest, handle, indent=2, sort_keys=True)
            handle.write("\n")
        # Line-based twin the on-device C verifier parses; the JSON stays the
        # human/host-readable record.
        text_path = os.path.join(output_root, MANIFEST_TEXT_NAME)
        with open(text_path, "w", encoding="utf-8") as handle:
            handle.write("format %d\n" % manifest["format"])
            handle.write("file_count %d\n" % manifest["file_count"])
            handle.write("total_bytes %d\n" % manifest["total_bytes"])
            handle.write("combined_sha256 %s\n" % manifest["combined_sha256"])
    return manifest


def main(argv):
    parser = argparse.ArgumentParser(
        description="Extract a Roblox APK's assets/ tree for the Switch "
                    "runtime. The APK never enters the repository.")
    parser.add_argument("apk", help="path to the local Roblox APK")
    parser.add_argument("output", nargs="?", default="assets",
                        help="output directory (default: ./assets)")
    parser.add_argument("--list", action="store_true",
                        help="scan and hash only; write nothing")
    args = parser.parse_args(argv)
    try:
        manifest = run(args.apk, args.output, list_only=args.list)
    except ExtractError as error:
        print("extract_apk_assets: " + str(error), file=sys.stderr)
        return error.code
    action = "listed" if args.list else "extracted"
    print("%s %d asset files (%d bytes); classes.dex present: %s" % (
        action, manifest["file_count"], manifest["total_bytes"],
        "yes" if manifest["classes_dex_present"] else "no"))
    if not args.list:
        print("manifest: " + os.path.join(args.output, MANIFEST_NAME))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
