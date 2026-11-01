#!/usr/bin/env python3
"""Inspect the USB descriptor bytes linked into the actual firmware ELF."""
from pathlib import Path
import struct
import sys
from elftools.elf.elffile import ELFFile


def check(build):
    with (build / "mosaico-webcam.elf").open("rb") as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name(".symtab").get_symbol_by_name("desc_fs_configuration")
        if not symbols or len(symbols) != 1:
            raise ValueError("Expected one linked UVC configuration descriptor")
        symbol = symbols[0]
        section = elf.get_section(symbol["st_shndx"])
        offset = symbol["st_value"] - section["sh_addr"]
        descriptor = section.data()[offset:offset + symbol["st_size"]]
    if struct.unpack_from("<H", descriptor, 2)[0] != len(descriptor):
        raise ValueError("USB configuration total length mismatch")
    frames = []
    cursor = 0
    while cursor < len(descriptor):
        length = descriptor[cursor]
        if length < 2 or cursor + length > len(descriptor):
            raise ValueError("Invalid USB descriptor boundary")
        item = descriptor[cursor:cursor + length]
        if item[1] == 0x24 and len(item) >= 38 and item[2] == 7:
            width, height = struct.unpack_from("<HH", item, 5)
            default = struct.unpack_from("<I", item, 21)[0]
            minimum, maximum, step = struct.unpack_from("<III", item, 26)
            frames.append((item[3], width, height, default, item[25], minimum, maximum, step))
        cursor += length
    expected = [(1, 640, 480, 400000, 0, 400000, 400000, 400000)]
    if frames != expected:
        raise ValueError(f"Unexpected advertised MJPEG modes: {frames!r}")
    print("Linked USB descriptor: one MJPEG mode, 640x480, fixed 25 fps")


if __name__ == "__main__":
    check(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "build")
