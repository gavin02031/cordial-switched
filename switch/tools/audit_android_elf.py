#!/usr/bin/env python3
"""Read-only ARM64 import inventory. Never loads or executes the supplied ELF."""
import argparse
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path



# AArch64 Linux syscall ABI passes the syscall number in x8. Recognize only
# a directly adjacent MOVZ x8/w8, #imm16 with zero halfword shift. This is
# static evidence, not a control-flow or reachability determination.
_LINUX_SYSCALL_HINTS = {98: 'futex', 172: 'getpid', 178: 'gettid'}


def adjacent_syscall_hint(data, instruction_offset):
    if instruction_offset < 4:
        return None
    prior = struct.unpack_from('<I', data, instruction_offset - 4)[0]
    if (prior & 0xffe0001f) not in (0xd2800008, 0x52800008):
        return None
    syscall_number = (prior >> 5) & 0xffff
    return {'number': syscall_number,
            'name': _LINUX_SYSCALL_HINTS.get(syscall_number),
            'basis': 'immediately_preceding_movz_x8_or_w8'}


def svc_candidates(data):
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('expected a little-endian ELF64 AArch64 library')
    offset = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    if stride != 56 or not count or count == 65535 or offset + stride * count > len(data):
        raise ValueError('invalid or unsupported program header table')
    found = set()
    executable_segments = []
    for i in range(count):
        kind, flags, start, address, _, size, memory, _ = struct.unpack_from('<IIQQQQQQ', data, offset + i * stride)
        if start + size > len(data) or size > memory:
            raise ValueError('invalid segment extent')
        if kind != 1 or not flags & 1:
            continue
        executable_segments.append((kind, flags, start, size))
        for relative in range((-address) % 4, size - 3, 4):
            word = struct.unpack_from('<I', data, start + relative)[0]
            if word == 0xd4000001:  # AArch64 SVC #0, including data in executable segments.
                found.add((address + relative, start + relative))
    candidates = []
    for address, offset in sorted(found):
        entry = {'virtual_address': hex(address), 'file_offset': hex(offset)}
        hint = adjacent_syscall_hint(data, offset)
        # Do not infer instructions across segment boundaries or file pages.
        if hint and any(kind == 1 and flags & 1 and start + 4 <= offset < start + size
                        for kind, flags, start, size in executable_segments):
            entry['syscall_hint'] = hint
        candidates.append(entry)
    return candidates


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



def compare_provider_manifest(imports, providers):
    """Compare exact ABI keys; preserve sites and also report distinct missing names."""
    if not isinstance(providers, list):
        raise ValueError('provider manifest must be a JSON array')
    available = set()
    for provider in providers:
        if not isinstance(provider, dict) or not all(
                key in provider for key in ('name', 'version', 'library')):
            raise ValueError('provider manifest record needs name, version and library')
        if not isinstance(provider['name'], str) or not provider['name']:
            raise ValueError('provider name must be a nonempty string')
        if provider['version'] is not None and not isinstance(provider['version'], str):
            raise ValueError('provider version must be a string or null')
        if provider['library'] is not None and not isinstance(provider['library'], str):
            raise ValueError('provider library must be a string or null')
        available.add((provider['name'], provider['version'], provider['library']))
    missing = [item for item in imports if item['required'] and
               (item['name'], item['version'], item['library']) not in available]
    seen = set()
    distinct = []
    for item in missing:
        key = (item['name'], item['version'], item['library'])
        if key not in seen:
            seen.add(key)
            distinct.append({'name': item['name'], 'version': item['version'],
                             'library': item['library']})
    return missing, distinct


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
        (report['missing_required'], report['missing_required_distinct']) = (
            compare_provider_manifest(report['imports'], supplied))
        report['missing_required_import_entries'] = len(report['missing_required'])
        report['missing_required_distinct_count'] = len(report['missing_required_distinct'])
        report['provider_manifest_sha256'] = hashlib.sha256(args.providers.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['required_symbols']} required, {report['optional_symbols']} optional symbols; "
          f"{len(report['svc_zero_candidates'])} SVC #0 candidates")
    if args.providers and report['missing_required']:
        print(f"Missing {report['missing_required_distinct_count']} distinct required ABI keys "
              f"across {report['missing_required_import_entries']} import entries:")
        for item in report['missing_required_distinct']:
            print(f"  {item['name']} @ {item['version'] or 'unversioned'} "
                  f"from {item['library'] or 'unspecified'}")
        raise SystemExit(1)


if __name__ == '__main__':
    main()
