# SPDX-License-Identifier: GPL-3.0-only
"""The power cycle's third run: the settings record cut to the 12 bytes of the firmwares before the MIDI IN
channels (scenes.c SET_OLD_SZ), its length and CRC set to match, so the boot reads a record as an older firmware left it.
Usage: settings_old.py FLASH_FILE"""
import struct
import sys
import zlib

path = sys.argv[1]
img = bytearray(open(path, "rb").read())
magic, n, _ = struct.unpack_from("<III", img, 0)
if magic != 0x59474E44 or n != 20:
    sys.exit(f"settings_old: no 20-byte settings record in {path} (magic {magic:#x}, length {n})")
rec = bytes(img[12:12 + 12])
struct.pack_into("<III", img, 0, magic, 12, zlib.crc32(rec) & 0xFFFFFFFF)
img[24:12 + n] = b"\xff" * (n - 12)
open(path, "wb").write(img)
