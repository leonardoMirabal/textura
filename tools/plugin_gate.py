#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The desktop plugin plays what the FM-1 plays (make test-plugin).

The plugin is the app compiled unchanged (plugin/src/engine_unit.c), so its render of a MIDI script must be the host
build's render of the same script (tests/host.c), built with the same switch (LN_HQ: no CPU limit, nothing thinned).

  equal   44.1 kHz, the plugin's test time (TEXTURA_PLUGIN_TEST=ms: the host build's events-per-millisecond order):
          after the latency, the plugin's output is the host build's, sample for sample (the host writes 16 bits with
          a clip: the plugin's float x 32768, clipped the same way, must be equal)
  rates   48, 96 and 88.2 kHz (the oversampler) and 32 kHz (the sinc), the test time: the host build's render taken to
          that rate offline (a long polyphase filter) against the plugin's: the residual under -90 dB of the signal
          below 12 kHz (or a quarter of the rate; the two resamplers' own filters differ in their transition bands)
          (stop: a lane stopped; lanes: notes, CCs and presets on every lane and channel 5; hq: a heavy 4-lane scene;
          without --quick also presets: every factory preset by program change)
  hq      the host build with LN_HQ plays plugin/tests/scripts/hq.txt: the governor's pool has no end, nothing is cut
  state   the shipped time (exact): a render, its state saved and loaded into a second instance, every parameter
          reads back the same and the state saves the same again; and an instance never activated, every parameter
          set at random by a flush (as clap-validator does), the same
  cpu     the shipped time at 48 kHz, 512-frame buffers: average and worst time per process call for heavy 4-lane
          scenes (every lane at HOW MANY and HOW ACTIVE 127, the grain delay at all its grains, the reverb up)

  tools/plugin_gate.py [--build build/plugin] [--quick] [--only equal,hq,rates,state,cpu]
Exit 0 when every check passes. Needs numpy and scipy for the rates (skipped without them)."""
import argparse
import os
import re
import subprocess
import sys
import wave
from array import array

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS = ["tests/scripts/stop.txt", "plugin/tests/scripts/lanes.txt", "plugin/tests/scripts/hq.txt"]
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


def host_build(out):
    eng = sorted(os.path.join("engines", f) for f in os.listdir(os.path.join(ROOT, "engines")) if f.endswith(".c"))
    deps = [os.path.join(ROOT, d) for d in eng + ["tests/host.c", "web/web_hal.c"]]
    for d in ("firmware/src", "firmware/gen", "engines"):
        deps += [os.path.join(ROOT, d, f) for f in os.listdir(os.path.join(ROOT, d)) if f.endswith((".c", ".h"))]
    if os.path.exists(out) and os.path.getmtime(out) > max(os.path.getmtime(d) for d in deps):
        return
    cc = os.environ.get("CC", "cc")
    run(["nice", "-n", "15", cc] + CFLAGS + ["-o", out] + HOST_SRC + eng + ["-lm"])


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


def plug_render(exe, bundle, script, out, rate, test=True, extra=()):
    env = dict(os.environ)
    if test:
        env["TEXTURA_PLUGIN_TEST"] = "ms"
    else:
        env.pop("TEXTURA_PLUGIN_TEST", None)
    r = run([exe, bundle, os.path.join(ROOT, script), out, "--rate", str(rate)] + list(extra), env=env, check=False)
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


def check_equal(args, host, exe, bundle, tmp):
    ok = True
    for s in SCRIPTS + ([] if args.quick else LONG):
        name = os.path.basename(s)[:-4]
        h, hrc = host_render(host, s, os.path.join(tmp, "host-" + name))
        r = plug_render(exe, bundle, s, os.path.join(tmp, "plug-" + name + ".wav"), 44100)
        if r.returncode != 0:
            print("FAIL  equal %s: the plugin's render exited %d" % (name, r.returncode))
            ok = False
            continue
        p = read_float_wav(os.path.join(tmp, "plug-" + name + ".wav"))
        n, diff, first, peak = compare(h, p)
        lat = re.search(r"latency (\d+)", r.stdout)
        good = diff == 0 and n > 44100 and peak > 0
        line = "%s  equal %-7s %d frames (latency %s), peak %d, %d samples differ" % (
            "ok  " if good else "FAIL", name, n // 2, lat.group(1) if lat else "?", peak, diff)
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


SCN_STEP, SCN_RUN = 24, 28   # scenes.c scn_t: step[4] (HOW ACTIVE) and run[4] at these bytes (4 lanes)


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
    ok_i = [i for i in bad if SCN_STEP <= i < SCN_STEP + 4 and a[SCN_RUN + i - SCN_STEP] == 0
            and b[SCN_RUN + i - SCN_STEP] == 0]
    good = ha == hb and len(a) == len(b) and len(ok_i) == len(bad)
    return good, " (saved again: %d bytes differ, %d of them a stopped lane's HOW ACTIVE)" % (len(bad), len(ok_i))


def check_state(args, exe, bundle, tmp):
    out = os.path.join(tmp, "plug-state.wav")
    r = plug_render(exe, bundle, SCRIPTS[1], out, 48000, test=False, extra=["--state-check"])
    m = re.search(r"state (\d+) bytes, saved again \w+( \w+)?, (\d+) parameters, (\d+) differ", r.stdout)
    good, note = state_ok(r, out)
    print("%s  state: %s%s" % ("ok  " if good else "FAIL", m.group(0) if m else (r.stdout + r.stderr).strip(), note))
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


def check_cpu(args, exe, bundle, tmp):
    progs = HEAVY[:2] if args.quick else HEAVY
    for prog in progs:
        sp = os.path.join(tmp, "heavy-%d.txt" % prog)
        heavy_script(sp, prog)
        r = plug_render(exe, bundle, sp, os.path.join(tmp, "heavy-%d.wav" % prog), 48000, test=False,
                        extra=["--block", "512", "--cpu-from", "5000"])
        m = re.search(r"cpu .*", r.stdout)
        print("info  cpu F%03d (4 lanes at 127, grain delay full): %s" % (prog + 1, m.group(0) if m else "?"))
    return True


def main():
    global args_quick
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build/plugin")
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--only", default="equal,hq,rates,state,cpu")
    args = ap.parse_args()
    args_quick = args.quick
    build = os.path.join(ROOT, args.build)
    exe = os.path.join(build, "txp_clap_render")
    bundle = os.path.join(build, "bundles", "Textura.clap")
    tmp = os.path.join(build, "gate")
    os.makedirs(tmp, exist_ok=True)
    host = os.path.join(build, "textura-host-hq")
    only = args.only.split(",")
    if "equal" in only or "rates" in only or "hq" in only:
        host_build(host)
    ok = True
    if "equal" in only:
        ok = check_equal(args, host, exe, bundle, tmp) and ok
    if "hq" in only:
        ok = check_hq(args, host, tmp) and ok
    if "rates" in only:
        ok = check_rates(args, host, exe, bundle, tmp) and ok
    if "state" in only:
        ok = check_state(args, exe, bundle, tmp) and ok
    if "cpu" in only:
        check_cpu(args, exe, bundle, tmp)
    print("plugin gate: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
