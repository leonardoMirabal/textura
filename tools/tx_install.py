#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Install a Textura package (build/fm1/textura-app.fwsc, textura-bench.fwsc) on the FM-1: Frodhes' installer
(tools/fm1_install.py at the rev tools/fm1_bench.py builds from, the same M-UPGRADE SysEx update) with Textura's port
choice (tools/tx_ports.py) and the unit pinned by its USB location:

  tools/tx_install.py PACKAGE.fwsc [--port NAME] [--location 0xNNNNNNNN] [--yes] [--force]
  tools/tx_install.py --info [--query] [--port NAME] [--location 0xNNNNNNNN]

The FM-1 is looked for under "Textura" first, then the update loader's names (a unit left in update mode); a port
named "Frodhes" is never probed unless --port names it, and never while a "Textura" port is present. A unit still on
Textura's old firmware (named "Frodhes") takes --port Frodhes for its first update.

One unit through both phases. Every unit's update loader takes the same name ("Felucca Update"), so after the switch
a name cannot say which unit is ours. The unit is pinned by its USB location (macOS ioreg's locationID) before the
switch: --location 0x... names it (its MIDI ports are then taken by CoreMIDI's USBLocationID, whatever their name,
and opened by index: two ports of one name told apart), else the location of the USB device behind the chosen port
(refused when two USB devices answer to that name). The waits for the loader and for the restarted firmware then take
only the ports at that location, whatever they are called: --port names the running firmware, the loader is followed
there under its own name. Without a location (no ioreg / CoreMIDI) a wait refuses when more than one port could be the
unit (exit 7): nothing is written to a unit that cannot be told apart. --info lists every FM-1 on USB with its
location, USB name, MIDI ports and state, from ioreg and CoreMIDI only (nothing sent); --query also asks each unit's
identity, never a loader's unless --location names it (another session may be writing it).

The installer is tools/fm1_install.py (Frodhes' v0.9.5 installer, kept with its header). Needs mido with python-rtmidi
(.local/venv; make setup).
"""
import argparse
import importlib.util
import queue
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))                  # (python -I: the tools' own helper)
import tx_ports  # noqa: E402

ROOT = HERE.parent
INSTALLER = HERE / "fm1_install.py"            # (Frodhes' v0.9.5 installer, kept here with its header)
AMBIGUOUS = 7                                  # exit: more than one unit could be the target


def installer_path():
    return INSTALLER


class RtLink:
    """one unit's MIDI in / out by python-rtmidi's port index (CoreMIDI's endpoint index at its USB location): two
    ports of one name ("Felucca Update" on two units) are told apart; the same interface as the installer's MidoLink"""

    def __init__(self, m, backend, loc, i_in, i_out, name):
        import rtmidi
        self.m, self.backend, self.loc, self.name = m, backend, loc, name
        self.q = queue.Queue()
        self.inp, self.out = rtmidi.MidiIn(), rtmidi.MidiOut()
        if self.inp.get_ports()[i_in:i_in + 1] != [name] or i_out >= len(self.out.get_ports()):
            raise OSError(f"the MIDI ports moved: input {i_in} is not {name!r} any more")
        self.inp.ignore_types(sysex=False, timing=True, active_sense=True)
        self.inp.set_callback(self._rx)
        self.inp.open_port(i_in)
        try:
            self.out.open_port(i_out)
        except Exception:
            self.inp.close_port()
            raise
        time.sleep(m.DELAY["open"])            # CoreMIDI may drop a message sent just after opening

    def _rx(self, ev, data=None):
        msg = ev[0]
        if msg and msg[0] == 0xF0:
            self.q.put(bytes(msg))

    @property
    def lost(self):
        ports = self.backend.ports_at(self.loc)
        return not ports or self.name not in [n for _, n in ports[0]]

    def send(self, pkt):
        try:
            self.out.send_message(list(pkt))
            return True
        except Exception:
            return False

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        for p in (self.inp, self.out):
            try:
                p.close_port()
            except Exception:
                pass


def load(path=None, location=None, query=False):
    """Frodhes' installer module, its port choice made Textura's and the unit pinned by its USB location"""
    spec = importlib.util.spec_from_file_location("fm1_install", path or installer_path())
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    also = r"textura|" + m.PORT_RE.pattern
    m.PORT_RE = re.compile(also, re.I)
    m.EXIT["ambiguous"] = AMBIGUOUS
    m.LOCATION = location
    m.QUERY = query
    U = m.Updater

    class TxBackend(m.MidoBackend):
        """the computer's MIDI (mido), its USB devices (ioreg) and each one's ports (CoreMIDI)"""

        @staticmethod
        def usb_devices():
            return tx_ports.usb_devices()

        @staticmethod
        def ports_at(loc):
            return tx_ports.midi_at(loc)

        @staticmethod
        def raw_inputs():
            import rtmidi                       # (mido's list drops a second port of the same name)
            return list(rtmidi.MidiIn().get_ports())

        def open_at(self, loc, i_in, i_out, name):
            return RtLink(m, self, loc, i_in, i_out, name)

    m.TxBackend = TxBackend
    orig_init = U.__init__

    def init(self, backend, port=None):
        orig_init(self, backend, port)
        self.location = m.LOCATION             # --location: where the unit must be (asserted before anything is sent)
        self.loc = None                        # the unit's USB location once pinned: the waits take only ports there

    def ambiguous(msg):
        return m.InstallError("ambiguous", msg)

    def usb(self):
        f = getattr(self.backend, "usb_devices", None)
        return f() if f else None

    def at(self, loc):
        f = getattr(self.backend, "ports_at", None)
        return f(loc) if f and loc is not None else None

    def raw(self):
        f = getattr(self.backend, "raw_inputs", None)
        return f() if f else self.backend.input_names()

    def named_frodhes(self):
        return bool(self.port) and tx_ports.OLD in self.port.lower()

    def candidates(self, only_port):
        names = self.backend.input_names()
        named = tx_ports.pick(names, self.port) if self.port else []
        if self.port and only_port:
            return named
        mine = self.port and "frodhes" not in self.port.lower()   # (a second FM-1 on Frodhes is never ours: the
        return named + [n for n in tx_ports.pick(names, None, also)  # loader's port may differ, Frodhes' never)
                        if n not in named and not (mine and "frodhes" in n.lower())]

    def pin(self, name):
        """the USB location of the device behind the port `name` (ioreg's product name); refused when two FM-1s
        answer to that name. None when the computer has no USB list or no device of that name (then the waits refuse
        whenever two ports could be the unit)"""
        devs = self.usb()
        if devs is None:
            return None
        hit = [(n, l) for n, l in tx_ports.usb_match(name, devs) if tx_ports.fm1_usb(n)]
        if len(hit) > 1:
            where = ", ".join(f"{n!r} at 0x{l:08x}" for n, l in hit)
            raise ambiguous(f"{len(hit)} FM-1s answer to the port name {name!r} ({where}): give the unit's "
                            "--location (tools/tx_install.py --info lists them)")
        self.loc = hit[0][1] if hit else None
        return self.loc

    def find_at(self, loc, want, first):
        """the unit at USB location loc: its ports by CoreMIDI's location, any name, opened by index. first: the
        unit named by --location (asserted: an FM-1 there, its firmware's reply an FM-1's), else a wait's look"""
        ports = self.at(loc)
        if ports is None:
            raise m.InstallError("usage", "--location: the MIDI ports' USB locations cannot be read on this computer "
                                          "(macOS CoreMIDI)")
        prod = [n for n, l in self.usb() or () if l == loc]
        if len(prod) != 1 or not tx_ports.fm1_usb(prod[0]):
            if first:
                raise m.InstallError("notfound", f"--location 0x{loc:08x}: the USB device there is "
                                                 f"{prod[0] if prod else 'nothing'!r}, not an FM-1")
            return None
        if tx_ports.OLD in prod[0].lower() and not self.named_frodhes():
            if first:
                raise m.InstallError("notfound", f"--location 0x{loc:08x}: a Frodhes unit there; if it is Textura's "
                                                 "FM-1 on its old firmware, add --port Frodhes")
            return None
        ins = [(i, n) for i, n in ports[0] if not (first and self.port) or self.port.lower() in n.lower()]
        if not ins:
            if first:
                raise m.InstallError("notfound", f"--location 0x{loc:08x}: no MIDI input there"
                                                 + (f" named {self.port!r}" if self.port else ""))
            return None
        if len(ins) > 1:
            raise ambiguous(f"{len(ins)} MIDI inputs at 0x{loc:08x} ({', '.join(n for _, n in ins)}): "
                            "name the one to use with --port NAME")
        i_in, name = ins[0]
        outs = [(i, n) for i, n in ports[1] if n == name] or (ports[1] if len(ports[1]) == 1 else [])
        if len(outs) != 1:
            if first:
                raise m.InstallError("notfound", f"--location 0x{loc:08x}: no MIDI output for {name!r} there")
            return None
        opener = getattr(self.backend, "open_at", None)
        try:
            if opener:
                link = opener(loc, i_in, outs[0][0], name)
            elif self.raw().count(name) == 1:
                link = self.backend.open(name, outs[0][1])
            else:
                raise ambiguous(f"two MIDI ports are named {name!r} and this MIDI backend opens them by name only")
        except m.InstallError:
            raise
        except Exception:
            return None
        if first:
            self.loc = loc
        ident = m.handshake(link, 2)
        if ident and first and not ident.loader and ident.model != "FM-1":
            link.close()
            raise m.InstallError("notfound", f"--location 0x{loc:08x}: the unit there answers {ident.text!r}, "
                                             "not an FM-1")
        if ident and (want is None or want(ident)):
            return m.Device(link, ident, name)
        link.close()
        return None

    def find(self, want=None, only_port=True):
        """the unit: by --location, else the first matching port that answers the handshake (and want(identity)),
        its USB location pinned before anything is sent; the waits (only_port False) only at that location"""
        if only_port and self.location is not None:
            return self.find_at(self.location, want, True)
        if not only_port and self.loc is not None and self.at(self.loc) is not None:
            return self.find_at(self.loc, want, False)
        names = self.candidates(only_port)
        raw = self.raw()
        if not only_port:                      # (no location: a wait takes a port only when it is the only one)
            could = [n for n in raw if n in names]
            if len(could) > 1:
                raise ambiguous(f"{len(could)} ports could be the unit ({', '.join(could)}) and its USB location is "
                                "not known: nothing written by this wait; run again with --location (--info lists "
                                "the units; one left in update mode finishes then)")
        outs = self.backend.output_names()
        for name in names:
            if raw.count(name) > 1:
                raise ambiguous(f"two MIDI ports are named {name!r}: they cannot be told apart by name; give the "
                                "unit's --location (tools/tx_install.py --info lists them)")
            out = m.pair_output(name, outs)
            if not out:
                continue
            if only_port:
                self.pin(name)                 # (refuses before anything is sent when two FM-1s share the name)
            try:
                link = self.backend.open(name, out)
            except Exception:
                continue
            ident = m.handshake(link, 2)
            if ident and (want is None or want(ident)):
                return m.Device(link, ident, name)
            link.close()
        return None

    U.__init__, U.usb, U.at, U.raw, U.named_frodhes = init, usb, at, raw, named_frodhes
    U.candidates, U.pin, U.find_at, U.find = candidates, pin, find_at, find

    orig_run = m.run

    def run(a, backend, out, ask):
        return info(m, m.Updater(backend, a.port), out, m.QUERY) if a.info else orig_run(a, backend, out, ask)

    m.run = run
    return m


LOADER = re.compile(r"update|ota-", re.I)       # an update loader's USB / port name ("Felucca Update")


def info(m, up, out, query=False):
    """every FM-1 on USB: its location, USB name, MIDI ports and state, read from ioreg and CoreMIDI only (nothing
    sent). query: each unit's identity asked too, never a loader's unless --location names it (another session may
    be writing it) and never a Frodhes unit's unless --port names it. Without the USB lists: the FM-1-like ports by
    name (query: the first running one's identity)"""
    devs = up.usb()
    if devs is None or getattr(up.backend, "ports_at", None) is None:
        names = up.candidates(False)
        if not names:
            raise m.not_found(up)
        for n in names:
            print(f"port: {n}" + ("  [update loader]" if LOADER.search(n) else "")
                  + "  (USB location unknown on this computer)", file=out)
        if query:
            run = [n for n in names if not LOADER.search(n)]
            save = up.candidates
            up.candidates = lambda only_port, _r=run: _r
            try:
                dev = up.find() if run else None
            finally:
                up.candidates = save
            if dev:
                dev.link.close()
                print(f"{dev.id.text}  [{'update loader' if dev.id.loader else 'running'}]  port: {dev.name}",
                      file=out)
        return 0
    units = [(n, l) for n, l in devs if tx_ports.fm1_usb(n)]
    if up.location is not None:
        units = [(n, l) for n, l in units if l == up.location]
    if not units:
        where = f" at 0x{up.location:08x}" if up.location is not None else ""
        raise m.InstallError("notfound", f"no FM-1 on USB{where} (USB devices: "
                                         f"{', '.join(n for n, _ in devs) or 'none'})")
    for n, l in units:
        ports = up.at(l) or ([], [])
        names = list(dict.fromkeys(p for _, p in ports[0]))
        loader = bool(LOADER.search(n) or any(LOADER.search(p) for p in names))
        state = "update loader" if loader else "running"
        ident = ""
        if not query:
            pass
        elif tx_ports.OLD in n.lower() and not up.named_frodhes():
            ident = "  identity not asked (a Frodhes unit: --port Frodhes)"
        elif loader and up.location != l:
            ident = "  identity not asked (a loader: only with --location)"
        else:
            save = up.port
            up.port = None                     # (every unit, not the one --port names)
            try:
                dev = up.find_at(l, None, False)
            except m.InstallError as e:
                dev, ident = None, f"  identity not asked ({e})"
            else:
                ident = "  no identity reply"
            finally:
                up.port = save
            if dev:
                dev.link.close()
                state, ident = "update loader" if dev.id.loader else "running", f"  {dev.id.text}"
        print(f"0x{l:08x}  {n!r:22}  ports: {', '.join(names) or '-'}  [{state}]{ident}", file=out)
    return 0


def main(argv=None, backend=None, out=sys.stdout, ask=None, path=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--location", type=tx_ports.parse_loc)
    ap.add_argument("--port")
    ap.add_argument("--query", action="store_true")
    try:
        mine, rest = ap.parse_known_args(argv)
    except SystemExit:
        print("tx_install: --location takes the unit's USB locationID (0x14200000; --info lists them)",
              file=sys.stderr)
        return 2
    if "-h" in rest or "--help" in rest:
        print(__doc__, file=out)
        return 0
    if mine.query and "--info" not in rest:
        print("tx_install: --query goes with --info", file=sys.stderr)
        return 2
    m = load(path, mine.location, mine.query)
    if backend is None:
        try:
            backend = m.TxBackend()
        except ImportError:
            print("error: needs mido and python-rtmidi (.local/venv)", file=sys.stderr)
            return 2
    kw = {"ask": ask} if ask else {}
    rc = m.main(rest, backend, out, **kw)
    if rc == m.EXIT.get("notfound", 3) and mine.location is None:
        try:
            print(f"tx_install: {tx_ports.why_none(backend.input_names(), mine.port)}", file=sys.stderr)
        except Exception:                       # noqa: BLE001 (the hint only)
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
