#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The device-only platform paths no host build runs (make fw-test), checked in their source: what each guards
against is said with its check. Exit 1 on a failed check."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += not ok


def src(name):
    return (ROOT / "firmware" / "src" / name).read_text(encoding="utf-8")


def body(text, head):
    """a function's body: from its head's line to the first column-0 closing brace"""
    m = re.search(r"^" + re.escape(head) + r"\n\{(.*?)^\}\n", text, re.S | re.M)
    return m.group(1) if m else ""


# USB RESCUE (recovery.c) drops the MIDI ring every step: nothing plays there, and a full ring makes usb.c hold EP1 OUT
# (NAK, ep1_take: fewer than EP1_ROOM slots free), so the installer's SysEx never arrives (Frodhes 1004e7bdc)
step = body(src("recovery.c"), "static void recovery_step(void)")
check(bool(re.search(r"while \(midi_get\(&st, &d1, &d2\)\)\s*(/\*.*?\*/\s*)?;", step, re.S)),
      "recovery: recovery_step drains the MIDI ring (a full ring held EP1 OUT: USB RESCUE took no installer)")
check(bool(re.search(r"if \(MQ - \(mi_w - mi_r\) < EP1_ROOM\)\n\s+return 0;", src("usb.c"))),
      "recovery: ... the ring's back-pressure on EP1 (what the drain is for) still there")

# the USB audio's EP4 poll (usb.c uac_service): sie_rd returns 0 when the SIE does not answer, and 0 read as TxPktRdy
# clear queued a new packet over the one still waiting (its 44 frames lost, uncounted). A timed-out INDEX write or
# TXCSR1 read is "still queued", counted in uac.sie_to (Frodhes 76404313d)
svc = body(src("usb.c"), "static void uac_service(void)")
m = re.search(r"sie_wr\(S_INDEX, 4\);(.*?)if \(csr & 0x01u\)", svc, re.S)
check(bool(m) and "usb.timeouts" in m.group(1) and "sie_to++" in m.group(1),
      "usb: a timed-out SIE access in the EP4 poll is still queued (sie_rd's 0 queued a packet over a waiting one)")

# the 48 kHz USB tap (usb.c uac_tap48): the 160 / 147 FIR's phases gain up to 1.67 on a loud high passage, so a signal
# the limiter kept at 24000 overshot full scale and was clamped flat; above UAC_KNEE (24576) the excess is halved before
# the clamp, both channels, nothing changes below it (Frodhes 9f896abeb)
usb = src("usb.c")
tap = body(usb, "static FR_RAM __attribute__((noinline)) void uac_tap48(const int32_t *out, uint32_t n)")
knee = re.search(r"#define UAC_KNEE (\d+)", usb)
ok = bool(knee) and int(knee.group(1)) == 24576
for ch in ("vl", "vr"):
    k = tap.find(f"if ((uint32_t)({ch} + UAC_KNEE) > 2u * UAC_KNEE)")
    c = tap.find(f"{ch} = {ch} < -32768 ? -32768 : {ch};")
    ok = ok and 0 <= k < c and f"{ch} = {ch} > 0 ? UAC_KNEE + (({ch} - UAC_KNEE) >> 1) : -UAC_KNEE - ((-{ch} - UAC_KNEE) >> 1);" in tap
check(ok, "usb: the 48 kHz tap's knee before its clamp (UAC_KNEE 24576, both channels: a loud passage clamped flat)")

# the update staging per unit (platform.c tx_ota_pick, store.h): 0xE0000 is in V16's SEQ64, so a unit that came from
# V16 (no 55 AA AA 55 at 0x93000, V15's VM start) stages at 0xA9000, in no region of V16's; a V15 unit at 0xE0000,
# where only a record left there is erased, never the stock data the sectors may hold. At power-on, before the screen
plat = src("platform.c")
pick = body(plat, "static void tx_ota_pick(void)")
check("st_read(0x93000u, m, 4)" in pick and "m[0] == 0x55u && m[1] == 0xAAu && m[2] == 0xAAu && m[3] == 0x55u" in pick and
      "tx_ota_area = TX_OTA_V15;" in pick and "if (rec) ota_unstage();" in pick and
      re.search(r"\} else\s+ota_boot_cleanup\(\);", pick) is not None,
      "ota: the staging picked per unit (V15's VM marker: 0xE0000, a record only erased; else V16's gap 0xA9000)")
check(re.search(r"static uint32_t tx_ota_area = TX_OTA_V16;.*?#include \"ota.c\"", plat, re.S) is not None and
      re.search(r"flash_ok = FL_JEDEC_OK\(FL_FAR\(fl_jedec_ram\)\(\)\);\n#endif\n#ifdef TX_OTA_V15\n\s+tx_ota_pick\(\);", plat)
      is not None, "ota: ... picked at power-on once the flash is known, V16's place until then (the USB rescue)")
store = src("store.h")
check("#define OTA_AREA tx_ota_area" in store and "#ifndef OTA_AREA" in src("ota.c"),
      "ota: ota.c stages at the picked place (store.h OTA_AREA), not its fixed 0xE0000")

sys.exit(1 if fails else 0)
