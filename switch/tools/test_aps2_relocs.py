"""Synthetic-stream tests for aps2_relocs.decode_aps2.

The real validation target (libroblox.so) is proprietary and not in the
repo, so these tests build small APS2 streams by hand and check the decoder
against them, including the header fields (count + initial offset) whose
omission caused the original audit mis-scan.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from aps2_relocs import (
    RELOCATION_GROUPED_BY_ADDEND,
    RELOCATION_GROUPED_BY_INFO,
    RELOCATION_GROUPED_BY_OFFSET_DELTA,
    RELOCATION_GROUP_HAS_ADDEND,
    decode_aps2,
)


def sleb128(value):
    out = bytearray()
    more = True
    while more:
        byte = value & 0x7F
        value >>= 7
        if (value == 0 and not (byte & 0x40)) or (value == -1 and (byte & 0x40)):
            more = False
        else:
            byte |= 0x80
        out.append(byte & 0xFF)
    return bytes(out)


def test_header_then_grouped_relative_run():
    # 3 relocations, initial offset 0x1000, one group with grouped delta 8,
    # grouped info (R_AARCH64_RELATIVE=1027), per-item addend deltas.
    buf = b"APS2"
    buf += sleb128(3)          # relocation count
    buf += sleb128(0x1000)     # initial r_offset
    flags = (
        RELOCATION_GROUPED_BY_OFFSET_DELTA
        | RELOCATION_GROUPED_BY_INFO
        | RELOCATION_GROUP_HAS_ADDEND
    )
    buf += sleb128(3) + sleb128(flags)
    buf += sleb128(8)          # group offset delta
    buf += sleb128(1027)       # group r_info
    buf += sleb128(0x200000)   # addend delta 1 (accumulates from 0)
    buf += sleb128(0x100)      # addend delta 2
    buf += sleb128(-0x40)      # addend delta 3
    relocs, groups, _ = decode_aps2(buf)
    assert groups == 1
    assert relocs == [
        (0x1008, 1027, 0x200000),
        (0x1010, 1027, 0x200100),
        (0x1018, 1027, 0x2000C0),
    ], relocs


def test_ungrouped_items_and_addend_reset():
    # Group 1: two relocations with per-item delta/info/addend.
    # Group 2: one relocation with no addend flag -> r_addend resets to 0.
    buf = b"APS2"
    buf += sleb128(3)
    buf += sleb128(0)
    buf += sleb128(2) + sleb128(RELOCATION_GROUP_HAS_ADDEND)
    buf += sleb128(0x100) + sleb128(1027) + sleb128(0x5000)
    buf += sleb128(0x20) + sleb128(1027) + sleb128(0x80)
    buf += sleb128(1) + sleb128(0)  # no flags: per-item delta+info, no addend
    buf += sleb128(0x10) + sleb128(1027)
    relocs, groups, _ = decode_aps2(buf)
    assert groups == 2
    assert relocs == [
        (0x100, 1027, 0x5000),
        (0x120, 1027, 0x5080),
        (0x130, 1027, 0),
    ], relocs


def test_truncated_stream_rejected():
    buf = b"APS2" + sleb128(5) + sleb128(0)  # declares 5, provides 0
    try:
        decode_aps2(buf)
    except ValueError:
        return
    raise AssertionError("truncated stream not rejected")


def test_trailing_garbage_rejected():
    buf = b"APS2" + sleb128(0) + sleb128(0) + b"\x00"  # extra byte
    try:
        decode_aps2(buf)
    except ValueError:
        return
    raise AssertionError("non-exact consumption not rejected")


if __name__ == "__main__":
    test_header_then_grouped_relative_run()
    test_ungrouped_items_and_addend_reset()
    test_truncated_stream_rejected()
    test_trailing_garbage_rejected()
    print("aps2_relocs tests passed")
