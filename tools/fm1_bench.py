#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Build txfm1, Textura's bench firmware for the FM-1 (bench/fm1/txfm1.c): the engines timed in the audio interrupt.
Adapted from Frodhes' tools/build.py (the update loader, the image, the package; the Rhodes checks dropped).

  tools/fm1_bench.py [--eng-opt -Os|-O2] [--id FM-1_959] [--app] [--no-twins]

Core 1's twins (TX_C1, engines/tx_engine.h): every engine file with TX_RAM code is built a second time as
o/NAME_c1.o, its TX_RAM functions renamed NAME_c1 and placed in XIP (twin_headers), so core 1 never fetches code from
RAM while core 0 does; the link keeps only the twins core 1 calls, and twin_check fails the build if a twin keeps flash
code of its own (a second copy: that function wants TX_SHARE). --no-twins builds without them (an A/B).

--app builds the real app instead (firmware/src/textura.c), to measure its true flash and RAM: build/fm1/textura-app.{bin,elf,map} and textura-app.fwsc. It is also
linked once with app.ld's XIP, RAM and POOL made large (a measuring link, never an image): the true sizes of every
section and every object over 1 KB (build/fm1/textura-app-objects.txt), so an app that does not fit can still be read
object by object (--measure: that report only, no image).

The device side of the platform (firmware/hal, crt0.S, app.ld, firmware/loader, usb.c, lcd.c, ota.c ...: Frodhes'
v0.9.5 files, kept in this repository with their headers) is copied with firmware/src, firmware/gen, the engines and
the package tools into build/fm1/tree, and built there. usb.c names the device, its MIDI port and audio interface
"Textura FM-1" (USB_NAME, checked at the stage) and the app's USB product id is USB_PID. The JieLi toolchain
(.local/jieli/toolchain, Linux x86-64; make setup) runs in one linux/amd64 Docker container; the package needs three
files of the JieLi AC79 SDK (.local/fw-AC79_AIoT_SDK; make setup). Outputs in build/fm1: txfm1.bin, txfm1.elf, txfm1.map, stack.txt, textura-bench.fwsc.
Nothing here is committed: build/ and .local/ are ignored.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "fm1"
TREE = OUT / "tree"
APP_XIP = 0x02000120
LOADER_LOAD = 0x01C0A800
RAM_AT, RAM_LEN = 0x01C06000, 0x70800     # app.ld's RAM: the debug gap, .data / .bss and the pool, one region
NOINIT_LEN = 0x3D50
LOADER_NAME = b"usb_hid_ota.bin"
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
STAGE = ["firmware/hal", "firmware/crt0.S", "firmware/app.ld", "firmware/loader", "firmware/src", "firmware/gen",
         "tools/fm1pkg_make.py", "tools/lz4blk.py", "tools/fm1_install.py"]


# Textura's own USB identity in the app and the bench (never the update loader: FELUCCA_LOADER keeps 1209:0002, "Felucca
# Update", Hügelton Instruments). usb.c (Frodhes', its header kept) carries the strings; the stage checks them:
#   STR2, the product: the device, its MIDI port (the jacks are named by it) and its audio interface: "Textura FM-1"
#     ("FM-1" in it: the web installers, thefirmwarehub.com's among them, probe only ports whose name has "fm-1",
#     "felucca", "sloop", ...; without it a unit on Textura could not be given another firmware from there)
#   STR1, the manufacturer: "Textura" (the loader's own branch keeps Frodhes' string)
# (string descriptors 2 + 2 x 12 = 26 and 2 + 2 x 7 = 16 bytes.) The product id is set at the compile (FELUCCA_USB_PID, usb.c's #ifndef):
# a computer keeps a MIDI device's name per VID:PID, so Textura needs its own, apart from Frodhes' units (0x0005 /
# 0x0006 ...). USB_PID is that one place: 0x000A is interim, a pid.codes test PID (1209:0001 .. 0010 are its test
# PIDs; 0001, 0002, 0004 .. 0009 are Felucca's / the loader's / Dinghy's / Frodhes'); the PID granted for Textura
# replaces it here. A test PID is not unique (anyone may use it): in-house tests only, never a released image.
USB_NAME = "Textura FM-1"
USB_MAKER = "Textura"
USB_PID = 0x000A
def str_desc(text):
    """a USB string descriptor's C initialiser (UTF-16LE, ASCII only): bLength, 3, the characters"""
    if not text.isascii() or not 0 < len(text) <= 126:
        raise SystemExit(f"fm1_bench: USB string {text!r}: 1 .. 126 ASCII characters")
    return "{" + ", ".join([str(2 + 2 * len(text)), "3"] + [f"'{c}', 0" for c in text]) + "}"


