# SPDX-License-Identifier: GPL-3.0-only
"""Which MIDI port or USB audio device is Textura's FM-1 (tools/fm1_timing.py, tools/hil_jack.py, tools/tx_install.py).

Textura's firmware names itself "Textura FM-1" on USB: the device, its MIDI port and its audio interface (the product
string; tools/fm1_bench.py writes it into the copied usb.c). Firmware built before that read "Frodhes" (Frodhes'
platform files, unchanged), and so does a Frodhes FM-1, which may be on the same computer and is not ours to drive:

  - a name with "Textura" comes first;
  - a "Frodhes" name is taken only when the caller names it (--port Frodhes: a unit still on the old firmware, for
    its first update), and never while a "Textura" name is present;
  - other names (`also`, a regular expression: the bench's own names, the update loader) come after the Textura ones.

Two FM-1s may share the computer, and every unit's update loader takes the same name ("Felucca Update"): a name cannot
tell them apart. A unit is then known by where it is plugged in, its USB locationID (macOS; the method of Frodhes'
tools/fm1_ports.py):

  usb_devices()        [(USB product name, locationID)] of the connected USB devices (ioreg), None off macOS
  usb_match(name, devs) the devices whose product name is the port name (or one starts with the other)
  fm1_usb(name)        the USB product is an FM-1's (Textura, the loader, the stock and older names)
  midi_at(loc)         ([(index, name)] MIDI sources, [(index, name)] destinations) of the USB device at loc
                       (CoreMIDI's USBLocationID; the index is python-rtmidi's port index), None off macOS
  parse_loc(text)      --location: 0x14200000 or its decimal
"""
import re
import subprocess
import sys

TEXTURA = "textura"
OLD = "frodhes"


def pick(names, port=None, also=None):
    """the names to try, best first (an empty list when none may be used)"""
    names = list(names)
    tex = [n for n in names if TEXTURA in n.lower()]
    if port:
        got = [n for n in names if port.lower() in n.lower()]
        got.sort(key=lambda n: TEXTURA not in n.lower())
    else:
        rx = re.compile(also, re.I) if also else None
        got = tex + [n for n in names if n not in tex and rx and rx.search(n) and OLD not in n.lower()]
    if tex:
        got = [n for n in got if OLD not in n.lower()]
    return got


def why_none(names, port=None):
    """the reason pick() gave nothing, for the caller's error"""
    if port and OLD in port.lower() and any(TEXTURA in n.lower() for n in names):
        return "a Textura port is present: a Frodhes port is never used beside it"
    if not port and any(OLD in n.lower() for n in names):
        return ("only a Frodhes port: if it is Textura's FM-1 on its old firmware, name it with --port Frodhes "
                "(a Frodhes FM-1 may be on this computer too)")
    return f"no Textura port among {names}"


FM1_USB = re.compile(r"textura|felucca|fm-1|frodhes|dinghy", re.I)   # an FM-1's USB product (any firmware, the loader)


def fm1_usb(name):
    return bool(name and FM1_USB.search(name))


def parse_loc(text):
    """--location: 0x14200000 or 337641472 (ioreg's locationID)"""
    return int(text, 0)


def parse_ioreg(txt):
    """[(USB product name, locationID)] from ioreg -p IOUSB -l -w0"""
    out = []
    for node in re.split(r"\n[ |]*\+-o ", txt)[1:]:
        name = re.search(r'"USB Product Name" = "([^"]*)"', node)
        loc = re.search(r'"locationID" = (\d+)', node)
        if name and loc:
            out.append((name.group(1), int(loc.group(1))))
    return out


def usb_devices():
    """[(USB product name, locationID)] of the connected USB devices (macOS ioreg, read only); None elsewhere"""
    if sys.platform != "darwin":
        return None
    try:
        txt = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True,
                             timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return parse_ioreg(txt)


def usb_match(name, devs):
    """the USB devices of the MIDI port `name`: the same product name, or one starting with the other ("Textura" /
    "Textura FM-1"), case aside"""
    n = name.lower().strip()
    return [(d, l) for d, l in devs
            if d.lower() == n or (len(n) >= 4 and (d.lower().startswith(n) or n.startswith(d.lower())))]


def midi_at(loc):
    """([(i, name)] sources, [(i, name)] destinations) of the USB device at locationID loc: CoreMIDI's objects read
    (nothing sent to any device); i is the endpoint's index, the same as python-rtmidi's port index. None when
    CoreMIDI is not there"""
    import ctypes
    import ctypes.util
    lib, cfl = ctypes.util.find_library("CoreMIDI"), ctypes.util.find_library("CoreFoundation")
    if sys.platform != "darwin" or not lib or not cfl:
        return None
    cm, cf = ctypes.cdll.LoadLibrary(lib), ctypes.cdll.LoadLibrary(cfl)
    u32, ul, vp = ctypes.c_uint32, ctypes.c_ulong, ctypes.c_void_p
    cf.CFStringCreateWithCString.restype, cf.CFStringCreateWithCString.argtypes = vp, [vp, ctypes.c_char_p, u32]
    cf.CFStringGetCString.argtypes = [vp, ctypes.c_char_p, ctypes.c_long, u32]
    cf.CFRelease.argtypes = [vp]
    for fn in ("MIDIGetSource", "MIDIGetDestination"):
        getattr(cm, fn).restype, getattr(cm, fn).argtypes = u32, [ul]
    cm.MIDIGetNumberOfSources.restype = cm.MIDIGetNumberOfDestinations.restype = ul
    cm.MIDIEndpointGetEntity.argtypes = cm.MIDIEntityGetDevice.argtypes = [u32, ctypes.POINTER(u32)]
    cm.MIDIObjectGetIntegerProperty.argtypes = [u32, vp, ctypes.POINTER(ctypes.c_int32)]
    cm.MIDIObjectGetStringProperty.argtypes = [u32, vp, ctypes.POINTER(vp)]
    utf8 = 0x08000100
    k_loc, k_name = (cf.CFStringCreateWithCString(None, k, utf8) for k in (b"USBLocationID", b"name"))

    def name(o):
        r = vp()
        if cm.MIDIObjectGetStringProperty(o, k_name, ctypes.byref(r)) or not r.value:
            return ""
        b = ctypes.create_string_buffer(512)
        cf.CFStringGetCString(r, b, 512, utf8)
        cf.CFRelease(r)
        return b.value.decode("utf-8", "replace")

    def at(n, get):
        out = []
        for i in range(n):
            e, ent, dev, v = get(i), u32(), u32(), ctypes.c_int32()
            if cm.MIDIEndpointGetEntity(e, ctypes.byref(ent)) or cm.MIDIEntityGetDevice(ent.value, ctypes.byref(dev)):
                continue
            if not cm.MIDIObjectGetIntegerProperty(dev.value, k_loc, ctypes.byref(v)) and (v.value & 0xFFFFFFFF) == loc:
                out.append((i, name(e) or name(dev.value)))
        return out
    return at(cm.MIDIGetNumberOfSources(), cm.MIDIGetSource), at(cm.MIDIGetNumberOfDestinations(), cm.MIDIGetDestination)
