# SPDX-License-Identifier: GPL-3.0-only
"""The power cycle's older settings records: the record cut to SIZE bytes (12: the firmwares before SCALE and the MIDI
IN channels, scenes.c SET_OLD_SZ; 20: before Frodhes' SCALE page, SET_MID_SZ), its length and CRC set to match, so the
boot reads a record as an older firmware left it.
Usage: settings_old.py FLASH_FILE [SIZE]"""
import struct
import sys
import zlib

path = sys.argv[1]
size = int(sys.argv[2]) if len(sys.argv) > 2 else 12
img = bytearray(open(path, "rb").read())
magic, n, _ = struct.unpack_from("<III", img, 0)
if magic != 0x59474E44 or n != 24 or size not in (12, 20):
    sys.exit(f"settings_old: no 24-byte settings record in {path} (magic {magic:#x}, length {n}), or size {size}")
rec = bytes(img[12:12 + size])
struct.pack_into("<III", img, 0, magic, size, zlib.crc32(rec) & 0xFFFFFFFF)
img[12 + size:12 + n] = b"\xff" * (n - size)
open(path, "wb").write(img)