def usb_check(path):
    """usb.c's app strings are USB_NAME (STR2) and USB_MAKER (STR1), each once, the loader's kept apart"""
    src = path.read_text(encoding="utf-8")
    for what, text in (("STR2", USB_NAME), ("STR1", USB_MAKER)):
        if src.count(f"static const uint8_t {what}[] = {str_desc(text)};") != 1:
            raise SystemExit(f"fm1_bench: {path}: {what} is not {text!r} once (the USB name; tools/fm1_bench.py)")
    if src.count("#ifdef FELUCCA_LOADER") < 2:
        raise SystemExit(f"fm1_bench: {path}: the loader's own strings (FELUCCA_LOADER) not found")


def stage(app=False):
    if TREE.exists():
        shutil.rmtree(TREE)
    TREE.mkdir(parents=True)
    for rel in STAGE:
        src, dst = ROOT / rel, TREE / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        (shutil.copytree if src.is_dir() else shutil.copy2)(src, dst)
    usb_check(TREE / "firmware" / "src" / "usb.c")
    shutil.copytree(ROOT / "engines", TREE / "engines")
    shutil.copytree(ROOT / "bench" / "fm1", TREE / "bench")
    if not app:
        shutil.copy2(OUT / "gen" / "fm1_cfg.h", TREE / "bench" / "fm1_cfg.h")


HOT_MARK = "/* C1_HOT */"
HOT_FILE = ROOT / "firmware" / "c1_hot.txt"


def hot_names():
    """firmware/c1_hot.txt's functions, in order (tools/c1hot.py writes it: make fw-c1hot)"""
    return [n for n in (ln.split("#")[0].strip() for ln in HOT_FILE.read_text().splitlines()) if n]


def hot_check(mapf, names):
    """every hot name placed, in one block at .text's start, in the list's order (a stale name: the build fails)"""
    at = {}
    for m in re.finditer(r"^ \.text\.([A-Za-z_0-9.]+)\s*\n?\s*(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", mapf.read_text(), re.M):
        at.setdefault(m.group(1), (int(m.group(2), 16), int(m.group(3), 16)))
    gone = [n for n in names if n not in at]
    if gone:
        raise SystemExit(f"fm1_bench: c1_hot.txt: {len(gone)} name(s) not in the image (stale: make fw-c1hot): {gone[:6]}")
    a = [at[n][0] for n in names]
    end = max(x + at[n][1] for n, x in zip(names, a))
    if a != sorted(a) or min(a) > APP_XIP + 0x100:
        raise SystemExit("fm1_bench: c1_hot.txt: the hot code is not placed in order at .text's start")
    print(f"  hot    {len(names)} functions at {min(a):#x} .. {end:#x} ({end - min(a)} B, firmware/c1_hot.txt)")


