#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Build txfm1, Textura's bench firmware for the FM-1 (bench/fm1/txfm1.c): the engines timed in the audio interrupt.
Adapted from Frodhes' tools/build.py (the update loader, the image, the package; the Rhodes checks dropped).

  tools/fm1_bench.py [--frodhes ../frodhes] [--rev v0.9.5] [--eng-opt -O2] [--id FM-1_959] [--app]

--app builds the real app instead (firmware/src/textura.c, the whole of Textura's firmware/src and firmware/gen over
Frodhes'), to measure its true flash and RAM: build/fm1/textura-app.{bin,elf,map} and textura-app.fwsc. It is also
linked once with app.ld's XIP, RAM and POOL made large (a measuring link, never an image): the true sizes of every
section and every object over 1 KB (build/fm1/textura-app-objects.txt), so an app that does not fit can still be read
object by object (--measure: that report only, no image).

The device side of Dinghy's platform (hal/, crt0, app.ld, the loader, usb.c, lcd.c, ota.c ...) is read from Frodhes'
git at --rev (git archive: its checkout is never touched) into build/fm1/tree; Textura's own platform.c / gfx.c / fonts
and the engines go over it. The JieLi toolchain (.local/jieli/toolchain, Linux x86-64) runs in one linux/amd64
Docker container. Outputs in build/fm1: txfm1.bin, txfm1.elf, txfm1.map, stack.txt, textura-bench.fwsc.
Nothing here is committed: build/ and .local/ are ignored.
"""
import argparse
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "fm1"
TREE = OUT / "tree"
# Frodhes' repository: the sibling of the repo root, also from a worktree or a copy nested under build/ (the first
# ancestor with a frodhes/.git beside it)
FRODHES = next((d.parent / "frodhes" for d in (ROOT, *ROOT.parents) if (d.parent / "frodhes" / ".git").exists()),
               ROOT.parent / "frodhes")
APP_XIP = 0x02000120
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
FR_FILES = ["firmware/hal", "firmware/crt0.S", "firmware/app.ld", "firmware/loader", "firmware/src",
            "tools/fm1pkg_make.py", "tools/lz4blk.py", "tools/fm1_install.py"]
TX_OVER = ["firmware/src/platform.c", "firmware/src/gfx.c", "firmware/gen/felucca_font.h", "firmware/hal/fm1_cc.h",
           "firmware/hal/fm1_flash_map.h"]


def stage(frodhes, rev, app=False):
    if TREE.exists():
        shutil.rmtree(TREE)
    TREE.mkdir(parents=True)
    tar = subprocess.run(["git", "-C", str(frodhes), "archive", "--format=tar", rev, *FR_FILES], capture_output=True)
    if tar.returncode:
        raise SystemExit(f"fm1_bench: git archive {rev} in {frodhes} failed: {tar.stderr.decode().strip()}")
    with tarfile.open(fileobj=io.BytesIO(tar.stdout)) as t:
        t.extractall(TREE, filter="data")
    for rel in TX_OVER:
        (TREE / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, TREE / rel)
    if app:                                   # (the app: all of Textura's own sources over Frodhes')
        for d in ("firmware/src", "firmware/gen"):
            shutil.copytree(ROOT / d, TREE / d, dirs_exist_ok=True)
    shutil.copytree(ROOT / "engines", TREE / "engines")
    shutil.copytree(ROOT / "bench" / "fm1", TREE / "bench")
    if not app:
        shutil.copy2(OUT / "gen" / "fm1_cfg.h", TREE / "bench" / "fm1_cfg.h")


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
           (".data+.bss", ram, 96 * 1024), (".pool", g(".pool"), 0x54000))
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


def docker(script):
    tc = (ROOT / ".local" / "jieli" / "toolchain").resolve()
    if not (tc / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit(f"fm1_bench: no toolchain in {tc}")
    (TREE / "build.sh").write_text(script)
    cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "--cpus", os.environ.get("JIELI_DOCKER_CPUS", "4"),
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
    ap.add_argument("--frodhes", default=str(FRODHES))
    ap.add_argument("--rev", default="v0.9.5", help="Frodhes' revision for the platform's device files (platform.c's)")
    ap.add_argument("--eng-opt", default="-O2", help="the engines' optimisation (the app stays -Os, as Frodhes)")
    ap.add_argument("--id", default="FM-1_959", help="the package identity (fm1_install: MODEL_digits)")
    ap.add_argument("--no-stage", action="store_true", help="reuse build/fm1/tree")
    ap.add_argument("--app", action="store_true", help="the real app (firmware/src/textura.c), not the bench")
    ap.add_argument("--measure", action="store_true", help="with --app: the measuring link's report only, no image")
    a = ap.parse_args()
    if not re.fullmatch(r"FM-1_\d{3}", a.id):
        raise SystemExit("--id: FM-1_NNN")
    name = "textura" if a.app else "txfm1"
    out = "textura-app" if a.app else "txfm1"
    if not a.app and not (OUT / "gen" / "fm1_cfg.h").exists():
        raise SystemExit("fm1_bench: no build/fm1/gen/fm1_cfg.h (make fm1-gen)")
    if not a.no_stage:
        stage(Path(a.frodhes), a.rev, a.app)
    if a.app:                                  # the measuring link's map: the same sections, room for all of them
        ld = (TREE / "firmware" / "app.ld").read_text()
        big, k = re.subn(r"(XIP\s+\(rx\)\s*:\s*ORIGIN\s*=\s*0x02000120,\s*LENGTH\s*=\s*)0x8DFBC", r"\g<1>16M", ld)
        big, k2 = re.subn(r"RAM\s+\(rwx\)\s*:\s*ORIGIN\s*=\s*0x01C08000,\s*LENGTH\s*=\s*96K",
                          "RAM    (rwx) : ORIGIN = 0x10000000, LENGTH = 16M", big)
        big, k3 = re.subn(r"POOL\s+\(rw\)\s*:\s*ORIGIN\s*=\s*0x01C20000,\s*LENGTH\s*=\s*0x54000",
                          "POOL   (rw)  : ORIGIN = 0x11000000, LENGTH = 16M", big)
        if (k, k2, k3) != (1, 1, 1):
            raise SystemExit("fm1_bench: app.ld's XIP / RAM / POOL lines not found (the measuring link needs them)")
        (TREE / "firmware" / "app_measure.ld").write_text(big)
    sdk = ROOT / ".local" / "fw-AC79_AIoT_SDK"
    inc = "-Ifirmware/hal -Ifirmware/src -Ifirmware/gen -Iengines -Ibench"
    cf = " ".join(CFLAGS)
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
             (f'{cc} {cf} {inc} -DFELUCCA_ID=\\"{a.id}\\" -DAPP_VER=\\"dev\\" -c firmware/src/textura.c -o o/textura.o'
              if a.app else
              f'{cc} {cf} {inc} -DFM1_PROF_TICK -DFELUCCA_ID=\\"{a.id}\\" -DAPP_VER=\\"bench\\" '
              "-c bench/txfm1.c -o o/txfm1.o")]
    eflags = f"{a.eng_opt} -ffunction-sections -fno-builtin -Wall -Wno-unused-function -Iengines -Ibench"
    if not a.app:
        lines.append(f"{cc} {eflags} -c bench/txfm1_core.c -o o/txfm1_core.o")
    for i in range(0, len(eng), 8):           # (8 at a time: the emulated compiler is slow)
        part = eng[i:i + 8]
        lines.append(" ".join(f"{cc} {eflags} -c engines/{n} -o o/{n[:-2]}.o &" for n in part) + " wait")
    objs = (f"o/crt0.o o/fm1_vec.o o/fm1_isr.o o/{name}.o " + ("" if a.app else "o/txfm1_core.o ")
            + " ".join(f"o/{n[:-2]}.o" for n in eng))
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
    log = docker("\n".join(lines) + "\n")
    if a.app and measure_report(TREE / "o", OUT):
        return 0 if a.measure else 1

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
    ram = sym("_bss_end") - 0x01C08000
    pool = sym("_pool_end") - 0x01C20000
    if ram > 96 * 1024:
        errors.append(f"RAM .data+.bss {ram} B > 96 KiB")
    if pool > 0x54000:
        errors.append(f"pool {pool} B > {0x54000} B")
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
    print(f"  RAM    .data+.bss {ram} B of 98304; pool {pool} B of {0x54000} (free {0x54000 - pool})")
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
