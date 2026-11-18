#!/usr/bin/env python3
"""Read-only ARM64 import inventory. Never loads or executes the supplied ELF."""
import argparse
import ctypes
import hashlib
import json
import platform
import re
import struct
import subprocess
import tempfile
from pathlib import Path



# AArch64 Linux syscall ABI passes the syscall number in x8. Recognize only
# a directly adjacent MOVZ x8/w8, #imm16 with zero halfword shift. This is
# static evidence, not a control-flow or reachability determination.
_LINUX_SYSCALL_HINTS = {98: 'futex', 172: 'getpid', 178: 'gettid'}


def _decode_x8_chain(data, instruction_offset, window=8):
    """Walk back up to `window` instructions decoding a MOVZ/MOVK immediate
    chain into x8/w8. Returns the derived syscall number, or None when x8 is
    not statically derivable in the window. Stops at control flow, since
    scanning across a branch would attribute another path's x8."""
    value = 0
    known_mask = 0  # bits pinned by MOVKs later than the MOVZ being sought
    for back in range(1, window + 1):
        if instruction_offset - 4 * back < 0:
            break
        word = struct.unpack_from('<I', data, instruction_offset - 4 * back)[0]
        # Stop at (in)direct branches and returns; x8 beyond is another path's.
        if (word & 0x7c000000) == 0x14000000 or (word & 0xfffffc1f) in (0xd61f0000, 0xd65f0000):
            break
        if (word & 31) != 8:
            continue
        # Something addresses x8/w8. Only a MOVZ/MOVK immediate chain is
        # statically decodable; any other write redefines x8, so whatever
        # came earlier no longer describes the SVC's syscall number. A store
        # with x8 as source also lands here, which merely costs precision.
        shift = ((word >> 21) & 3) * 16
        imm = ((word >> 5) & 0xffff) << shift
        if (word & 0xffe0001f) in (0xd2800008, 0x52800008):  # MOVZ x8/w8, #imm
            # MOVZ zeroes the register, then later MOVKs overwrote their
            # halfwords; combine for the full immediate.
            return (value & known_mask) | imm
        if (word & 0xffe0001f) == 0xf2800008:  # MOVK x8
            known_mask |= 0xffff << shift
            value = (value & ~(0xffff << shift)) | imm
            continue
        break
    return None


def _x8_computed_near(data, instruction_offset, window=4):
    """True when an instruction within `window` before the SVC writes x8/w8
    with a data-processing operation (EOR/LSR/SXTW family) rather than an
    immediate - the shape of runtime-derived syscall numbers, which static
    analysis cannot resolve to a value."""
    for back in range(1, window + 1):
        if instruction_offset - 4 * back < 0:
            break
        word = struct.unpack_from('<I', data, instruction_offset - 4 * back)[0]
        # Same control-flow boundary as the immediate-chain walk: an EOR on
        # another path says nothing about this SVC.
        if (word & 0x7c000000) == 0x14000000 or (word & 0xfffffc1f) in (0xd61f0000, 0xd65f0000):
            break
        rd = word & 31
        if rd != 8:
            continue
        if (word & 0x1f000000) == 0x0a000000 or (word & 0x1f000000) == 0x4a000000:  # EOR (shifted/reg)
            return True
        if (word & 0x7f200000) == 0x1ac02000 or (word & 0x7f200000) == 0x1ac02400:  # LSRV/ASRV
            return True
        if (word & 0xff20fc00) == 0x93407c00:  # SXTW
            return True
    return False


def svc_context(data, instruction_offset):
    """Classify one SVC #0 site by how x8 is produced nearby.
    direct_number: statically derivable immediate chain (value included).
    derived_at_runtime: x8 written by data-processing ops just before the
    SVC - a runtime-computed syscall number, unresolvable statically.
    unknown: neither shape found in the scan window."""
    number = _decode_x8_chain(data, instruction_offset)
    if number is not None:
        context = {'classification': 'direct_number', 'number': number}
        name = _LINUX_SYSCALL_HINTS.get(number)
        if name:
            context['name'] = name
        return context
    if _x8_computed_near(data, instruction_offset):
        return {'classification': 'derived_at_runtime'}
    return {'classification': 'unknown'}


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
        if any(kind == 1 and flags & 1 and start + 4 <= offset < start + size
               for kind, flags, start, size in executable_segments):
            entry['svc_context'] = svc_context(data, offset)
        candidates.append(entry)
    return candidates