def measure_report(o, out):
    """the measuring link (--app): sections and objects; True when the app does not fit app.ld (or --measure)"""
    sec = {}
    for ln in (o / "measure_sections.txt").read_text().splitlines():
        p = ln.split()
        if len(p) >= 4 and p[0].isdigit() and p[1].startswith("."):
            sec[p[1]] = int(p[2], 16)
    rows = []
    for ln in (o / "measure_syms.txt").read_text().splitlines():
        m = re.match(r"^[0-9a-f]+\s.*?(\.[\w.]+)\s+([0-9a-f]{8})\s+(\S+)$", ln)   # (llvm-objdump: no O flag)
        if m and int(m.group(2), 16) >= 1024:
            rows.append((int(m.group(2), 16), m.group(1), m.group(3)))
    rows.sort(reverse=True)
    (out / "textura-app-objects.txt").write_text("".join(f"{s:8d}  {sc:12s} {n}\n" for s, sc, n in rows))
    for f in ("measure.map",):
        shutil.copy2(o / f, out / f"textura-app-{f}")
    g = lambda n: sec.get(n, 0)
    ram = g(".data") + g(".bss")
    lim = ((".text", g(".text"), 0x8DFBC - g(".ram_text") - g(".data")), (".ram_text", g(".ram_text"), 0x6000),
           (".data+.bss+.pool", ram + g(".pool"), RAM_LEN), (".noinit", g(".noinit"), NOINIT_LEN))
    print("  measuring link (app.ld's regions made large):")
    for n, v, l in lim:
        print(f"    {n:11s} {v:8d} B of {l:7d}  " + (f"OVER by {v - l}" if v > l else f"free {l - v}"))
    print("  largest objects (build/fm1/textura-app-objects.txt):")
    for s, sc, n in rows[:30]:
        print(f"    {s:8d}  {sc:12s} {n}")
    fit = (o / "fit.txt").read_text() if (o / "fit.txt").exists() else ""
    if "overflow" in fit or "will not fit" in fit:
        print("  app.ld: does not fit:")
        for ln in fit.splitlines():
            print("    " + ln.split("ld: ")[-1])
        return True
    return not (o / "textura.elf").exists()


def twin_headers(o, eng):
    """core 1's twins (TX_C1, engines/tx_engine.h): from each engine object's symbols, the files with TX_RAM code and,
    for each, o/twin_NAME.h: every TX_RAM global (of all the engines) renamed NAME_c1, this file's other globals weak
    (the originals are kept, its twin's copies drop out). Returns the files and their TX_RAM functions."""
    ram, glob = {}, {}
    for n in eng:
        b = n[:-2]
        ram[b], glob[b] = set(), set()
        for ln in (o / f"{b}.sym").read_text().splitlines():
            p = ln.split()
            if len(p) < 4 or not re.fullmatch(r"[0-9a-f]{8}", p[0]) or "*UND*" in ln or "*ABS*" in ln:
                continue
            scope, sec, name = ln[9], p[-3], p[-1]
            if sec.startswith(".ram_text"):
                ram[b].add((name, scope == "g"))
            elif scope == "g":
                glob[b].add(name)
    twins = sorted(b for b in ram if ram[b])
    ren = sorted(nm for b in twins for nm, g in ram[b] if g)
    for b in twins:
        (o / f"twin_{b}.h").write_text(
            f"/* generated by tools/fm1_bench.py: engines/{b}.c built again as core 1's twin (TX_TWIN_BUILD) */\n"
            + "".join(f"#define {nm} {nm}_c1\n" for nm in ren)
            + "".join(f"#pragma weak {nm}\n" for nm in sorted(glob[b])))
    return twins, {b: {nm for nm, _ in ram[b]} for b in twins}


def twin_check(mapf, ramfn):
    """the link's sections from the twins (o/NAME_c1.o): code only of TX_RAM functions (anything else would be a second
    XIP copy of flash code); prints their sizes"""
    m = mapf.read_text()
    m = m[m.find("Linker script and memory map"):]
    code = data = 0
    bad = []
    for sec, size, obj in re.findall(r"^ (\.[^\s]+)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+o/(\w+)_c1\.o$", m, re.M):
        size = int(size, 16)
        if not size:
            continue
        if sec.startswith(".text"):
            fn = re.sub(r"_c1$", "", sec[len(".text."):])
            if fn not in ramfn.get(obj, ()):
                bad.append(f"{obj}: {sec}")
            code += size
        else:
            data += size
    print(f"  twins  core 1's XIP copies of the TX_RAM code: {code} B of code, {data} B of tables")
    if bad:
        raise SystemExit("fm1_bench: the twins keep flash code of their own (make it TX_SHARE): " + ", ".join(bad))


