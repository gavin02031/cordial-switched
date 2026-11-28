"""Decoder for Android packed relocations (APS2 / SHT_ANDROID_RELA).

Written for the 2026-11-19 syscall-audit correction (Revision 1 in
switch/docs/morrow-v099-syscall-audit.md): the audit originally assumed APS2
RELATIVE addends are the relocation slot contents, so a whole-file pointer
scan covered them. They are not: addends are stream-encoded accumulated
sleb128 deltas, so only a real decode enumerates relocation-derived pointers.

Encoding (bionic linker_reloc_iterators.h, oreo-r4):
  "APS2" magic, then sleb128 relocation_count, sleb128 initial r_offset,
  then relocation groups. Per group: group_size, group_flags, then optional
  grouped fields (offset delta if flag 2, r_info if flag 1, addend delta if
  flags 8|4). Per relocation: offset delta (if not grouped), r_info (if not
  grouped), addend delta (if flag 8 and not grouped). r_offset and r_addend
  accumulate across the stream; a group without flag 8 resets r_addend to 0.

Stdlib only. Usage: python3 aps2_relocs.py path/to/lib.so [--init-array]
"""

import struct
import sys

RELOCATION_GROUPED_BY_INFO = 0x1
RELOCATION_GROUPED_BY_OFFSET_DELTA = 0x2
RELOCATION_GROUPED_BY_ADDEND = 0x4
RELOCATION_GROUP_HAS_ADDEND = 0x8


def _sleb128(buf, pos):
    result = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated sleb128")
        byte = buf[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            break
    if shift < 64 and (byte & 0x40):
        result |= -(1 << shift)
    return result, pos


def decode_aps2(buf):
    """Decode an APS2 stream. Returns (relocations, groups, end_pos).

    relocations is a list of (r_offset, r_info, r_addend) in stream order.
    Raises ValueError if the stream is malformed or not consumed exactly.
    """
    if buf[:4] != b"APS2":
        raise ValueError("bad magic %r" % buf[:4])
    pos = 4
    count, pos = _sleb128(buf, pos)
    r_offset, pos = _sleb128(buf, pos)
    r_info = 0
    r_addend = 0
    relocs = []
    groups = 0
    while len(relocs) < count:
        groups += 1
        group_size, pos = _sleb128(buf, pos)
        group_flags, pos = _sleb128(buf, pos)
        if group_flags & RELOCATION_GROUPED_BY_OFFSET_DELTA:
            group_delta, pos = _sleb128(buf, pos)
        if group_flags & RELOCATION_GROUPED_BY_INFO:
            r_info, pos = _sleb128(buf, pos)
        if (group_flags & RELOCATION_GROUP_HAS_ADDEND) and (
            group_flags & RELOCATION_GROUPED_BY_ADDEND
        ):
            addend_delta, pos = _sleb128(buf, pos)
            r_addend += addend_delta
        elif not (group_flags & RELOCATION_GROUP_HAS_ADDEND):
            r_addend = 0
        for _ in range(group_size):
            if not (group_flags & RELOCATION_GROUPED_BY_OFFSET_DELTA):
                delta, pos = _sleb128(buf, pos)
                r_offset += delta
            else:
                r_offset += group_delta
            if not (group_flags & RELOCATION_GROUPED_BY_INFO):
                r_info, pos = _sleb128(buf, pos)
            if (group_flags & RELOCATION_GROUP_HAS_ADDEND) and not (
                group_flags & RELOCATION_GROUPED_BY_ADDEND
            ):
                addend_delta, pos = _sleb128(buf, pos)
                r_addend += addend_delta
            relocs.append((r_offset, r_info, r_addend))
    if pos != len(buf):
        raise ValueError(
            "stream not consumed exactly: pos=%d len=%d" % (pos, len(buf))
        )
    return relocs, groups, pos


def _sections(data):
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        secs.append(struct.unpack_from("<IIQQQQIIQQ", data, off))
    shstr = secs[e_shstrndx]

    def name(s):
        start = shstr[4] + s[0]
        end = data.index(b"\x00", start)
        return data[start:end].decode()

    return {
        name(s): dict(
            zip(
                ["name", "type", "flags", "addr", "offset", "size",
                 "link", "info", "align", "entsize"],
                s,
            )
        )
        for s in secs
    }


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    data = open(argv[1], "rb").read()
    secs = _sections(data)
    rela = secs[".rela.dyn"]
    if rela["type"] != 0x60000002:  # SHT_ANDROID_RELA
        sys.exit(".rela.dyn is not SHT_ANDROID_RELA (type %#x)" % rela["type"])
    buf = data[rela["offset"] : rela["offset"] + rela["size"]]
    relocs, groups, _ = decode_aps2(buf)
    print("relocations: %d, groups: %d (byte-exact)" % (len(relocs), groups))
    if "--init-array" in argv:
        ia = secs[".init_array"]
        lo, hi = ia["addr"], ia["addr"] + ia["size"]
        slots = {}
        for r_offset, r_info, r_addend in relocs:
            if r_info == 1027 and lo <= r_offset < hi:  # R_AARCH64_RELATIVE
                slots[(r_offset - lo) // 8] = r_addend
        expected = ia["size"] // 8
        print("init_array slots resolved: %d / %d" % (len(slots), expected))
        for i in sorted(slots):
            print("  ctor[%d] = %#x" % (i, slots[i]))
        if len(slots) != expected:
            sys.exit("init_array resolution incomplete")


if __name__ == "__main__":
    main(sys.argv)
