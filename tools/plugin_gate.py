#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The desktop plugin plays what the FM-1 plays (make test-plugin).

The plugin is the app compiled unchanged (plugin/src/engine_unit.c) with 16 lanes (plugin/cmake/slots.cmake
TXP_LANES), so its render of a MIDI script must be the host build's render of the same script (tests/host.c), built
with the same switch (LN_HQ: no CPU limit, nothing thinned): four-lane content against the four-lane host build (the
FM-1's app: the 16-lane plugin plays it sample for sample), sixteen-lane content against the 16-lane host build.

  equal   44.1 kHz, the plugin's test time (TEXTURA_PLUGIN_TEST=ms: the host build's events-per-millisecond order):
          after the latency, the plugin's output is the host build's, sample for sample (the host writes 16 bits with
          a clip: the plugin's float x 32768, clipped the same way, must be equal). stop, lanes, hq, presets (every
          factory preset, not with --quick): against the four-lane build; lanes16 (sixteen lanes playing, every
          channel, then a factory preset over them: lanes 5 .. 16 empty again), against the 16-lane build, whose
          expects must pass too
  rates   48, 96 and 88.2 kHz (the oversampler) and 32 kHz (the sinc), the test time: the host build's render taken to
          that rate offline (a long polyphase filter) against the plugin's: the residual under -90 dB of the signal
          below 12 kHz (or a quarter of the rate; the two resamplers' own filters differ in their transition bands)
          (stop: a lane stopped; lanes: notes, CCs and presets on every lane and channel 5; hq: a heavy 4-lane scene;
          without --quick also presets: every factory preset by program change)
  hq      the host build with LN_HQ plays plugin/tests/scripts/hq.txt: the governor's pool has no end, nothing is cut
  state   the shipped time (exact): a render, its state saved and loaded into a second instance, every parameter
          reads back the same and the state saves the same again (four lanes, and sixteen lanes playing); an instance
          never activated, every parameter set at random by a flush (as clap-validator does), the same; and a state
          of the four-lane plugin (plugin/tests/fixtures/state_v1.json: version 1, the FM-1's scene record) loads into
          lanes 1 .. 4, the others empty, LANES 4, its records and settings as they were
  cpu     the shipped time at 48 kHz, 512-frame buffers: average and worst time per process call for heavy 4-lane
          scenes (every lane at HOW MANY and HOW ACTIVE 127, the grain delay at all its grains, the reverb up), and
          for sixteen heavy lanes (the four heaviest presets' lanes side by side, by a state)
  gui     the window, headless (no native view): plugin/tests/scripts/gui4.txt (four lanes) and gui16.txt (sixteen)
          played, then the window painted as a PNG twice from two runs (the same bytes: stable), not empty; and the
          window's controls used (M, S, VOL, a map dot, LANES +, a row) and checked against the parameters, the events
          to the host, the masks and the saved state (txp_clap_render --shot, --gui-test). --shots DIR keeps a copy of
          the two pictures there.

  tools/plugin_gate.py [--build build/plugin] [--quick] [--only equal,hq,rates,state,cpu,gui] [--shots DIR] [--wine]
--wine: the Windows build under wine (tools/plugin_win.sh test, in its container): BUILD/txp_clap_render.exe and
BUILD/bundles/CLAP/Textura.clap, every path handed to them as Z:/... (the host build stays native).
Exit 0 when every check passes. Needs numpy and scipy for the rates (skipped without them)."""
import argparse
import hashlib
import json
import os
import shutil
import re
import subprocess
import sys
import wave
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = ["tests/scripts/stop.txt", "plugin/tests/scripts/lanes.txt", "plugin/tests/scripts/hq.txt"]
SCRIPTS16 = ["plugin/tests/scripts/lanes16.txt"]   # (against the 16-lane host build)
V1_STATE = "plugin/tests/fixtures/state_v1.json"   # (the four-lane plugin's state: F034 WATERFALL, a card, effects)
LONG = ["tests/scripts/presets.txt"]   # (not with --quick: every factory preset, 400 s)
HOST_SRC = ["firmware/src/textura.c"]
CFLAGS = ["-O2", "-Wall", "-Wno-unused-function", "-DDINGHY_HOST", "-DLN_HQ", '-DAPP_VER="dev"',
          "-Ifirmware/src", "-Ifirmware/gen", "-Iengines"]
HEAVY = [11, 33, 73, 14]   # the program numbers of 4-lane presets: F012 STORM NEARS, F034 WATERFALL, F074 FOREST
                           # FIRE, F015 HAILSTORM


def run(cmd, env=None, check=True):
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("failed: " + " ".join(cmd))
    return r


def host_build(out, lanes=4):
    eng = sorted(os.path.join("engines", f) for f in os.listdir(os.path.join(ROOT, "engines")) if f.endswith(".c"))
    deps = [os.path.join(ROOT, d) for d in eng + ["tests/host.c", "web/web_hal.c"]]
    for d in ("firmware/src", "firmware/gen", "engines"):
        deps += [os.path.join(ROOT, d, f) for f in os.listdir(os.path.join(ROOT, d)) if f.endswith((".c", ".h"))]
    if os.path.exists(out) and os.path.getmtime(out) > max(os.path.getmtime(d) for d in deps):
        return
    cc = os.environ.get("CC", "cc")
    lanes_def = [] if lanes == 4 else ["-DTX_MIX_SLOTS=%du" % lanes]
    lanes_def += ["-DSCN_FX_DEF=SCN_FX_ALWAYS"]   # (EFFECTS ON LOAD at the plugin's default, not the FM-1's)
    run(["nice", "-n", "15", cc] + CFLAGS + lanes_def + ["-o", out] + HOST_SRC + eng + ["-lm"])


def read_wav(path):
    with wave.open(path, "rb") as w:
        n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    return raw, ch, sw, n


def read_float_wav(path):   # 32-bit float, stereo (wave cannot read format 3: read the data chunk directly)
    with open(path, "rb") as f:
        b = f.read()
    at = b.index(b"data") + 8
    a = array("f")
    a.frombytes(b[at:])
    return a


def host_render(host, script, out):
    r = run([host, out, os.path.join(ROOT, script)], check=False)   # (its exit: the script's failed expects)
    raw, ch, sw, n = read_wav(out + ".wav")
    a = array("h")
    a.frombytes(raw)
    return a, r.returncode


WINE = False   # (--wine: the Windows test host under wine, its paths as Z:/...)


def plug_render(exe, bundle, script, out, rate, test=True, extra=()):
    env = dict(os.environ)
    if test:
        env["TEXTURA_PLUGIN_TEST"] = "ms"
    else:
        env.pop("TEXTURA_PLUGIN_TEST", None)
    cmd = [exe, bundle, os.path.join(ROOT, script), out, "--rate", str(rate)] + list(extra)
    if WINE:   # (wine passes the environment on: TEXTURA_PLUGIN_TEST reaches the plugin)
        cmd = ["wine"] + [("Z:" + a) if a.startswith("/") else a for a in cmd]
    r = run(cmd, env=env, check=False)
    if r.returncode not in (0, 4):   # (4: --state-check's saved text differs; check_state looks closer)
        sys.stderr.write(r.stdout + r.stderr)
    return r


def compare(h, p):
    """(samples, samples that differ, the first that does, the host's peak): the plugin's float x 32768, clipped
    as the host build writes its 16 bits"""
    try:
        import numpy as np
        hh = np.frombuffer(h.tobytes(), dtype=np.int16).astype(np.int32)
        pp = np.clip(np.rint(np.frombuffer(p.tobytes(), dtype=np.float32).astype(np.float64) * 32768.0),
                     -32767, 32767).astype(np.int32)
        n = min(len(hh), len(pp))
        d = np.nonzero(hh[:n] != pp[:n])[0]
        return n, len(d), int(d[0]) if len(d) else -1, int(np.max(np.abs(hh[:n]))) if n else 0
    except ImportError:
        n = min(len(h), len(p))
        diff, first, peak = 0, -1, 0
        for i in range(n):
            v = int(round(p[i] * 32768.0))
            v = 32767 if v > 32767 else -32767 if v < -32767 else v
            peak = max(peak, abs(h[i]))
            if v != h[i]:
                diff += 1
                first = i if first < 0 else first
        return n, diff, first, peak


def check_equal(args, host, host16, exe, bundle, tmp):
    ok = True
    for s in SCRIPTS + SCRIPTS16 + ([] if args.quick else LONG):
        name = os.path.basename(s)[:-4]
        h, hrc = host_render(host16 if s in SCRIPTS16 else host, s, os.path.join(tmp, "host-" + name))
        if s in SCRIPTS16 and hrc != 0:
            print("FAIL  equal %s: the 16-lane host build failed %d of the script's expects" % (name, hrc))
            ok = False
        r = plug_render(exe, bundle, s, os.path.join(tmp, "plug-" + name + ".wav"), 44100)
        if r.returncode != 0:
            print("FAIL  equal %s: the plugin's render exited %d" % (name, r.returncode))
            ok = False
            continue
        p = read_float_wav(os.path.join(tmp, "plug-" + name + ".wav"))
        n, diff, first, peak = compare(h, p)
        lat = re.search(r"latency (\d+)", r.stdout)
        good = diff == 0 and n > 44100 and peak > 0
        line = "%s  equal %-7s %d frames (latency %s), peak %d, %d samples differ%s" % (
            "ok  " if good else "FAIL", name, n // 2, lat.group(1) if lat else "?", peak, diff,
            " (16 lanes; the host build's expects passed)" if s in SCRIPTS16 and hrc == 0 else "")
        if first >= 0:
            line += ", the first at frame %d" % (first // 2)
        print(line)
        ok = ok and good
    return ok


def check_hq(args, host, tmp):
    r = run([host, os.path.join(tmp, "host-hqx"), os.path.join(ROOT, "plugin/tests/scripts/hq.txt")], check=False)
    n = len(re.findall(r" ok   expect", r.stdout))
    good = r.returncode == 0 and n >= 5
    print("%s  hq: the host build with LN_HQ, %d expects passed (the pool has no end, no lane cut)%s" % (
        "ok  " if good else "FAIL", n, "" if good else "\n" + r.stdout[-2000:]))
    return good


def check_rates(args, host, exe, bundle, tmp):
    try:
        import numpy as np
        from scipy.signal import resample_poly
    except ImportError:
        print("skip  rates: no numpy / scipy")
        return True
    ok = True
    s = SCRIPTS[1]
    name = os.path.basename(s)[:-4]
    h, _ = host_render(host, s, os.path.join(tmp, "host-" + name))
    hs = np.array(h, dtype=np.float64).reshape(-1, 2) / 32768.0
    rates = [48000, 96000] if args.quick else [48000, 96000, 88200, 32000]
    for rate in rates:
        out = os.path.join(tmp, "plug-%s-%d.wav" % (name, rate))
        r = plug_render(exe, bundle, s, out, rate)
        if r.returncode != 0:
            print("FAIL  rate %d: the plugin's render exited %d" % (rate, r.returncode))
            ok = False
            continue
        p = np.array(read_float_wav(out), dtype=np.float64).reshape(-1, 2)
        from math import gcd
        g = gcd(rate, 44100)
        ref = resample_poly(hs, rate // g, 44100 // g, axis=0, window=("kaiser", 12.0))
        n = min(len(ref), len(p)) - rate // 10
        a, b = ref[rate // 10:n], p[rate // 10:n]   # (the edges: the offline filter's own start and end)
        # the band both keep: the two resamplers' transition bands differ (above ~12 kHz, the plugin keeps more of the top octave)
        top = min(12000.0, 0.25 * rate)
        f = np.fft.rfftfreq(len(a), 1.0 / rate)
        keep = f < top
        A, B = np.fft.rfft(a, axis=0)[keep], np.fft.rfft(b, axis=0)[keep]
        res = 10 * np.log10((np.sum(np.abs(B - A) ** 2) + 1e-30) / (np.sum(np.abs(A) ** 2) + 1e-30))
        lim = -90.0
        good = bool(res < lim)
        print("%s  rate %6d: residual %.1f dB of the signal under %.1f kHz (limit %.0f dB)" % (
            "ok  " if good else "FAIL", rate, res, top / 1000.0, lim))
        ok = ok and good
    return ok


SCN_LANES = 16                # the plugin's lanes (plugin/cmake/slots.cmake TXP_LANES)
SCN_STEP, SCN_RUN = 16 + 2 * SCN_LANES, 16 + 3 * SCN_LANES   # scenes.c scn_t (version 2): step[] (HOW ACTIVE), run[]


def scene_bytes(path):
    t = open(path).read()
    m = re.search(r'"scene":"([0-9a-f]+)"', t)
    return bytes.fromhex(m.group(1)) if m else b"", t[:t.index('"scene"')] if '"scene"' in t else t


def state_ok(r, out):
    """the harness's --state-check: (passed, a note). Exit 4: the values read back the same but the state saved again
    differs; a stopped lane's HOW ACTIVE may read 0 then (a stopped lane loads with its step cleared: scenes.c
    scn_apply, scape.c), which plays nothing either way; any other byte is a failure"""
    if r.returncode == 0:
        return True, ""
    if r.returncode != 4:
        return False, ""
    a, ha = scene_bytes(out + ".state1")
    b, hb = scene_bytes(out + ".state2")
    bad = [i for i in range(max(len(a), len(b))) if i >= len(a) or i >= len(b) or a[i] != b[i]]
    ok_i = [i for i in bad if SCN_STEP <= i < SCN_STEP + SCN_LANES and a[SCN_RUN + i - SCN_STEP] == 0
            and b[SCN_RUN + i - SCN_STEP] == 0]
    good = ha == hb and len(a) == len(b) and len(ok_i) == len(bad)
    return good, " (saved again: %d bytes differ, %d of them a stopped lane's HOW ACTIVE)" % (len(bad), len(ok_i))


def lanes16_hold(tmp):
    """lanes16.txt up to its factory preset: sixteen lanes playing when it ends (its state saves sixteen)"""
    sp = os.path.join(tmp, "lanes16-hold.txt")
    with open(os.path.join(ROOT, SCRIPTS16[0])) as f, open(sp, "w") as o:
        for line in f:
            w = line.split()
            if len(w) >= 3 and w[1] == "midi" and w[2] == "0xC0" and int(w[0]) > 2000:
                o.write("%d end\n" % (int(w[0]) - 100))
                break
            o.write(line)
    return sp


def check_state(args, exe, bundle, tmp):
    good = True
    for what, script in (("state", SCRIPTS[1]), ("state, 16 lanes playing", lanes16_hold(tmp))):
        out = os.path.join(tmp, "plug-state.wav" if what == "state" else "plug-state16.wav")
        r = plug_render(exe, bundle, script, out, 48000, test=False, extra=["--state-check"])
        m = re.search(r"state (\d+) bytes, saved again \w+( \w+)?, (\d+) parameters, (\d+) differ", r.stdout)
        g, note = state_ok(r, out)
        print("%s  %s: %s%s" % ("ok  " if g else "FAIL", what, m.group(0) if m else (r.stdout + r.stderr).strip(), note))
        good = good and g
    good = check_v1(exe, bundle, tmp) and good
    good = check_old_cur(exe, bundle, tmp) and good
    end = os.path.join(tmp, "end.txt")
    with open(end, "w") as f:
        f.write("100 end\n")
    for seed in ((1, 2) if args.quick else range(1, 11)):
        out = os.path.join(tmp, "fuzz.wav")
        r = plug_render(exe, bundle, end, out, 48000, test=False,
                        extra=["--state-check", "--inactive", "--fuzz", str(seed)])
        m = re.search(r"state .*", r.stdout)
        g, note = state_ok(r, out)
        print("%s  state, never activated, every parameter at random (seed %d): %s%s" % (
            "ok  " if g else "FAIL", seed, m.group(0) if m else (r.stdout + r.stderr).strip(), note))
        good = good and g
    return good


V1 = dict(snd=16, cnt=20, step=24, run=28, vol=32, pan=36, nrec=40, lfam=42, rec=44)   # scenes.c scn_v1_t (4 lanes)
V2 = dict(snd=16, cnt=16 + SCN_LANES, step=16 + 2 * SCN_LANES, run=16 + 3 * SCN_LANES, vol=16 + 4 * SCN_LANES,
          pan=16 + 5 * SCN_LANES, lf=16 + 6 * SCN_LANES, mute=16 + 7 * SCN_LANES, nrec=20 + 7 * SCN_LANES,
          rec=24 + 7 * SCN_LANES)    # scenes.c scn_t, version 2


def records(b, at, n):
    return sorted((b[at + 4 * i] | b[at + 4 * i + 1] << 8, int.from_bytes(b[at + 4 * i + 2:at + 4 * i + 4], "little", signed=True))
                  for i in range(n))


def check_old_cur(exe, bundle, tmp):
    """a state saved before F101 (no "nf": 100 factory scenes, then the user's) with U05 current ("cur" 104) loads
    with U05 current: saved again, its "cur" is U05's in this build (nf + 4)"""
    end = os.path.join(tmp, "end3000.txt")
    with open(end, "w") as f:
        f.write("3000 end\n")
    base, old, out = (os.path.join(tmp, "plug-oldcur-%s.json" % n) for n in ("base", "old", "out"))
    plug_render(exe, bundle, end, os.path.join(tmp, "plug-oldcur.wav"), 48000, test=False, extra=["--state-out", base])
    try:
        st = json.load(open(base))
        st.pop("nf", None)
        st.update(cur="104", umask="16", user=st["scene"])   # (U05 holds the scene that plays)
        json.dump(st, open(old, "w"), separators=(",", ":"))   # (the plugin reads its own compact form)
        plug_render(exe, bundle, end, os.path.join(tmp, "plug-oldcur2.wav"), 48000, test=False,
                    extra=["--state-in", old, "--state-out", out])
        st2 = json.load(open(out))
        cur, nf = int(st2["cur"]), int(st2["nf"])
        good = cur == nf + 4
        note = "cur %d, nf %d: %s" % (cur, nf, "U%02d" % (cur - nf + 1) if cur >= nf else "F%03d" % (cur + 1))
    except (OSError, ValueError, KeyError) as e:
        good, note = False, str(e)
    print("%s  state saved before F101 (100 factory scenes) with U05 current loads with U05: %s" % ("ok  " if good else "FAIL", note))
    return good


def check_v1(exe, bundle, tmp):
    """the four-lane plugin's state (version 1) into the 16-lane plugin: lanes 1 .. 4 as it had them, the others
    empty, LANES 4, the same records and settings; then saved, it loads and saves the same again"""
    out = os.path.join(tmp, "plug-v1.wav")
    st = os.path.join(tmp, "plug-v1.json")
    end = os.path.join(tmp, "end3000.txt")
    with open(end, "w") as f:
        f.write("3000 end\n")
    r = plug_render(exe, bundle, end, out, 48000, test=False,
                    extra=["--state-in", os.path.join(ROOT, V1_STATE), "--state-out", st, "--state-check"])
    g_rt, note = state_ok(r, out)
    a, ha = scene_bytes(os.path.join(ROOT, V1_STATE))
    try:
        b, hb = scene_bytes(st)
    except (OSError, ValueError):
        b, hb = b"", ""
    bad = []
    if len(a) != 1020 or a[0] != 1:
        bad.append("the fixture is not a version 1 record")
    elif len(b) != 2040 or b[0] != 2:
        bad.append("saved: not a version 2 record (%d bytes)" % len(b))
    else:
        if b[2] != a[2]:
            bad.append("landscape %d, not %d" % (b[2], a[2]))
        if b[3] != 4:
            bad.append("LANES %d, not 4" % (b[3] + 0))
        mute = int.from_bytes(b[V2["mute"]:V2["mute"] + 4], "little")
        lfam = a[V1["lfam"]] | a[V1["lfam"] + 1] << 8
        for k in range(SCN_LANES):
            snd = b[V2["snd"] + k] - 256 if b[V2["snd"] + k] > 127 else b[V2["snd"] + k]
            if k >= 4:
                if snd != -1 or b[V2["run"] + k] or mute >> k & 1:
                    bad.append("lane %d not empty" % (k + 1))
                continue
            for f in ("snd", "run", "vol", "pan") + (("cnt", "step") if a[V1["run"] + k] else ()):
                if b[V2[f] + k] != a[V1[f] + k]:
                    bad.append("lane %d %s %d, not %d" % (k + 1, f, b[V2[f] + k], a[V1[f] + k]))
            if snd >= 0 and b[V2["lf"] + k] != (lfam >> 4 * k & 15):
                bad.append("lane %d family %d, not %d" % (k + 1, b[V2["lf"] + k], lfam >> 4 * k & 15))
            if (mute >> k & 1) != (a[3] >> k & 1):
                bad.append("lane %d mute" % (k + 1))
        ra = records(a, V1["rec"], a[V1["nrec"]] | a[V1["nrec"] + 1] << 8)
        rb = records(b, V2["rec"], b[V2["nrec"]] | b[V2["nrec"] + 1] << 8)
        if ra != rb:
            bad.append("records: %d, not the fixture's %d (%s)" % (len(rb), len(ra), sorted(set(ra) ^ set(rb))[:4]))
        sa = dict(x.split("=") for x in re.search(r'"set":"([^"]*)"', ha).group(1).split(","))
        sb = dict(x.split("=") for x in re.search(r'"set":"([^"]*)"', hb).group(1).split(","))
        if any(sb.get(k) != v for k, v in sa.items()):
            bad.append("settings: %s" % [k for k, v in sa.items() if sb.get(k) != v])
        if '"textura":"2"' not in hb:
            bad.append("saved: not \"textura\":\"2\"")
    good = not bad and g_rt
    print("%s  state of before (version 1, four lanes) into 16 lanes: %s%s" % (
        "ok  " if good else "FAIL", "lanes 1 .. 4 as saved, 5 .. 16 empty, LANES 4, %d records, settings kept" % (
            len(records(a, V1["rec"], a[V1["nrec"]] | a[V1["nrec"] + 1] << 8)) if len(a) == 1020 else 0)
        if not bad else "; ".join(bad[:6]), "" if g_rt else ", saved again: " + (r.stdout + r.stderr).strip()[-300:] + note))
    return good


def check_param_ids(exe, bundle, tmp):
    """the host parameters' ids, ranges and modules are as they were (automation and saved projects hold ids; the
    names are free to change): plugin/tests/fixtures/param_ids.txt, from --list"""
    keys = os.path.join(tmp, "end3000.txt")
    with open(keys, "w") as f:
        f.write("3000 end\n")
    r = plug_render(exe, bundle, keys, os.path.join(tmp, "plug-list.wav"), 48000, extra=["--list"])
    now = [ln.split("\t")[:3] for ln in r.stdout.splitlines() if ln.count("\t") >= 3]
    with open(os.path.join(ROOT, "plugin/tests/fixtures/param_ids.txt")) as f:
        was = [ln.rstrip("\n").split("\t") for ln in f if ln.strip()]
    good = r.returncode == 0 and now == was
    print("%s  host parameter ids, ranges and modules unchanged (%d of %d)" % (
        "ok  " if good else "FAIL", sum(a == b for a, b in zip(now, was)), len(was)))
    return good


def heavy16_state(exe, bundle, tmp):
    """a state with sixteen heavy lanes: the four heaviest presets' lanes side by side (each preset loaded and saved
    by the plugin, its lanes put at 4j .. 4j + 3), every one at HOW MANY and HOW ACTIVE their most, LANES 16"""
    scenes = []
    for prog in HEAVY:
        sp = os.path.join(tmp, "pc-%d.txt" % prog)
        with open(sp, "w") as f:
            f.write("200 midi 0xC0 %d 0\n2500 end\n" % prog)
        st = os.path.join(tmp, "pc-%d.json" % prog)
        plug_render(exe, bundle, sp, os.path.join(tmp, "pc-%d.wav" % prog), 48000, test=False, extra=["--state-out", st])
        scenes.append(scene_bytes(st))
    b = bytearray(scenes[0][0])
    for j, (s, _) in enumerate(scenes):
        for k in range(4):
            d = 4 * j + k
            for f in ("snd", "vol", "pan", "lf"):
                b[V2[f] + d] = s[V2[f] + k]
            on = s[V2["snd"] + k] < 128
            b[V2["cnt"] + d], b[V2["step"] + d], b[V2["run"] + d] = (6, 14, 1) if on else (0, 0, 0)
    b[3] = SCN_LANES
    b[V2["mute"]:V2["mute"] + 4] = bytes(4)
    n = b[V2["nrec"]] | b[V2["nrec"] + 1] << 8
    keep = [r for r in records(b, V2["rec"], n) if (r[0] >> 8) not in (0, 1, 2, 3)]   # (no lane's card: lanes moved)
    rec = bytearray()
    for i, v in keep:
        rec += i.to_bytes(2, "little") + v.to_bytes(2, "little", signed=True)
    b[V2["nrec"]:V2["nrec"] + 2] = len(keep).to_bytes(2, "little")
    b[V2["rec"]:] = bytes(rec) + bytes(len(b) - V2["rec"] - len(rec))
    path = os.path.join(tmp, "heavy16.json")
    hdr = scenes[0][1]
    with open(path, "w") as f:
        f.write(hdr + '"scene":"' + b.hex() + '"}')
    return path


def heavy_script(path, prog):
    with open(path, "w") as f:
        f.write("500 midi 0xC0 %d 0\n" % prog)
        for ch in range(4):
            f.write("3000 midi 0x%X 21 127\n3000 midi 0x%X 22 127\n" % (0xB0 + ch, 0xB0 + ch))
        for cc, v in ((102, 90), (105, 127), (104, 90), (91, 100), (86, 110)):
            f.write("3100 midi 0xB4 %d %d\n" % (cc, v))
        for k, ch in enumerate(range(4)):
            f.write("%d midi 0x%X %d 110\n" % (4000 + 300 * k, 0x90 + ch, 48 + 5 * k))
        f.write("%d end\n" % (14000 if args_quick else 24000))


args_quick = False


GUI = [("gui4", "plugin/tests/scripts/gui4.txt"), ("gui16", "plugin/tests/scripts/gui16.txt")]
GUI_COMP = ["--param", "0x41a=1", "--param", "0x41b=127", "--param", "0x41e=4", "--param", "0x426=2"]   # gui4: COMP
#                                         on, THR -40 dBFS, 6:1 (its meters live in the pictures; the gui test checks they
#                                         are fed); EFFECTS ON LOAD NEVER, so the script's preset keeps them


GUI_VIEWS = ("", "space", "sounds", "presets", "settings", "colour", "mode", "drag", "links", "menu", "fx", "comp", "filter", "reverb", "ms", "lfo",
             "env", "matrix", "save", "random-list", "stopped", "preview", "replay", "rows", "lane-stopped", "rain")   # clap_render
#                                        --shot: x.png, x-space.png ... (the movement: LFO 1's mode, a drag in it, the
#                                        links at rest, the right-click menu; the effects' drawers: DELAY, COMP, FILTER, REVERB, M/S,
#                                        MOVEMENT's LFO 1, ENV, the matrix; STOP ALL, the sound browser's preview on a
#                                        stopped lane, PLAY ALL; the rows with lane 3 selected; a lane stopped by its own
#                                        button; Rain on a roof's drawer; RANDOM's list, the top bar's dice)
GUI_TEST_VIEWS = ("dice-lane", "random-none", "random")   # clap_render --gui-test's (gui4's second run): a row's dice
#                                        rolled (its hint hovered), ROLL with nothing ticked, ROLL with all four (kept with --shots)
GUI_SIZES = ((1200, 720), (1950, 1210), (1440, 1080), (960, 600))   # real DAW window sizes: the MAP round in its place


def check_gui(args, exe, bundle, tmp):
    ok = True
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "plugin_card_hints.py"), "--check"],
                       capture_output=True, text=True)
    print("%s  gui card hints: %s" % ("ok  " if r.returncode == 0 else "FAIL", (r.stdout + r.stderr).strip()))
    ok = ok and r.returncode == 0
    # KEYS = FM-1 KEYS: SELECTED CH's notes are the FM-1's keys (plugin/tests/scripts/keys.txt): lane 2 picked by its
    # black keys, its HOW MANY and HOW ACTIVE by the white ones, lane 5 by the first black key past G5; the app's
    # selected lane (the window's too) in the state
    st = os.path.join(tmp, "keys.json")
    r = plug_render(exe, bundle, "plugin/tests/scripts/keys.txt", os.path.join(tmp, "keys.wav"), 44100, test=False,
                    extra=["--param", "0x10001=1", "--param", "0x451=7", "--get", "0x101", "--get", "0x102",
                           "--state-out", st])
    got = dict(re.findall(r"param (0x[0-9a-f]+) = (\S+)", r.stdout))
    sel = re.search(r'"sel":"(\d+)"', open(st).read()).group(1) if os.path.exists(st) else "?"
    good = r.returncode == 0 and got.get("0x101") == "4" and got.get("0x102") == "3" and sel == "4"
    print("%s  gui KEYS = FM-1 KEYS: lane 2 HOW MANY %s, HOW ACTIVE %s, then lane %s selected" % (
        "ok  " if good else "FAIL", got.get("0x101"), got.get("0x102"), int(sel) + 1 if sel.isdigit() else sel))
    ok = ok and good
    for w, h in GUI_SIZES:   # every view at each size (the map's own check: clap_render's "gui map")
        png = os.path.join(tmp, "size-%dx%d.png" % (w, h))
        r = plug_render(exe, bundle, GUI[0][1], os.path.join(tmp, "size.wav"), 44100, test=False,
                        extra=GUI_COMP + ["--shot", png, "--shot-w", str(w), "--shot-h", str(h)])
        m = re.search(r"gui map: .*", r.stdout)
        good = r.returncode == 0 and os.path.exists(png) and m is not None
        print("%s  gui %d x %d: %s" % ("ok  " if good else "FAIL", w, h, m.group(0)[9:] if m else (r.stdout + r.stderr)[-300:]))
        ok = ok and good
        if good and args.shots:
            os.makedirs(args.shots, exist_ok=True)
            shutil.copy(png, os.path.join(args.shots, "textura-window-%dx%d.png" % (w, h)))
    for name, script in GUI:
        shas = []
        for run_i in range(2):
            png = os.path.join(tmp, "%s-%d.png" % (name, run_i))
            r = plug_render(exe, bundle, script, os.path.join(tmp, name + ".wav"), 44100, test=False,
                            extra=(GUI_COMP if name == "gui4" else []) + ["--shot", png] +
                            (["--gui-test"] if run_i == 1 and name == "gui4" else []))
            good = r.returncode == 0 and os.path.exists(png)
            if not good:
                sys.stderr.write(r.stdout + r.stderr)
                ok = False
                break
            h = hashlib.sha256()
            for view in GUI_VIEWS:   # the map, then SPACE, the browsers, SETTINGS, the colour picker: all the same twice
                with open(png if not view else png[:-4] + "-" + view + ".png", "rb") as f:
                    h.update(f.read())
            shas.append(h.hexdigest())
            for line in r.stdout.splitlines():
                if line.startswith("gui ") and run_i == 1:
                    print("info  %s: %s" % (name, line[4:]))
        if len(shas) == 2:
            same = shas[0] == shas[1]
            print("%s  gui %s: the window and its views painted twice, %s (%s)" % ("ok  " if same else "FAIL", name,
                  "the same" if same else "DIFFERENT", shas[0][:12]))
            ok = ok and same
            if args.shots:
                os.makedirs(args.shots, exist_ok=True)
                for view in GUI_VIEWS:
                    shutil.copy(os.path.join(tmp, name + "-0" + ("-" + view if view else "") + ".png"),
                                os.path.join(args.shots, "textura-window-%s%s.png" % (name, "-" + view if view else "")))
                for view in GUI_TEST_VIEWS if name == "gui4" else ():
                    src = os.path.join(tmp, name + "-1-" + view + ".png")
                    if os.path.exists(src):
                        shutil.copy(src, os.path.join(args.shots, "textura-window-%s-%s.png" % (name, view)))
    return ok


def check_cpu(args, exe, bundle, tmp):
    progs = HEAVY[:2] if args.quick else HEAVY
    for prog in progs:
        sp = os.path.join(tmp, "heavy-%d.txt" % prog)
        heavy_script(sp, prog)
        r = plug_render(exe, bundle, sp, os.path.join(tmp, "heavy-%d.wav" % prog), 48000, test=False,
                        extra=["--block", "512", "--cpu-from", "5000"])
        m = re.search(r"cpu .*", r.stdout)
        print("info  cpu F%03d (4 lanes at 127, grain delay full): %s" % (prog + 1, m.group(0) if m else "?"))
    st = heavy16_state(exe, bundle, tmp)
    sp = os.path.join(tmp, "heavy16.txt")
    with open(sp, "w") as f:
        for cc, v in ((102, 90), (105, 127), (104, 90), (91, 100), (86, 110)):
            f.write("1000 midi 0xB4 %d %d\n" % (cc, v))
        for ch in range(16):
            f.write("%d midi 0x%X %d 110\n" % (2000 + 150 * ch, 0x90 + ch, 40 + 3 * ch))
        f.write("%d end\n" % (14000 if args.quick else 24000))
    r = plug_render(exe, bundle, sp, os.path.join(tmp, "heavy16.wav"), 48000, test=False,
                    extra=["--block", "512", "--cpu-from", "5000", "--state-in", st])
    m = re.search(r"cpu .*", r.stdout)
    print("info  cpu 16 heavy lanes (F012, F034, F074, F015 side by side, all at their most, grain delay full): %s" % (
        m.group(0) if m else "? " + (r.stdout + r.stderr)[-300:]))
    return True


def main():
    global args_quick, WINE
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build/plugin")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--only", default="equal,hq,rates,state,cpu,gui")
    ap.add_argument("--shots", default="")
    ap.add_argument("--wine", action="store_true")
    args = ap.parse_args()
    args_quick = args.quick
    WINE = args.wine
    build = os.path.join(ROOT, args.build)
    exe = os.path.join(build, "txp_clap_render" + (".exe" if WINE else ""))
    bundle = os.path.join(build, "bundles", "CLAP" if WINE else "", "Textura.clap")
    tmp = os.path.join(build, "gate")
    os.makedirs(tmp, exist_ok=True)
    host = os.path.join(build, "textura-host-hq")
    host16 = os.path.join(build, "textura-host-hq16")
    only = args.only.split(",")
    if "equal" in only or "rates" in only or "hq" in only:
        host_build(host)
    if "equal" in only:
        host_build(host16, lanes=SCN_LANES)
    ok = True
    if "equal" in only:
        ok = check_equal(args, host, host16, exe, bundle, tmp) and ok
    if "hq" in only:
        ok = check_hq(args, host, tmp) and ok
    if "rates" in only:
        ok = check_rates(args, host, exe, bundle, tmp) and ok
    if "state" in only:
        ok = check_state(args, exe, bundle, tmp) and ok
        ok = check_param_ids(exe, bundle, tmp) and ok
    if "gui" in only:
        ok = check_gui(args, exe, bundle, tmp) and ok
    if "cpu" in only:
        check_cpu(args, exe, bundle, tmp)
    print("plugin gate: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
