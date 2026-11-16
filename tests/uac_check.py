#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The USB audio IN endpoint's timing (make fw-test), from Frodhes 0.9.9: the EP4 poll follows the host's IN token
(usb.c FELUCCA_UAC_PHASE, on in platform.c) and each flash slice waits for the moment right after a packet (usb.c
uac_slice_ok, fm1_flash.h FL_SLICE_OK: FELUCCA_UAC_ALIGN). The endpoint holds one packet, so the next one must be
queued between the IN token that takes the last and the next frame's token; the 2 kHz poll at a fixed phase leaves
0..500 us of that frame unused, and an interrupts-off stretch (a flash slice) of ~520 us may lose a frame.

- the timing model (tests/uac/uac_timing.py) with tests/uac/uac_phase.py, the C logic step for step: the critical
  stretch, a flash slice with token movement, a stream start, a render, a drifting token, aligned slices;
- usb.c itself (tests/uac_seq_test.c, built with the firmware's FELUCCA_UAC_PHASE and once without it): no frame
  twice or lost uncounted (stalls, overruns, SIE time-outs), no token lost under 700 us stretches where the fixed
  poll loses them, the extra polls, the SIE accesses in the worst block, aligned slice trains, the save's longest
  poll gap, a controller reset.
Exit 1 on a failed check."""
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "uac"))
import uac_timing as u                       # noqa: E402
import uac_phase as p                        # noqa: E402

OUT = ROOT / "build" / "uac_test"
PH = p.timer5_phase
CC = os.environ.get("CC", "cc")
fails = 0


def check(ok, what):
    global fails
    print(("ok    " if ok else "FAIL  ") + what)
    fails += not ok


def flash_slice_us():
    h = (ROOT / "firmware" / "hal" / "fm1_flash.h").read_text(encoding="utf-8")
    return int(re.search(r"#define FL_SLICE_US\s+(\d+)u", h).group(1)) + 50


def build(phase, src=None, name=None):
    OUT.mkdir(parents=True, exist_ok=True)
    exe = str(OUT / (name or f"uac_seq_phase{phase}"))
    r = subprocess.run([CC, "-O1", "-w", f"-DFELUCCA_UAC_PHASE={phase}", "-I" + str(src or ROOT / "firmware" / "src"),
                        "-o", exe, str(ROOT / "tests" / "uac_seq_test.c")], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-2000:])
        return None
    return exe


def cases(exe, *args):
    if not exe:
        return [], 1
    r = subprocess.run([exe, *args], capture_output=True, text=True)
    rows = []
    for ln in r.stdout.splitlines():
        if ln.startswith("case "):
            t = ln.split()[1:]
            rows.append({t[i]: float(t[i + 1]) for i in range(0, len(t), 2)})
    return rows, r.returncode if rows else 1


# ---- the timing model

c0, c150 = u.critical(timer5=PH), u.critical(timer5=PH, jitter=150)
t0, t150 = u.critical(), u.critical(jitter=150)
check(c0 >= 880 and c150 >= 580, f"model: the critical stretch {c0} us ({c150} us, the token moving 150 us); the fixed "
      f"poll's {t0} / {t150}")
worst, bad, counted, _ = u.sweep(flash_slice_us(), step=20, jitter=150, timer5=PH)
check(bad == 0 and counted == 0 and worst >= 250, f"model: a flash slice ({flash_slice_us()} us), the token moving "
      f"150 us: nothing lost, slack {worst} us")
worst, bad, counted, _ = u.sweep(0, step=20, timer5=PH)
check(bad == 0 and counted == 0 and worst >= 480, f"model: no stretch: nothing lost, slack {worst} us")
check(u.sweep(800, step=20, audio=(5100, 2500), timer5=PH)[1] == 0 and
      u.sweep(800, step=20, audio=(4900, 4900), timer5=PH)[1] == 0, "model: the poll nested in a render: the same")
ok = True
for start in range(0, 2000, 140):
    for first in (2, 3, 5, 30):
        for ph in range(0, 1000, 180):
            r = u.simulate(u.Case(ms=40, tok_phase=ph, start_at=start, first_frame=first, timer5=PH))
            ok = ok and r.true_missed == 0 and r.counted == 0
check(ok, "model: a stream start alone never misses")
for drift in (0.3, -0.3):
    rnd = random.Random(1)
    j = {n: int(drift * n) for n in range(2400)}
    cli = tuple((k * 37000 + rnd.randrange(1000), 850) for k in range(1, 60))
    ph0 = rnd.randrange(1000)
    r = u.simulate(u.Case(ms=2300, tok_phase=ph0, tok_jitter=j, cli=cli, timer5=PH))
    t = u.simulate(u.Case(ms=2300, tok_phase=ph0, tok_jitter=j, cli=cli))
    check(r.true_missed == 0 and t.true_missed > 0, f"model: a token drifting {drift:+} us a frame, 850 us stretches: "
          f"lost {r.true_missed} (the fixed poll {t.true_missed})")
bad = {}
for name, pol in (("fixed", None), ("phase", PH)):
    bad[name] = 0
    for ph in range(0, 1000, 25):
        base = u.simulate(u.Case(ms=45, tok_phase=ph, timer5=pol))
        cli, t = [], 5000
        for s in base.services:
            if s >= t and len(cli) < 20:
                cli.append((s + 2, 800))
                t = s + 2 + 800 + 750
        bad[name] += u.simulate(u.Case(ms=45, tok_phase=ph, cli=tuple(cli), timer5=pol)).true_missed > 0
check(bad["phase"] == 0 and bad["fixed"] > 0, f"model: 800 us slices right after a packet: phases losing a token "
      f"{bad['phase']} (the fixed poll {bad['fixed']})")

# ---- usb.c itself

fixed, phased = build(0), build(1)
rows, rc = cases(phased, "--hours", "0.1")
check(rc == 0 and len(rows) == 12 and all(r["phase"] == 1 for r in rows) and
      not [r for r in rows if r["dup_uncounted"] or r["drop_uncounted"] or r["n_bad"] or r["overwritten"]] and
      all(r["stalls"] > 0 and r["overruns"] > 0 and r["adj_up"] > 0 and r["adj_down"] > 0 for r in rows),
      "usb.c: the phased poll, every frame exact (stalls, overruns, both packet lengths), 12 cases")
rows, rc = cases(phased, "--hours", "0.05", "--sie-fault", "0.0005")
check(rc == 0 and bool(rows) and all(r["sie_to"] > 0 and r["overwritten"] == 0 and r["drop_uncounted"] == 0 and
                                     r["dup_uncounted"] == 0 for r in rows), "usb.c: an SIE time-out waits, counted")
f0, rc0 = cases(fixed, "--hours", "0.02", "--cli", "700", "50", "--tj", "20")
f1, rc1 = cases(phased, "--hours", "0.02", "--cli", "700", "50", "--tj", "20")
m0, m1 = sum(r["true_missed"] for r in f0), sum(r["true_missed"] for r in f1)
check(rc0 == 0 and rc1 == 0 and m0 > 50 and m1 == 0 and all(r["cli"] > 1000 for r in f1),
      f"usb.c: 700 us stretches every ~50 ms: tokens lost {m1:.0f} (the fixed poll {m0:.0f})")
base = cases(fixed, "--hours", "0.01", "--tj", "20")[0]
ph = cases(phased, "--hours", "0.01", "--tj", "20")[0]
ph400 = cases(phased, "--hours", "0.01")[0]
b = max((r["polls_per_s"] for r in base), default=0)
m, m400 = max((r["polls_per_s"] for r in ph), default=1e9), max((r["polls_per_s"] for r in ph400), default=1e9)
check(b > 0 and m <= 1.06 * b and m400 <= 1.15 * b, f"usb.c: EP4 polls a second {m:.0f} (+-10 us token jitter), "
      f"{m400:.0f} (+-200 us); the fixed poll {b:.0f}")
s0, rc0 = cases(fixed, "--hours", "0.02")
s1, rc1 = cases(phased, "--hours", "0.02")
a0, a1 = max((r["sie_blk_max"] for r in s0), default=0), max((r["sie_blk_max"] for r in s1), default=1e9)
check(rc0 == 0 and rc1 == 0 and a1 <= a0, f"usb.c: SIE accesses in the worst audio block {a1:.0f} (fixed {a0:.0f})")
args = ("--hours", "0.005", "--cli", "600", "0.75", "--cli-exact", "--tj", "20")
fr0, _ = cases(fixed, *args)
fr1, _ = cases(phased, *args)
al1, rc = cases(phased, *args, "--align")
t = [sum(r["true_missed"] for r in x) for x in (fr0, fr1, al1)]
n_free, n_al = sum(r["cli"] for r in fr1), sum(r["cli"] for r in al1)
check(rc == 0 and t[0] > 100 and t[2] == 0 and n_al >= 0.45 * n_free and
      not [r for r in al1 if r["dup_uncounted"] or r["drop_uncounted"] or r["n_bad"]],
      f"usb.c: a 600 us slice train: tokens lost {t[2]:.0f} aligned (the fixed poll {t[0]:.0f}, phased at random "
      f"{t[1]:.0f}); slices aligned / free {n_al / max(n_free, 1):.2f}")
args = ("--hours", "0.003", "--clean", "--tj", "20", "--cli", "282", "0.75", "--cli-exact")
free = max((r["gap_max_us"] for r in cases(phased, *args)[0]), default=0)
al = max((r["gap_max_us"] for r in cases(phased, *args, "--align")[0]), default=1e9)
check(free >= 700 and al <= 520, f"usb.c: a settings save's slices (282 us, 750 us apart): the longest EP4 poll gap "
      f"{al:.0f} us aligned ({free:.0f} at random moments)")
ok = True
for exe in (fixed, phased):
    rows, _ = cases(exe, "--hours", "0.01", "--ctl-reset", "100")
    ok = ok and bool(rows) and all(r["ep_wrong"] == 0 for r in rows)
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "src"
    shutil.copytree(ROOT / "firmware" / "hal", Path(tmp) / "hal")
    src.mkdir()
    shutil.copy(ROOT / "firmware" / "src" / "uac_fir.h", src)
    usb = (ROOT / "firmware" / "src" / "usb.c").read_text(encoding="utf-8")
    planted = usb.replace("    usb.timeouts = 0;\n#if FELUCCA_UAC_PHASE\n    uac.idx4 = 0; ",
                          "    usb.timeouts = 0;\n#if FELUCCA_UAC_PHASE\n    (void)0; ", 1)
    (src / "usb.c").write_text(planted, encoding="utf-8")
    rows, _ = cases(build(1, src, "uac_seq_planted"), "--hours", "0.01", "--ctl-reset", "100")
    ok = ok and planted != usb and bool(rows) and all(r["ep_wrong"] > 0 for r in rows)
check(ok, "usb.c: a controller reset: the next EP4 poll writes INDEX again (a tree without it reads EP0's CSR0)")

sys.exit(1 if fails else 0)
