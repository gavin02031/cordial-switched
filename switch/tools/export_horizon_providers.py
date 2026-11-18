#!/usr/bin/env python3
"""Export an ABI-key manifest from a *completed* real Horizon resolver audit.

This never executes the guest library. It accepts only an observed link report,
not source-token matches or guessed host function names.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path


def _fields(report):
    fields = {}
    for line in report.splitlines():
        if not (line.startswith('full_audit_') or
                line.startswith('morrow_build_version=')):
            continue
        if '=' not in line:
            raise ValueError('malformed Horizon audit line')
        key, value = line.split('=', 1)
        if key in fields:
            raise ValueError('duplicate Horizon audit field: ' + key)
        fields[key] = value
    return fields


def _count(fields, key):
    value = fields.get(key, '')
    if not re.fullmatch(r'[0-9]+', value):
        raise ValueError('missing or invalid count: ' + key)
    return int(value)


def _abi_key(record):
    if record.count(' @ ') != 1 or record.count(' from ') != 1:
        raise ValueError('ambiguous ABI-key record')
    name, rest = record.split(' @ ', 1)
    version, library = rest.split(' from ', 1)
    if (not all((name, version, library)) or
            any(part != part.strip() or part == '?'
                for part in (name, version, library))):
        raise ValueError('incomplete ABI key')
    if any(ord(ch) < 32 or ord(ch) > 126 for ch in record):
        raise ValueError('non-printable ABI key')
    return dict(name=name,
                version=None if version == 'unversioned' else version,
                library=None if library == 'unspecified' else library)


def _entries(fields, key_prefix, expected):
    indexed = {}
    for key, value in fields.items():
        if key.startswith(key_prefix):
            if key == key_prefix + 'list_complete':
                continue
            suffix = key[len(key_prefix):]
            if not re.fullmatch(r'[0-9]+', suffix):
                raise ValueError('malformed import index: ' + key)
            index = int(suffix)
            if index in indexed or index < 1:
                raise ValueError('duplicate or invalid import index: ' + key)
            indexed[index] = _abi_key(value)
    if sorted(indexed) != list(range(1, expected + 1)):
        raise ValueError('incomplete imported ABI-key sequence: ' + key_prefix)
    return [indexed[i] for i in range(1, expected + 1)]


def export_from_report(report):
    """Return (observed providers, evidence). No source-only assertions."""
    fields = _fields(report)
    if report.splitlines().count('event=full_final_base_audit_started') != 1:
        raise ValueError('not a single final-base Horizon audit')
    version = fields.get('morrow_build_version')
    if not version:
        raise ValueError('missing Morrow build identity')
    if fields.get('full_audit_relocation_scan_complete') != 'yes':
        raise ValueError('Horizon relocation scan did not finish')
    if fields.get('full_audit_status') not in ('3', '4', '5'):
        raise ValueError('audit status is not a completed relocation scan')
    for key in ('full_audit_missing_import_list_complete',
                'full_audit_resolved_import_list_complete'):
        if fields.get(key) != 'yes':
            raise ValueError('truncated Horizon import inventory: ' + key)

    lookups = _count(fields, 'full_audit_import_lookups')
    supported = _count(fields, 'full_audit_supported_imports')
    missing = _count(fields, 'full_audit_missing_imports')
    if lookups == 0 or supported + missing != lookups:
        raise ValueError('inconsistent final-base resolver lookup counts')
    if _count(fields, 'full_audit_missing_imports_reported') != missing:
        raise ValueError('incomplete missing-import inventory')
    if _count(fields, 'full_audit_resolved_imports_reported') != supported:
        raise ValueError('incomplete resolved-import inventory')

    entries = _entries(fields, 'full_audit_resolved_import_', supported)
    missing_entries = _entries(fields, 'full_audit_missing_import_', missing)
    keys = {(item['name'], item['version'], item['library'])
            for item in entries}
    missing_keys = {(item['name'], item['version'], item['library'])
                    for item in missing_entries}
    if keys & missing_keys:
        raise ValueError('contradictory Horizon resolver observation: '
                         'ABI key both resolved and missing')
    manifest = [dict(name=n, version=v, library=l)
                for n, v, l in sorted(keys, key=lambda x:
                                      (x[0], x[1] or '', x[2] or ''))]
    evidence = dict(
        kind='horizon_final_base_resolver_observation',
        morrow_build_version=version,
        relocation_scan_complete=True,
        import_lookups=lookups,
        resolved_import_entries=supported,
        missing_import_entries=missing,
        distinct_missing_abi_keys=len(missing_keys),
        distinct_resolved_abi_keys=len(manifest),
        caveat=('Evidence is limited to ABI keys the Horizon resolver was '
                'actually asked to resolve. It does not establish runtime '
                'function semantics, guest initialisation, or playability.'))
    return manifest, evidence


def _stage_write(path, payload):
    """Write payload to a sibling temp file, fsync, and return its path."""
    tmp = path.with_name(path.name + '.tmp')
    with open(tmp, 'wb') as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())
    return tmp


def write_pair(manifest, evidence, output, evidence_output):
    """Write the manifest/evidence pair with staged, fsynced replacements.

    The evidence carries the manifest's SHA-256 so a downstream check can
    reject a mismatched pair. Two renames are never globally atomic: the
    manifest is replaced LAST, so the only possible mid-failure state is a
    new evidence file beside an old manifest, which the hash rejects. If
    that second rename fails, the new evidence is removed so no apparently
    valid mismatched pair remains; the pre-existing pair is lost either way,
    which a missing file signals more honestly than a mismatch.
    """
    manifest_bytes = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
    evidence = dict(evidence)
    evidence['manifest_sha256'] = hashlib.sha256(manifest_bytes).hexdigest()
    evidence_bytes = (json.dumps(evidence, indent=2) + '\n').encode('utf-8')
    manifest_tmp = _stage_write(output, manifest_bytes)
    try:
        evidence_tmp = _stage_write(evidence_output, evidence_bytes)
        os.replace(evidence_tmp, evidence_output)
    except BaseException:
        manifest_tmp.unlink(missing_ok=True)
        raise
    try:
        os.replace(manifest_tmp, output)
    except BaseException:
        manifest_tmp.unlink(missing_ok=True)
        evidence_output.unlink(missing_ok=True)
        raise


def verify_pair(output, evidence_output):
    """True only when the evidence names the exact manifest bytes present."""
    manifest_bytes = output.read_bytes()
    evidence = json.loads(evidence_output.read_text())
    recorded = evidence.get('manifest_sha256')
    return bool(recorded) and recorded == hashlib.sha256(manifest_bytes).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('link_report', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--evidence-output', required=True, type=Path)
    args = parser.parse_args()
    raw = args.link_report.read_bytes()
    manifest, evidence = export_from_report(raw.decode('utf-8'))
    evidence['link_report_sha256'] = hashlib.sha256(raw).hexdigest()
    write_pair(manifest, evidence, args.output, args.evidence_output)
    print(f"Observed {len(manifest)} distinct Horizon provider ABI keys "
          f"across {evidence['resolved_import_entries']} resolved lookups")


if __name__ == '__main__':
    main()
