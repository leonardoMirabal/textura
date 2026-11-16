#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The FM-1's port choice (tools/tx_ports.py, tools/tx_install.py) and its USB name (firmware/src/usb.c, checked as
tools/fm1_bench.py does), no device: "Textura" first, "Frodhes" only when named and never beside a Textura port; the
installer's candidates the same (tools/fm1_install.py)."""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import tx_ports  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += not ok


def mod(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


ALSO = r"fm-1|txfm1|felucca|dinghy"
both = ["IAC Driver Bus 1", "Frodhes", "Textura", "MOTU M6"]
old = ["IAC Driver Bus 1", "Frodhes"]
check(tx_ports.pick(both) == ["Textura"], "Textura and Frodhes: Textura only")
check(tx_ports.pick(both, "Frodhes") == [], "--port Frodhes beside a Textura port: nothing")
check("never used beside" in tx_ports.why_none(both, "Frodhes"), "... and why")
check(tx_ports.pick(old) == [], "Frodhes alone, not named: nothing")
check("--port Frodhes" in tx_ports.why_none(old), "... and the hint")
check(tx_ports.pick(old, "frodhes") == ["Frodhes"], "Frodhes alone, named: taken (the first update)")
check(tx_ports.pick(["Felucca Update", "Textura"], None, ALSO) == ["Textura", "Felucca Update"], "Textura before the loader")
check(tx_ports.pick(["Frodhes", "Felucca Update"], None, ALSO) == ["Felucca Update"], "the loader, never Frodhes")
check(tx_ports.pick(["MOTU M6", "IAC Driver Bus 1"]) == [], "other gear: nothing")

b = mod("fm1_bench")
check(b.str_desc("Textura") == "{16, 3, 'T', 0, 'e', 0, 'x', 0, 't', 0, 'u', 0, 'r', 0, 'a', 0}", "STR2 Textura: 16 bytes")
usb = (ROOT / "firmware" / "src" / "usb.c").read_text(encoding="utf-8")
check(usb.count(f"static const uint8_t STR2[] = {b.str_desc(b.USB_NAME)};") == 1 and "fm-1" in b.USB_NAME.lower(),
      "usb.c: the app's STR2 is USB_NAME (a name with FM-1: the web installers find it)")
check(usb.count(f"static const uint8_t STR1[] = {b.str_desc(b.USB_MAKER)};") == 1, "usb.c: the app's STR1 is Textura")
check(usb.count("#ifdef FELUCCA_LOADER") >= 2 and "'F', 0, 'e', 0, 'l', 0" in usb and "0xFC, 0" in usb,
      "usb.c: the loader's own strings kept (Felucca Update, H\u00fcgelton)")
check("'F', 0, 'r', 0, 'o', 0, 'd', 0" not in usb, "usb.c: no Frodhes string")
b.usb_check(ROOT / "firmware" / "src" / "usb.c")
check(True, "fm1_bench.usb_check passes on usb.c")
check(0x0001 <= b.USB_PID <= 0x0010 and b.USB_PID not in (1, 2, 4, 5, 6, 7, 8, 9) or b.USB_PID > 0x1FFF,
      f"USB_PID {b.USB_PID:#06x}: a free pid.codes test PID (interim) or a granted one, never Frodhes' / the loader's")


class Fake:
    def __init__(self, names):
        self.names = names

    def input_names(self):
        return list(self.names)


try:
    inst = mod("tx_install")
    m = inst.load()
except SystemExit as e:
    print(f"skip  installer ({e})")
else:
    U = m.Updater
    check(U(Fake(both)).candidates(True) == ["Textura"], "installer: Textura, never Frodhes beside it")
    check(U(Fake(old + ["Felucca Update"])).candidates(True) == ["Felucca Update"], "installer: the loader, not Frodhes")
    check(U(Fake(old)).candidates(True) == [], "installer: Frodhes alone, not named: nothing")
    check(U(Fake(old), "Frodhes").candidates(True) == ["Frodhes"], "installer: --port Frodhes, the first update")
    check(U(Fake(["Frodhes", "Felucca Update"]), "Frodhes").candidates(False) == ["Frodhes", "Felucca Update"],
          "installer: waiting after --port Frodhes: it, then the loader")
    check(U(Fake(both + ["Felucca Update"]), "Frodhes").candidates(False) == ["Textura", "Felucca Update"],
          "installer: back as Textura: Frodhes dropped")
    check(bool(m.PORT_RE.search("Textura")) and bool(m.PORT_RE.search("ota-FM-1_900")), "installer: PORT_RE")
sys.exit(1 if fails else 0)