def text_section_ranges(data):
    """Return file ranges of executable .text sections, or None if stripped.

    PT_LOAD has permission to execute but can also contain literal pools and
    non-code data. A segment-wide SVC count must not be reported as a count
    of instructions found inside the ELF's named .text section.
    """
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected a little-endian ELF64 image')
    section_offset = struct.unpack_from('<Q', data, 40)[0]
    stride, count, names_index = struct.unpack_from('<HHH', data, 58)
    if section_offset == 0 and count == 0:
        return None  # A valid ELF can omit its section headers entirely.
    if (section_offset == 0 or stride != 64 or not count or
            not names_index or names_index >= count or
            count == 65535 or names_index == 65535 or
            section_offset + stride * count > len(data)):
        raise ValueError('invalid or unsupported section header table')

    def section(index):
        return struct.unpack_from('<IIQQQQIIQQ', data,
                                  section_offset + index * stride)

    names = section(names_index)
    if names[1] != 3 or names[4] + names[5] > len(data):
        raise ValueError('missing or invalid section-name string table')
    names_start, names_end = names[4], names[4] + names[5]
    result = []
    for index in range(count):
        sh = section(index)
        if sh[1] != 1 or not sh[2] & 4:  # SHT_PROGBITS, SHF_EXECINSTR
            continue
        if sh[0] >= names[5]:
            raise ValueError('invalid executable section name offset')
        name_start = names_start + sh[0]
        name_end = data.find(b'\0', name_start, names_end)
        if name_end < 0:
            raise ValueError('unterminated executable section name')
        if data[name_start:name_end] != b'.text':
            continue
        start, size = sh[4], sh[5]
        if start + size > len(data):
            raise ValueError('.text section extends past the ELF')
        result.append((start, start + size))
    return result


def section_svc_candidates(candidates, ranges):
    if ranges is None:
        return None
    result = []
    for candidate in candidates:
        offset = int(candidate['file_offset'], 16)
        for start, end in ranges:
            if start <= offset and offset + 4 <= end:
                entry = dict(candidate)
                # An adjacent MOVZ outside .text is not an adjacent .text
                # instruction; keep the segment-wide hint separate.
                if offset - 4 < start:
                    entry.pop('syscall_hint', None)
                result.append(entry)
                break
    return result


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
    text_ranges = text_section_ranges(data)
    text_candidates = section_svc_candidates(candidates, text_ranges)
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
                svc_text_candidates=text_candidates,
                svc_text_section_status=('unavailable' if text_ranges is None else
                                         'found' if text_ranges else 'absent'),
                caveat='Executable PT_LOAD segments contain more than .text code. Both lists are static SVC #0 matches, not evidence of reachable syscalls. Absent section headers cannot establish a zero .text count. Import inventory is not an execution test.')



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


def target_registered_names():
    """Names the Switch build registers that a host build cannot resolve: the
    EGL/GLES2 catalogue (compiled out without __SWITCH__) and the ARM64 jump
    bridge (compiled out without __aarch64__). Static source parse of the same
    resolver sources the target links; a name listed here is registered with
    the target resolver, not exercised on hardware."""
    source = Path(__file__).resolve().parents[1] / 'source'
    egl = (source / 'android_egl_gles_compat.c').read_text()
    names = set(re.findall(r'X\((egl\w+|gl\w+)\)', egl))
    names.update(re.findall(r'strcmp\(name, "(egl\w+|gl\w+)"\)', egl))
    jump = (source / 'android_jump_compat.c').read_text()
    names.update(re.findall(r'strcmp\(name, "(setjmp|longjmp)"\)', jump))
    return names


