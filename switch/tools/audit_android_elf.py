#!/usr/bin/env python3
"""Read-only ARM64 import inventory. Never loads or executes the supplied ELF."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path


def svc_candidates(data):
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('expected a little-endian ELF64 AArch64 library')
    offset = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    if stride != 56 or not count or count == 65535 or offset + stride * count > len(data):
        raise ValueError('invalid or unsupported program header table')
    found = set()
    for i in range(count):
        kind, flags, start, address, _, size, memory, _ = struct.unpack_from('<IIQQQQQQ', data, offset + i * stride)
        if start + size > len(data) or size > memory:
            raise ValueError('invalid segment extent')
        if kind != 1 or not flags & 1:
            continue
        for relative in range((-address) % 4, size - 3, 4):
            word = struct.unpack_from('<I', data, start + relative)[0]
            if word == 0xd4000001:  # AArch64 SVC #0, including data in executable segments.
                found.add((address + relative, start + relative))
    return [{'virtual_address': hex(a), 'file_offset': hex(o)} for a, o in sorted(found)]


def parse_imports(symbols, versions):
    providers = {}
    library = None
    in_needs = False
    for line in versions.splitlines():
        if 'Version needs section' in line:
            in_needs = True
        elif 'Version ' in line and ' section ' in line:
            in_needs = False
        if not in_needs:
            continue
        match = re.search(r'File:\s+(\S+)', line)
        if match:
            library = match[1]
        match = re.search(r'Name:\s+(\S+).*Version:\s+(\d+)', line)
        if match:
            providers[int(match[2])] = (match[1], library)
    imports = []
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) < 8 or not re.fullmatch(r'\d+:', fields[0]) or fields[6] != 'UND':
            continue
        binding, name = fields[4], fields[7]
        if binding not in ('GLOBAL', 'WEAK', 'UNIQUE'):
            raise ValueError('unsupported undefined symbol binding: ' + binding)
        version, library = None, None
        if '@' in name:
            name, version = name.split('@', 1)
            version = version.lstrip('@')
            index = re.search(r'\((\d+)\)\s*$', line)
            if not index or int(index[1]) not in providers:
                raise ValueError('missing version requirement for ' + name)
            recorded, library = providers[int(index[1])]
            if version != recorded:
                raise ValueError('inconsistent version requirement for ' + name)
        imports.append(dict(name=name, version=version, library=library, binding=binding,
                            required=binding != 'WEAK'))
    return imports


def inventory(path, readelf):
    data = path.read_bytes()
    candidates = svc_candidates(data)
    def inspect(*args):
        return subprocess.run([readelf, *args, str(path)], check=True, text=True,
                              capture_output=True, env={'LC_ALL': 'C', 'PATH': __import__('os').environ.get('PATH', '')}).stdout
    symbols = inspect('--dyn-syms', '--wide')
    if not re.search(r"Symbol table ['\"]\.dynsym['\"]", symbols):
        raise ValueError('readelf did not expose .dynsym; cannot claim a complete import inventory')
    imports = parse_imports(symbols, inspect('--version-info', '--wide'))
    needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', inspect('--dynamic', '--wide'))
    return dict(sha256=hashlib.sha256(data).hexdigest(), needed=needed, imports=imports,
                required_symbols=sum(x['required'] for x in imports),
                optional_symbols=sum(not x['required'] for x in imports),
                svc_zero_candidates=candidates,
                caveat='SVC matches may be data or unreachable code; absence does not exclude computed instructions or syscalls in dependencies. Import inventory is not an execution test.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--readelf', default='llvm-readelf')
    parser.add_argument('--providers', type=Path, help='JSON: target-verified list of {name, version, library} objects')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = inventory(args.elf, args.readelf)
    if args.providers:
        supplied = json.loads(args.providers.read_text())
        available = {(p['name'], p['version'], p['library']) for p in supplied}
        report['missing_required'] = [p for p in report['imports'] if p['required'] and
                                     (p['name'], p['version'], p['library']) not in available]
        report['provider_manifest_sha256'] = hashlib.sha256(args.providers.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['required_symbols']} required, {report['optional_symbols']} optional symbols; "
          f"{len(report['svc_zero_candidates'])} SVC #0 candidates")
    if args.providers and report['missing_required']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
