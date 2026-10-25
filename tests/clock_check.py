# SPDX-License-Identifier: GPL-3.0-only
"""The tempo clock (firmware/src/tempo.c) on the host build: synthetic MIDI clock streams over USB and TRS, steady,
jittered, in bursts (several pulses in one USB packet), with the main loop busy; tempo jumps; the clock lost and back;
Start / Stop / Continue / Song Position; the CLOCK modes (INT ignores a clock, FOLLOW ignores the taps) and each port's
CLK IN; the grain delay's SYNC on the clock's tempo. Each case writes a host script, runs build/fw/textura-host and
reads the state from its "dump" lines. Run from the top of the tree: python3 -I tests/clock_check.py"""
import os
import random
import re
import subprocess
import sys

HOST = "build/fw/textura-host"
OUT = "build/fw/clock"
FAILS = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        FAILS.append(what)


def pulses(t0, bpm, n):
    """the true times (ms, float) of n pulses from t0 at bpm"""
    p = 60000.0 / bpm / 24
    return [t0 + i * p for i in range(n)]


def emit(ts, src="midi", arrive=lambda t: t):
    return [f"{int(arrive(t))} {src} 0xF8 0 0" for t in ts]


def nrpn(t, idx, v):
    """a global parameter (scope 4) by NRPN on channel 5"""
    return [f"{t} midi 0xB4 99 4", f"{t} midi 0xB4 98 {idx}", f"{t} midi 0xB4 6 {v}"]


PG_GSYNC, PG_CLOCK, PG_INUSB, PG_INTRS = 12, 40, 41, 42


def run(tag, lines, dumps):
    os.makedirs(OUT, exist_ok=True)
    end = max([int(l.split()[0]) for l in lines] + list(dumps)) + 50
    body = sorted(lines + [f"{int(t)} dump" for t in dumps], key=lambda l: int(l.split()[0]))
    path = os.path.join(OUT, tag + ".txt")
    with open(path, "w") as f:
        f.write("\n".join(body + [f"{end} end"]) + "\n")
    r = subprocess.run([HOST, os.path.join(OUT, tag), path], capture_output=True, text=True)
    st = {}
    for line in r.stdout.splitlines():
        m = re.match(r"@ (\d+) (\S+) (.*)", line)
        if m:
            st.setdefault(int(m.group(1)), {})[m.group(2)] = m.group(3)
    return st


def near(v, want, tol):
    return abs(int(v) - want) <= tol