def docker(script):
    tc = (ROOT / ".local" / "jieli" / "toolchain").resolve()
    if not (tc / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit(f"fm1_bench: no toolchain in {tc}")
    (TREE / "build.sh").write_text(script)
    # 4 CPUs, or fewer where the machine has fewer (a private repository's CI runner has 2; docker refuses more)
    cpus = os.environ.get("JIELI_DOCKER_CPUS", str(min(4, os.cpu_count() or 1)))
    cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "--cpus", cpus,
           "-v", f"{TREE}:/work", "-v", f"{tc}:/opt/jieli:ro", "-w", "/work", "debian:bookworm-slim", "sh", "build.sh"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stderr.write(r.stdout[-6000:] + r.stderr[-6000:])
    if r.returncode:
        raise SystemExit("fm1_bench: the toolchain failed")


def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image, lz4blk):
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eng-opt", help="the engines' optimisation: the app -Os (on the FM-1 it costs ~1 %% CPU and saves "
                    "~41 KB of flash: the A0 probe), the bench -O2 (its timings comparable); the app's own code stays -Os")
    ap.add_argument("--id", default="FM-1_959", help="the package identity (fm1_install: MODEL_digits)")
    ap.add_argument("--no-stage", action="store_true", help="reuse build/fm1/tree")
    ap.add_argument("--release", metavar="X.Y.Z", help="with --app: a release: VERSION X.Y.Z in the app (else dev), and "
                    "build/fm1/textura-app-X.Y.Z.fwsc with its textura-app-X.Y.Z.json (version, commit, sha256 ...)")
    ap.add_argument("--notes", metavar="FILE", help="with --release: the json's changes, FILE's first paragraph")
    ap.add_argument("--app", action="store_true", help="the real app (firmware/src/textura.c), not the bench")
    ap.add_argument("--measure", action="store_true", help="with --app: the measuring link's report only, no image")
    ap.add_argument("--define", action="append", default=[], help="-D NAME for bench/txfm1.c (with --app: for "
                    "firmware/src/textura.c, e.g. TX_NO_C1_YIELD)")
    ap.add_argument("--no-hot", action="store_true", help="with --app: no hot-code layout (firmware/c1_hot.txt not "
                    "placed: the link's own order, as before it; an A/B on the device)")
    ap.add_argument("--no-twins", action="store_true", help="no core-1 twins (TX_NO_TWINS: core 1 runs the TX_RAM code "
                    "from RAM too, as before them; an A/B of their cost on the device)")
    a = ap.parse_args()
    if any(not re.fullmatch(r"[A-Z][A-Z0-9_]*", d) for d in a.define):
        raise SystemExit("--define: NAME (A-Z, 0-9, _)")
    if not re.fullmatch(r"FM-1_\d{3}", a.id):
        raise SystemExit("--id: FM-1_NNN")
    if a.release and (not a.app or not re.fullmatch(r"\d+\.\d+\.\d+", a.release)):
        raise SystemExit("--release: X.Y.Z, with --app")
    name = "textura" if a.app else "txfm1"
    out = "textura-app" if a.app else "txfm1"
    if not a.app and not (OUT / "gen" / "fm1_cfg.h").exists():
        raise SystemExit("fm1_bench: no build/fm1/gen/fm1_cfg.h (make fm1-gen)")
    if not a.no_stage:
        stage(a.app)
    hot = hot_names() if a.app and not a.no_hot else []
    if not a.no_stage:                         # the hot code at .text's start, in c1_hot.txt's order (app.ld's marker)
        ldp = TREE / "firmware" / "app.ld"
        ld = ldp.read_text()
        if ld.count(HOT_MARK) != 1:
            raise SystemExit(f"fm1_bench: app.ld has no {HOT_MARK} (the hot code's place)")
        ldp.write_text(ld.replace(HOT_MARK, "".join(f" *(.text.{n})" for n in hot)))
    if a.app:                                  # the measuring link's map: the same sections, room for all of them
        ld = (TREE / "firmware" / "app.ld").read_text()
        big, k = re.subn(r"(XIP\s+\(rx\)\s*:\s*ORIGIN\s*=\s*0x02000120,\s*LENGTH\s*=\s*)0x8DFBC", r"\g<1>16M", ld)
        big, k2 = re.subn(r"RAM\s+\(rwx\)\s*:\s*ORIGIN\s*=\s*0x01C06000,\s*LENGTH\s*=\s*0x70800",
                          "RAM    (rwx) : ORIGIN = 0x10000000, LENGTH = 16M", big)
        if (k, k2) != (1, 1):
            raise SystemExit("fm1_bench: app.ld's XIP / RAM lines not found (the measuring link needs them)")
        (TREE / "firmware" / "app_measure.ld").write_text(big)
    sdk = ROOT / ".local" / "fw-AC79_AIoT_SDK"
    inc = "-Ifirmware/hal -Ifirmware/src -Ifirmware/gen -Iengines -Ibench"
    cf = " ".join(CFLAGS) + (" -DTX_NO_TWINS" if a.no_twins else "")
    eng = sorted(p.name for p in (TREE / "engines").glob("*.c"))
    cc = "/opt/jieli/pi32v2/bin/clang -target pi32v2"
    lines = ["set -e", "mkdir -p o ldr",
             # the update loader (Frodhes': a generic ota-FM-1_900 identity, the screen named TEXTURA)
             f"{cc} -c firmware/loader/crt0_ldr.S -o ldr/crt0_ldr.o",
             f'{cc} {cf} -Ifirmware/hal -Ifirmware/src -DLDR_NAME=\\"TEXTURA\\" -DLDR_COLOUR=0xF800u -DLDR_FOOT=\\"\\" '
             "-c firmware/loader/loader.c -o ldr/loader.o",
             "/opt/jieli/pi32v2/bin/ld --gc-sections -e _start -T firmware/loader/loader.ld ldr/crt0_ldr.o ldr/loader.o -o ldr/loader.elf",
             "/opt/jieli/common/bin/objcopy -O binary -j .text ldr/loader.elf ldr/loader.bin",
             "/opt/jieli/common/bin/objdump -h ldr/loader.elf > ldr/loader.h.txt",
             # the app
             f"{cc} -c firmware/crt0.S -o o/crt0.o", f"{cc} -c firmware/hal/fm1_vec.S -o o/fm1_vec.o",
             f"{cc} -c firmware/hal/fm1_isr.S -o o/fm1_isr.o",
             (f'{cc} {cf} {inc} -DFELUCCA_ID=\\"{a.id}\\" -DFELUCCA_USB_PID={USB_PID:#06x} -DAPP_VER=\\"{a.release or "dev"}\\" '
              + "".join(f"-D{d} " for d in a.define) +
              "-c firmware/src/textura.c -o o/textura.o"
              if a.app else
              f'{cc} {cf} {inc} -DFM1_PROF_TICK -DFELUCCA_ID=\\"{a.id}\\" -DFELUCCA_USB_PID={USB_PID:#06x} -DAPP_VER=\\"bench\\" '
              + "".join(f"-D{d} " for d in a.define) +
              "-c bench/txfm1.c -o o/txfm1.o")]
    if not a.eng_opt:
        a.eng_opt = "-Os" if a.app else "-O2"
    eflags = (f"{a.eng_opt} -ffunction-sections -fno-builtin -Wall -Wno-unused-function -Iengines -Ibench"
              + (" -DTX_NO_TWINS" if a.no_twins else "") + (" -DTX_APP" if a.app else ""))   # (TX_APP: no player or
    #                                                    bench text in the app's flash, tx_engine.h TX_TEXT)
    if not a.app:
        lines.append(f"{cc} {eflags} -c bench/txfm1_core.c -o o/txfm1_core.o")
    for i in range(0, len(eng), 8):           # (8 at a time: the emulated compiler is slow)
        part = eng[i:i + 8]
        lines.append(" ".join(f"{cc} {eflags} -c engines/{n} -o o/{n[:-2]}.o &" for n in part) + " wait")
    lines += [f"/opt/jieli/common/bin/objdump -t o/{n[:-2]}.o > o/{n[:-2]}.sym" for n in eng]
    docker("\n".join(lines) + "\n")
    twins, ramfn = ([], {}) if a.no_twins else twin_headers(TREE / "o", eng)   # core 1's twins (TX_C1): the TX_RAM
    #                                                                             files built again, in XIP
    lines = ["set -e"]
    tflags = f"{eflags} -fdata-sections -DTX_TWIN_BUILD"
    for i in range(0, len(twins), 8):
        part = twins[i:i + 8]
        lines.append(" ".join(f"{cc} {tflags} -include o/twin_{b}.h -c engines/{b}.c -o o/{b}_c1.o &" for b in part)
                     + " wait")
    objs = (f"o/crt0.o o/fm1_vec.o o/fm1_isr.o o/{name}.o " + ("" if a.app else "o/txfm1_core.o ")
            + " ".join(f"o/{n[:-2]}.o" for n in eng) + "".join(f" o/{b}_c1.o" for b in twins))
    if a.app:
        ldc = "/opt/jieli/pi32v2/bin/ld --gc-sections -e _start"
        lines += [f"{ldc} -T firmware/app_measure.ld -Map=o/measure.map {objs} -o o/measure.elf",
                  "/opt/jieli/common/bin/objdump -h o/measure.elf > o/measure_sections.txt",
                  "/opt/jieli/common/bin/objdump -t o/measure.elf > o/measure_syms.txt"]
        if a.measure:
            lines.append("exit 0")
        else:
            lines.append(f"{ldc} -T firmware/app.ld -Map=o/{name}.map {objs} -o o/{name}.elf > o/fit.txt 2>&1 "
                         "|| { cat o/fit.txt; echo DOES-NOT-FIT; exit 0; }")
    lines += [f"/opt/jieli/pi32v2/bin/ld --gc-sections -e _start -T firmware/app.ld -Map=o/{name}.map {objs} -o o/{name}.elf",
              f"for s in text data ram_text; do rm -f o/$s.bin; /opt/jieli/common/bin/objcopy -O binary -j .$s o/{name}.elf o/$s.bin; done",
              f"/opt/jieli/common/bin/objdump -t o/{name}.elf > o/syms.txt",
              f"/opt/jieli/common/bin/objdump -h o/{name}.elf > o/sections.txt",
              "cat o/*.su > o/stack.txt 2>/dev/null || true"]
    docker("\n".join(lines) + "\n")
    if a.app and measure_report(TREE / "o", OUT):
        return 0 if a.measure else 1

    if not a.no_twins:
        twin_check(TREE / "o" / f"{name}.map", ramfn)
    if hot:
        hot_check(TREE / "o" / f"{name}.map", hot)
    sys.path.insert(0, str(TREE / "tools"))
    import fm1pkg_make  # noqa: E402
    import lz4blk  # noqa: E402
    fm1pkg_make.SDK = sdk
    # the loader
    for ln in (TREE / "ldr" / "loader.h.txt").read_text().splitlines():
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    limg = (TREE / "ldr" / "loader.bin").read_bytes()
    if not limg or len(limg) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    ota = ldr_wrap(limg, lz4blk)
    # the app image
    syms = (TREE / "o" / "syms.txt").read_text()

    def sym(name):
        m = re.search(r"^([0-9a-f]+) .*\s" + re.escape(name) + r"$", syms, re.M)
        return int(m.group(1), 16) if m else 0
    o = TREE / "o"
    img = bytearray((o / "text.bin").read_bytes())
    for sect, lname in (("ram_text.bin", "_rt_load"), ("data.bin", "_data_load")):
        load = sym(lname)
        blob = (o / sect).read_bytes() if (o / sect).exists() else b""
        if blob:
            if load % 4 or load - APP_XIP < len(img):
                raise SystemExit(f"{lname} {load:#x}: misaligned or overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img)) + blob
    img += b"\xff" * (-len(img) % 4)
    errors = []
    if sym("_start") != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
    if len(img) > fm1pkg_make.APP_SLOT:
        errors.append(f"image {len(img)} B exceeds the app slot {fm1pkg_make.APP_SLOT} B")
    ram = sym("_bss_end") - RAM_AT
    pool = sym("_pool_end") - sym("_pool_start")
    if sym("_pool_end") - RAM_AT > RAM_LEN:
        errors.append(f"RAM .data+.bss+.pool {sym('_pool_end') - RAM_AT} B > {RAM_LEN} B")
    # the stack frames (-fstack-usage), largest first
    frames = []
    for ln in (o / "stack.txt").read_text().splitlines():
        p = ln.split("\t")
        if len(p) >= 2 and p[1].isdigit():
            frames.append((int(p[1]), p[0]))
    frames.sort(reverse=True)
    for d in ("elf", "map"):
        shutil.copy2(o / f"{name}.{d}", OUT / f"{out}.{d}")
    shutil.copy2(o / "stack.txt", OUT / "stack.txt")
    (OUT / f"{out}.bin").write_bytes(img)
    print(f"  image  {len(img)} B of {fm1pkg_make.APP_SLOT} ({100 * len(img) // fm1pkg_make.APP_SLOT} %), "
          f"free {fm1pkg_make.APP_SLOT - len(img)} B")
    print(f"  RAM    .data+.bss {ram} B + pool {pool} B of {RAM_LEN} (free {RAM_LEN - ram - pool})")
    if frames:
        print("  stack  largest frames: " + ", ".join(f"{f[1].split(':')[-1]} {f[0]}" for f in frames[:8]))
    for e in errors:
        print("  FAIL  ", e)
    if errors:
        raise SystemExit("fm1_bench: checks failed")
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(bytes(img), fm1pkg_make.KEY), ota, a.id)
    pname = "textura-app.fwsc" if a.app else "textura-bench.fwsc"
    (OUT / pname).write_bytes(pkg)
    print(f"  loader {len(limg)} B, ota.bin {len(ota)} B")
    print(f"  package {OUT / pname} {len(pkg)} B, identity {a.id}, "
          f"sha256 {hashlib.sha256(pkg).hexdigest()}")
    if a.release:
        release_files(a, pkg)
    return 0


def release_files(a, pkg):
    """build/fm1/textura-app-X.Y.Z.fwsc and its json (the release's assets: .github/workflows/release.yml)"""
    import json
    import time
    git = lambda *c: subprocess.run(["git", "-C", str(ROOT), *c], capture_output=True, text=True).stdout.strip()
    notes = ""
    if a.notes:
        notes = " ".join(Path(a.notes).read_text(encoding="utf-8").strip().split("\n\n")[0].split())
    meta = {"name": "Textura", "version": a.release, "commit": git("rev-parse", "--short", "HEAD"),
            "date": git("log", "-1", "--format=%cs") or time.strftime("%Y-%m-%d"), "identity": a.id,
            "usb": f"{USB_NAME} 1209:{USB_PID:04X}", "beta": True, "sha256": hashlib.sha256(pkg).hexdigest(),
            "changes": notes}
    base = OUT / f"textura-app-{a.release}"
    Path(f"{base}.fwsc").write_bytes(pkg)
    Path(f"{base}.json").write_text(json.dumps(meta) + "\n")
    print(f"  release {base}.fwsc, {base}.json (VERSION {a.release})")


if __name__ == "__main__":
    sys.exit(main())
