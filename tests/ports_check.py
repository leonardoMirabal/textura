#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The FM-1's port choice (tools/tx_ports.py, tools/tx_install.py) and its USB name (tools/fm1_bench.py), no device:
"Textura" first, "Frodhes" only when named and never beside a Textura port; the installer's candidates the same; the
product string descriptor 16 bytes. The installer part needs Frodhes' tools/fm1_install.py (the staged tree or its git:
skipped, and said so, without either)."""
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
str1 = ("static const uint8_t STR1[] = {42, 3, 'H', 0, 0xFC, 0, 'g', 0, 'e', 0,\n"
        "                               'n', 0, 't', 0, 's', 0};\n")
src = (str1 + "#ifdef FELUCCA_LOADER\nstatic const uint8_t STR2[] = {30, 3, 'F', 0};\n#else\n"
       "static const uint8_t STR2[] = {16, 3, 'F', 0, 'r', 0, 'o', 0, 'd', 0, 'h', 0, 'e', 0, 's', 0};   /* Frodhes: the\n"
       " * product */\n#endif\n")
tmp = ROOT / "build" / "fw" / "ports_check_usb.c"
tmp.parent.mkdir(parents=True, exist_ok=True)
tmp.write_text(src)
b.usb_name(tmp)
out = tmp.read_text()
check(b.str_desc(b.USB_NAME) in out and "fm-1" in b.USB_NAME.lower() and "'F', 0, 'r'" not in out
      and "{30, 3, 'F', 0}" in out, "usb_name: the app's STR2 replaced (a name with FM-1: the web installers find it), the loader's kept")
tex = "static const uint8_t STR1[] = {16, 3, 'T', 0, 'e', 0, 'x', 0, 't', 0, 'u', 0, 'r', 0, 'a', 0};"
check(out.startswith("#ifdef FELUCCA_LOADER\n" + str1 + "#else\n" + tex), "usb_name: STR1 Textura, the loader's kept")
check(0x0001 <= b.USB_PID <= 0x0010 and b.USB_PID not in (1, 2, 4, 5, 6, 7, 8, 9) or b.USB_PID > 0x1FFF,
      f"USB_PID {b.USB_PID:#06x}: a free pid.codes test PID (interim) or a granted one, never Frodhes' / the loader's")
try:
    b.usb_name(tmp)
    check(False, "usb_name: a second pass fails loudly")
except SystemExit:
    check(True, "usb_name: a second pass fails loudly")
tmp.unlink()


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
