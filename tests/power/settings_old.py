# SPDX-License-Identifier: GPL-3.0-only
"""The power cycle's older settings records: the record cut to SIZE bytes (12: the firmwares before SCALE and the MIDI
IN channels, scenes.c SET_OLD_SZ; 20: before Frodhes' SCALE page, SET_MID_SZ; 24: before OUTPUT, SET_PREV_SZ; 26: before
its binaural AMOUNT and HEAD SIZE, SET_OUT_SZ), its length and CRC set to match, so the
boot reads a record as an older firmware left it (the record saved is 256 bytes: set_store.c's, the
settings first, zero-padded). With L1 NF instead of a size: the whole record, its layout set back to 1 (the scenes
as F001 .. F100 then the user's, scenes.c SET_LAYOUT) and its last scene moved there from a layout of NF factory
scenes, as a firmware with 100 presets left it. With RND: the whole record, its spare byte (byte 19) set as a build
that kept RANDOM's ticks there left it (0x80 and the four ticks; the firmware now writes 0 and ignores it).
Usage: settings_old.py FLASH_FILE [SIZE | L1 NF | RND]"""
import struct
import sys
import zlib

path = sys.argv[1]
img = bytearray(open(path, "rb").read())
magic, n, _ = struct.unpack_from("<III", img, 0)
if len(sys.argv) > 3 and sys.argv[2] == "L1":
    nf = int(sys.argv[3])
    if magic != 0x54535854 or n != 256 or img[12 + 11] != 2 or img[12 + 1] <= nf:
        sys.exit(f"settings_old: no layout 2 settings record with a user scene loaded last in {path}")
    img[12 + 11] = 1
    img[12 + 1] = 100 + img[12 + 1] - nf
    struct.pack_into("<III", img, 0, magic, n, zlib.crc32(bytes(img[12:12 + n])) & 0xFFFFFFFF)
    open(path, "wb").write(img)
    sys.exit(0)
if len(sys.argv) > 2 and sys.argv[2] == "RND":
    if magic != 0x54535854 or n != 256 or img[12 + 19] != 0:
        sys.exit(f"settings_old: no settings record with a spare byte at 0 in {path}")
    img[12 + 19] = 0x8F
    struct.pack_into("<III", img, 0, magic, n, zlib.crc32(bytes(img[12:12 + n])) & 0xFFFFFFFF)
    open(path, "wb").write(img)
    sys.exit(0)
size = int(sys.argv[2]) if len(sys.argv) > 2 else 12
if magic != 0x54535854 or n != 256 or size not in (12, 20, 24, 26):
    sys.exit(f"settings_old: no 256-byte settings record in {path} (magic {magic:#x}, length {n}), or size {size}")
rec = bytes(img[12:12 + size])
struct.pack_into("<III", img, 0, magic, size, zlib.crc32(rec) & 0xFFFFFFFF)
img[12 + size:12 + n] = b"\xff" * (n - size)
open(path, "wb").write(img)
