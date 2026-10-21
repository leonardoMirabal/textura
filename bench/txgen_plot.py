# SPDX-License-Identifier: GPL-3.0-only
"""Plot a txgen long run: the activity, the pitch centre and the notes, the recall's weight, the audio's rms.
usage: .local/venv/bin/python bench/txgen_plot.py build/gen/longrun.csv build/gen/longrun.png"""
import csv
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

rows = list(csv.DictReader(open(sys.argv[1])))
t = [int(r["second"]) / 60 for r in rows]
col = lambda k: [float(r[k]) for r in rows]
fig, ax = plt.subplots(4, 1, figsize=(13, 9), sharex=True)
ax[0].plot(t, col("activity"), lw=0.8); ax[0].set_ylabel("activity (knob)"); ax[0].set_ylim(0, 127)
ax[1].plot(t, col("pitch_centre"), lw=0.8, label="pitch centre (semitones above ROOT)")
mn = [m - 60 if n > 0 else float("nan") for m, n in zip(col("mean_note"), col("notes"))]
ax[1].plot(t, mn, ".", ms=1.5, alpha=0.5, label="mean note of the second")
ax[1].legend(loc="upper right", fontsize=8); ax[1].set_ylabel("pitch")
ax[2].plot(t, col("notes"), lw=0.5); ax[2].set_ylabel("notes / s")
ax2 = ax[2].twinx(); ax2.plot(t, col("recall"), color="C3", lw=0.8); ax2.set_ylabel("recall weight", color="C3"); ax2.set_ylim(0, 1.05)
ax[3].plot(t, col("rms_db"), lw=0.6); ax[3].set_ylabel("rms (dBFS)"); ax[3].set_xlabel("minutes")
fig.suptitle(sys.argv[1]); plt.tight_layout(); plt.savefig(sys.argv[2], dpi=80)
