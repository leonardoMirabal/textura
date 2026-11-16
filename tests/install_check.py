#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/tx_install.py with two FM-1s on one computer, no device: a simulated USB / MIDI world (units with a USB
location, a product name, MIDI ports and a running firmware or an update loader that ask for reads like the real one).

  - two units in the loader at once, both "Felucca Update": refused without --location, nothing sent; with
    --location only that unit is written;
  - --port names the running firmware: the same unit followed into its loader (now one of two "Felucca Update"),
    the other untouched; without CoreMIDI's locations that wait refuses instead;
  - two units of one name: refused before anything is sent; --location on a non-FM-1: refused;
  - --info lists every FM-1 with its location, USB name and state, nothing sent; --query asks identities, never a
    loader's unless --location names it.
Uses tools/fm1_install.py (Frodhes' v0.9.5 installer, kept here with its header)."""
import importlib.util
import io
import queue
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += not ok


spec = importlib.util.spec_from_file_location("tx_install", ROOT / "tools" / "tx_install.py")
TI = importlib.util.module_from_spec(spec)
spec.loader.exec_module(TI)
try:
    PATH = TI.installer_path()
except SystemExit as e:
    print(f"skip  installer ({e})")
    sys.exit(0)
I = TI.load(PATH)                              # (the module's helpers: pack7, the SysEx constants)
FAST = dict(open=0, start=0.01, reply=0, loader=0.01, reboot=0.01, retry=0.02, poll=0.05, hs=0.1,
            idle_check=0.4, idle_write=0.4, wait_loader=1.5, wait_reboot=1.5)


def package(product="FM-1_959", size=0x2000):
    """a package the installer accepts: header, one file (flash.bin) with its CRC, the loader mark, the identity
    markers interleaved"""
    img = bytearray((i * 7) & 0xFF for i in range(size))
    img[0x1800:0x1810] = I.LOADER_MARK
    data = bytes(img[0x400:])
    e = bytearray(0x50)
    e[4:6] = I._crc16(data).to_bytes(2, "little")
    e[8:12], e[12:16] = (0x400).to_bytes(4, "little"), len(data).to_bytes(4, "little")
    e[0x40:0x49] = b"flash.bin"
    lst = I._dec(bytes(e))
    hdr = bytearray(0x40)
    hdr[8:10] = (1).to_bytes(2, "little")
    hdr[2:4] = I._crc16(lst).to_bytes(2, "little")
    hdr[0:2] = I._crc16(bytes(hdr[2:0x40])).to_bytes(2, "little")
    img[0:0x40], img[0x40:0x90] = I._dec(bytes(hdr)), lst
    raw = bytearray()
    for i in range(I.BLOCKS):
        mk = (ord(product[i]) + i + 1) & 0xFF if i < len(product) else 0x7D
        raw += img[i * I.KEEP:(i + 1) * I.KEEP] + bytes([mk])
    return bytes(raw + img[I.BLOCKS * I.KEEP:])


class Unit:
    """one FM-1: its USB location and product, its MIDI port, its identity; a running firmware or the loader"""

    def __init__(self, world, loc, product, port, ident):
        self.world, self.loc = world, loc
        self.sent, self.upgrades, self.links, self.gen = [], 0, [], 0
        self.boot(product, port, ident)

    def boot(self, product, port, ident):
        self.gen += 1
        self.product, self.port, self.ident, self.waiting, self.queue = product, port, ident, None, []

    def tx(self, pkt):
        for ln in self.links:
            if ln.gen == self.gen and not ln.closed:
                ln.q.put(bytes(pkt))

    def rx(self, d):
        self.sent.append(d)
        if d == I.HS_QUERY:
            t = self.ident.encode()
            self.tx(b"\xF0" + I.pack7(bytes([0, 0x59, 0x11, 0, 0, 0]) + t + bytes(28 - len(t))) + b"\xF7")
        elif d == I.UPGRADE:
            self.upgrades += 1
            self.queue = ([(k * 512, 512) for k in range(4)] + [(I.FINISH_WRITE, 8)] if self.ident.startswith("ota-")
                          else [(0, 64), (0x40, 160), (I.FINISH_CHECK, 8)])
            self.next()
        elif self.waiting:
            addr, _ = self.waiting
            self.waiting = None
            if addr == I.FINISH_CHECK:          # the switch: the loader, its own USB name and port
                threading.Timer(0.05, self.boot, ("Felucca Update", "Felucca Update", "ota-FM-1_900")).start()
            elif addr == I.FINISH_WRITE:        # the restart: the new firmware
                threading.Timer(0.05, self.boot, ("Textura FM-1", "Textura", self.world.product)).start()
            else:
                self.next()

    def next(self):
        if self.queue:
            self.waiting = addr, n = self.queue.pop(0)
            u = bytearray([0, 0x59, 0x30, 0, 0, 0, 0]) + addr.to_bytes(4, "little") + bytes([n & 0xFF, n >> 8, 0])
            u.append(~sum(u[6:14]) & 0xFF)
            self.tx(b"\xF0" + I.pack7(u) + b"\xF7")


class Link:
    def __init__(self, unit):
        self.unit, self.gen, self.q, self.closed, self.name = unit, unit.gen, queue.Queue(), False, unit.port
        unit.links.append(self)

    @property
    def lost(self):
        return self.gen != self.unit.gen

    def send(self, pkt):
        if self.lost:
            return False
        self.unit.rx(bytes(pkt))
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        self.closed = True


class World:
    """the computer: other gear, the units; mido's view (names, one per name), rtmidi's (indices, duplicates kept),
    ioreg's (product, location) and CoreMIDI's (the ports at a location). coremidi False: no locations for ports"""

    def __init__(self, units, coremidi=True, product="FM-1_959"):
        self.product = product
        self.units = [Unit(self, *u) for u in units]
        self.gear = [("M6", 0x03110000)]
        if not coremidi:
            self.ports_at = None
        self.opened = []

    def raw_inputs(self):
        return [g for g, _ in self.gear] + [u.port for u in self.units]

    def input_names(self):
        return list(dict.fromkeys(self.raw_inputs()))

    output_names = input_names

    def usb_devices(self):
        return list(self.gear) + [(u.product, u.loc) for u in self.units]

    def ports_at(self, loc):
        ins = [(i + len(self.gear), u.port) for i, u in enumerate(self.units) if u.loc == loc]
        ins += [(i, g) for i, (g, l) in enumerate(self.gear) if l == loc]
        return ins, list(ins)

    def open(self, in_name, out_name):             # (mido: by name, the first of that name)
        u = next((u for u in self.units if u.port == in_name), None)
        if u is None:
            raise IOError("no such port")
        self.opened.append(u.loc)
        return Link(u)

    def open_at(self, loc, i, o, name):            # (rtmidi: by index)
        u = self.units[i - len(self.gear)]
        if u.loc != loc or u.port != name:
            raise IOError("moved")
        self.opened.append(loc)
        return Link(u)


A, B = 0x00100000, 0x20130000
PKG = ROOT / "build" / "fw" / "install_check.fwsc"
PKG.parent.mkdir(parents=True, exist_ok=True)
PKG.write_bytes(package())


def run(world, *args):
    out, err = io.StringIO(), io.StringIO()
    old, sys.stderr = sys.stderr, err
    try:
        TI.load = (lambda *a, **k: _set(LOAD(*a, **k)))   # (each run its own module)
        rc = TI.main(list(args), backend=world, out=out, ask=lambda _p: True, path=PATH)
    finally:
        sys.stderr = old
        TI.load = LOAD
    return rc, out.getvalue() + err.getvalue()


def _set(m):
    m.DELAY.update(FAST)
    return m


LOAD = TI.load
loader = ("Felucca Update", "Felucca Update", "ota-FM-1_900")
running = ("Textura FM-1", "Textura", "FM-1_958")

w = World([(A, *loader), (B, *loader)])
rc, txt = run(w, str(PKG), "--yes")
check(rc == 7 and not any(u.upgrades for u in w.units), f"two loaders, no --location: refused (exit {rc}), nothing written")
check("--location" in txt, "... and the hint names --location")

w = World([(A, *loader), (B, *loader)])
rc, txt = run(w, str(PKG), "--yes", "--location", f"0x{B:08x}")
a, b = w.units
check(rc == 0 and b.upgrades == 1 and a.upgrades == 0 and not a.sent and b.ident == "FM-1_959",
      f"two loaders, --location B: only B written and back as FM-1_959 (exit {rc})")

w = World([(A, *running), (B, *loader)])
rc, txt = run(w, str(PKG), "--yes", "--port", "Textura")
a, b = w.units
check(rc == 0 and a.upgrades == 2 and not b.sent and a.ident == "FM-1_959",
      f"--port Textura: that unit followed into its loader (one of two 'Felucca Update'), B never touched (exit {rc})")

w = World([(A, *running), (B, *loader)])
rc, txt = run(w, str(PKG), "--yes", "--location", f"0x{A:08x}")
a, b = w.units
check(rc == 0 and a.upgrades == 2 and not b.sent, f"--location A, running: switched and written, B untouched (exit {rc})")

w = World([(A, *running), (B, *loader)], coremidi=False)
rc, txt = run(w, str(PKG), "--yes", "--port", "Textura")
a, b = w.units
check(rc == 7 and a.upgrades == 1 and b.upgrades == 0, f"no CoreMIDI locations: the wait refuses two loaders (exit {rc}), "
      "nothing written to either (A left in update mode)")

w = World([(A, *running), (B, *running)])
rc, txt = run(w, str(PKG), "--yes")
check(rc == 7 and not any(u.sent for u in w.units), f"two units named 'Textura': refused before anything is sent (exit {rc})")

w = World([(A, *running), (B, *running)])
rc, txt = run(w, str(PKG), "--yes", "--location", f"0x{B:08x}")
a, b = w.units
check(rc == 0 and not a.sent and b.ident == "FM-1_959", f"two units named 'Textura', --location B: B only (exit {rc})")

w = World([(A, *running)])
rc, txt = run(w, str(PKG), "--yes", "--location", "0x03110000")
check(rc == 3 and not w.units[0].sent and "not an FM-1" in txt, f"--location on the M6: refused (exit {rc})")

w = World([(A, *running)])
rc, txt = run(w, str(PKG), "--yes")
check(rc == 0 and w.units[0].ident == "FM-1_959", f"one unit, no --location: installed as before (exit {rc})")

FR = (0x30000000, "Frodhes FM-1", "Frodhes", "FM-1_957")
w = World([(A, *running), (B, *loader), FR])
rc, txt = run(w, "--info")
check(rc == 0 and "0x00100000" in txt and "[running]" in txt and "0x20130000" in txt and "[update loader]" in txt
      and "0x30000000" in txt and "M6" not in txt and not any(u.sent for u in w.units),
      "--info: every FM-1 with location, USB name, ports, state; nothing sent to any unit")
print(txt, end="")

w = World([(A, *running), (B, *loader), FR])
rc, txt = run(w, "--info", "--query")
a, b, f = w.units
check(rc == 0 and "FM-1_958" in txt and a.sent and not b.sent and not f.sent and "only with --location" in txt,
      "--info --query: the running unit asked; the loader and the Frodhes unit not")
print(txt, end="")

w = World([(A, *running), (B, *loader)])
rc, txt = run(w, "--info", "--query", "--location", f"0x{B:08x}")
a, b = w.units
check(rc == 0 and "ota-FM-1_900" in txt and b.sent and not a.sent and "0x00100000" not in txt,
      "--info --query --location B: that loader asked, nothing else")

w = World([(A, *running), (B, *loader)], coremidi=False)
rc, txt = run(w, "--info")
check(rc == 0 and "Felucca Update  [update loader]" in txt and not any(u.sent for u in w.units),
      "--info without CoreMIDI: the ports by name, nothing sent")

w = World([(A, *running), (B, *loader)], coremidi=False)
rc, txt = run(w, "--info", "--query")
a, b = w.units
check(rc == 0 and "FM-1_958" in txt and a.sent and not b.sent, "--info --query without CoreMIDI: the running unit only")

rc, txt = run(World([(A, *running)]), str(PKG), "--query")
check(rc == 2, "--query without --info: refused")
PKG.unlink()
sys.exit(1 if fails else 0)