def host_provider_coverage(imports):
    """Query the host build of the same resolver the two Switch link paths use."""
    if platform.system() != 'Linux' or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise ValueError('--host-providers requires a 64-bit Linux build host')
    source = Path(__file__).resolve().parents[1] / 'source'
    sources = sorted(source.glob('android_*_compat.c'))
    sources.extend(source / name for name in (
        'android_import_resolver.c', 'android_arm64_tcb_shadow.c',
        'android_virtual_fd.c', 'engine_tls_scratch.c',
        'engine_module_registry.c'))
    if platform.machine() == 'aarch64':
        sources.append(source / 'android_bionic_setjmp.s')
    with tempfile.TemporaryDirectory() as tmp:
        library = Path(tmp) / 'host-providers.so'
        subprocess.run(['cc', '-std=c11', '-O2', '-fPIC', '-shared', '-Wl,-z,defs',
                        *sources, '-lm', '-pthread', '-o', library], check=True)
        resolve = ctypes.CDLL(str(library)).android_import_resolve
        resolve.restype = ctypes.c_bool
        resolve.argtypes = [ctypes.c_char_p] * 3 + [ctypes.POINTER(ctypes.c_uint64)]
        records = []
        for item in imports:
            args = [item[key].encode() if item[key] is not None else None
                    for key in ('name', 'version', 'library')]
            address = ctypes.c_uint64()
            found = resolve(*args, ctypes.byref(address))
            records.append(dict(item, supported=bool(found and address.value)))
    target = target_registered_names()
    for row in records:
        row['target_registered'] = not row['supported'] and row['name'] in target
    return dict(platform='Linux', machine=platform.machine(),
                supported=sum(row['supported'] for row in records),
                missing_required=sum(row['required'] and not row['supported'] for row in records),
                missing_optional=sum(not row['required'] and not row['supported'] for row in records),
                target_registered_required=sum(
                    row['required'] and row['target_registered'] for row in records),
                target_registered_optional=sum(
                    not row['required'] and row['target_registered'] for row in records),
                imports=records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--readelf', default='llvm-readelf')
    comparison = parser.add_mutually_exclusive_group()
    comparison.add_argument('--host-providers', action='store_true',
                            help='compile and query local Linux providers; diagnostic only')
    comparison.add_argument('--providers', type=Path, help='JSON: target-verified list of {name, version, library} objects')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = inventory(args.elf, args.readelf)
    if args.host_providers:
        host = host_provider_coverage(report['imports'])
        report['host_provider_coverage'] = host
        print(f"Linux {host['machine']} providers: {host['supported']} supported, "
              f"{host['missing_required']} required and {host['missing_optional']} optional missing "
              f"({host['target_registered_required']} required and "
              f"{host['target_registered_optional']} optional target-registered, "
              f"not exercised on hardware)")
    if args.providers:
        supplied = json.loads(args.providers.read_text())
        (report['missing_required'], report['missing_required_distinct']) = (
            compare_provider_manifest(report['imports'], supplied))
        report['missing_required_import_entries'] = len(report['missing_required'])
        report['missing_required_distinct_count'] = len(report['missing_required_distinct'])
        report['provider_manifest_sha256'] = hashlib.sha256(args.providers.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"{report['required_symbols']} required, {report['optional_symbols']} optional symbols; "
          f"{len(report['svc_zero_candidates'])} executable-segment SVC #0 matches; "
          f"{len(report['svc_text_candidates']) if report['svc_text_candidates'] is not None else 'unknown'} .text matches")
    if args.providers and report['missing_required']:
        print(f"Missing {report['missing_required_distinct_count']} distinct required ABI keys "
              f"across {report['missing_required_import_entries']} import entries:")
        for item in report['missing_required_distinct']:
            print(f"  {item['name']} @ {item['version'] or 'unversioned'} "
                  f"from {item['library'] or 'unspecified'}")
        raise SystemExit(1)


if __name__ == '__main__':
    main()