def main():
    if not os.path.exists(HOST):
        sys.exit(f"{HOST}: build it first (make fw-host)")
    rng = random.Random(7)

    # steady 96 BPM over USB (AUTO, the default): the tempo, the lock, the beat in samples, the delay on it
    ls = nrpn(100, PG_GSYNC, 9 * 127 // 14 + 1) + emit(pulses(500, 96, 24 * 8))
    s = run("steady", ls, [2000, 2600, 4000])
    check(near(s[2600]["bpm10"], 960, 1) and s[2600]["clklock"] == "1", f"steady 96 BPM USB: {s[2600]['bpm10']} lock {s[2600]['clklock']}")
    check(near(s[4000]["beatsmp"], 27562, 30), f"steady: a beat {s[4000]['beatsmp']} samples (27562)")
    check(s[4000]["gsync"] == s[4000]["beatsmp"] and s[4000]["gdsync"] == s[4000]["gsync"],
          f"steady: the grain delay's 1/4 on the clock ({s[4000]['gsync']}, the chain {s[4000]['gdsync']})")

    # jittered TRS (+-3 ms) and bursty USB (pulses arriving 3 at a time) with the main loop busy now and then
    ts = pulses(500, 120, 24 * 10)
    s = run("jitter", emit(ts, "trs", lambda t: t + rng.uniform(-3, 3)), [4500])
    check(near(s[4500]["bpm10"], 1200, 3) and s[4500]["clksrc"] == "2", f"jittered TRS 120 BPM: {s[4500]['bpm10']} from {s[4500]['clksrc']}")
    ls = emit(ts, "midi", lambda t: t + (2 - int((t - 500) / 20.833) % 3) * 20.833)
    t = 600
    while t < 5000:
        ls.append(f"{int(t)} stall {rng.randint(0, 30)}")
        t += rng.uniform(40, 200)
    s = run("bursty", ls, [4500])
    check(near(s[4500]["bpm10"], 1200, 3) and s[4500]["clklock"] == "1", f"bursty USB, a busy loop: {s[4500]['bpm10']}")

    # a jump 100 -> 140 BPM: followed within 2 beats
    a = pulses(500, 100, 24 * 4)
    b = pulses(a[-1] + 60000 / 140 / 24, 140, 24 * 6)
    s = run("jump", emit(a) + emit(b), [int(a[-1]) - 10, int(b[0] + 2 * 428.6)])
    t1 = int(b[0] + 2 * 428.6)
    check(near(s[int(a[-1]) - 10]["bpm10"], 1000, 2) and near(s[t1]["bpm10"], 1400, 3), f"a jump 100 -> 140: {s[t1]['bpm10']} two beats on")

    # lost and back: 1 s without pulses (the lock goes, the tempo stays), then back at 90 BPM
    a = pulses(500, 110, 24 * 5)
    b = pulses(a[-1] + 1000, 90, 24 * 5)
    s = run("loss", emit(a) + emit(b), [int(a[-1]) + 500, int(b[-1]) - 5])
    tl = int(a[-1]) + 500
    check(s[tl]["clklock"] == "0" and near(s[tl]["bpm10"], 1100, 2), f"lost: the lock {s[tl]['clklock']}, the tempo held {s[tl]['bpm10']}")
    check(s[int(b[-1]) - 5]["clklock"] == "1" and near(s[int(b[-1]) - 5]["bpm10"], 900, 2), f"back at 90: {s[int(b[-1]) - 5]['bpm10']}")

    # Start, Stop, Song Position, Continue (16ths -> 6 pulses each)
    ts = pulses(500, 120, 24 * 8)
    ls = emit(ts) + ["1000 midi 0xFA 0 0", "2000 midi 0xFC 0 0", "2100 midi 0xF2 16 0", "2500 midi 0xFB 0 0"]
    s = run("transport", ls, [990, 1500, 2050, 2200, 2700])
    check(s[990]["running"] == "0" and s[1500]["running"] == "1" and s[2050]["running"] == "0" and s[2700]["running"] == "1",
          "Start / Stop / Continue move the transport")
    check(s[2200]["songpos"] == "96", f"Song Position 16 sixteenths = 96 pulses ({s[2200]['songpos']})")
    check(int(s[2700]["songpos"]) > 96, f"Continue: on from it ({s[2700]['songpos']})")

    # INT: a clock changes nothing; a port off: its clock is dropped, the other's is followed
    s = run("int", nrpn(100, PG_CLOCK, 0) + emit(pulses(500, 90, 24 * 6)), [3500])
    check(s[3500]["bpm10"] == "1200" and s[3500]["clklock"] == "0", f"INT ignores a clock ({s[3500]['bpm10']})")
    s = run("port", nrpn(100, PG_INUSB, 0) + emit(pulses(500, 90, 24 * 6)) + emit(pulses(500.3, 100, 24 * 6), "trs"), [3500])
    check(near(s[3500]["bpm10"], 1000, 2) and s[3500]["clksrc"] == "2", f"CLK IN USB off: the TRS clock ({s[3500]['bpm10']})")

    # the taps: ignored while a clock leads; in FOLLOW always, and FOLLOW without a clock is stopped
    taps = [f"{t} btn 12" for t in (3000, 3400, 3800)]
    s = run("tapclock", emit(pulses(500, 90, 24 * 9)) + taps, [3900])
    check(near(s[3900]["bpm10"], 900, 2), f"taps under a clock change nothing ({s[3900]['bpm10']})")
    s = run("follow", nrpn(100, PG_CLOCK, 127 * 2 // 3) + [f"{t} btn 12" for t in (1000, 1400, 1800)] + ["2500 press 12", "3200 release 12"], [1900, 3300])
    check(s[1900]["bpm10"] == "1200" and s[3300]["running"] == "0", f"FOLLOW: taps ({s[1900]['bpm10']}) and PLAY held do nothing without a clock")

    # the clock gone in AUTO: after 2 s the BPM's tempo again
    a = pulses(500, 100, 24 * 4)
    s = run("auto", emit(a), [int(a[-1]) + 1000, int(a[-1]) + 2600])
    check(near(s[int(a[-1]) + 1000]["bpm10"], 1000, 2) and s[int(a[-1]) + 2600]["bpm10"] == "1200",
          f"AUTO: the clock's tempo held, then the BPM's ({s[int(a[-1]) + 2600]['bpm10']})")

    print(f"{'ok' if not FAILS else 'FAIL'}   clock: {len(FAILS)} failure(s)")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
