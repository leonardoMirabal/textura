#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Install a Textura package (build/fm1/textura-app.fwsc, textura-bench.fwsc) on the FM-1: Frodhes' installer
(tools/fm1_install.py at the rev tools/fm1_bench.py builds from, the same M-UPGRADE SysEx update) with Textura's port
choice (tools/tx_ports.py):

  tools/tx_install.py PACKAGE.fwsc [--port NAME] [--yes] [--force]
  tools/tx_install.py --info [--port NAME]

The FM-1 is looked for under "Textura" first, then the update loader's names (a unit left in update mode); a port
named "Frodhes" is never probed unless --port names it, and never while a "Textura" port is present. A unit still on
Textura's old firmware (named "Frodhes") takes --port Frodhes for its first update; keep a Frodhes FM-1 unplugged then,
as after the restart its "Frodhes" port could be taken for ours.

The installer is read from build/fm1/tree (staged by make fm1-app / fm1-bench), else from Frodhes' git (git archive
into build/fm1/install: its checkout is never touched). Needs mido with python-rtmidi (.local/venv).
"""
import argparse
import importlib.util
import io
import re
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))                  # (python -I: the tools' own helper)
import tx_ports  # noqa: E402

ROOT = HERE.parent
REV = "v0.9.5"                                 # (tools/fm1_bench.py's --rev)
STAGED = ROOT / "build" / "fm1" / "tree" / "tools" / "fm1_install.py"
OWN = ROOT / "build" / "fm1" / "install" / "fm1_install.py"


def installer_path():
    if STAGED.exists():
        return STAGED
    if not OWN.exists():
        fr = next((d.parent / "frodhes" for d in (ROOT, *ROOT.parents) if (d.parent / "frodhes" / ".git").exists()),
                  ROOT.parent / "frodhes")
        tar = subprocess.run(["git", "-C", str(fr), "archive", "--format=tar", REV, "tools/fm1_install.py"],
                             capture_output=True)
        if tar.returncode:
            raise SystemExit(f"tx_install: no {STAGED} and git archive {REV} in {fr} failed: "
                             f"{tar.stderr.decode().strip()}")
        OWN.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(tar.stdout)) as t:
            OWN.write_bytes(t.extractfile("tools/fm1_install.py").read())
    return OWN


def load(path=None):
    """Frodhes' installer module, its port choice made Textura's"""
    spec = importlib.util.spec_from_file_location("fm1_install", path or installer_path())
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    also = r"textura|" + m.PORT_RE.pattern
    m.PORT_RE = re.compile(also, re.I)

    def candidates(self, only_port):
        names = self.backend.input_names()
        named = tx_ports.pick(names, self.port) if self.port else []
        if self.port and only_port:
            return named
        mine = self.port and "frodhes" not in self.port.lower()   # (a second FM-1 on Frodhes is never ours: the
        return named + [n for n in tx_ports.pick(names, None, also)  # loader's port may differ, Frodhes' never)
                        if n not in named and not (mine and "frodhes" in n.lower())]

    m.Updater.candidates = candidates
    return m


def main(argv=None, backend=None, out=sys.stdout, ask=None, path=None):
    m = load(path)
    kw = {"ask": ask} if ask else {}
    rc = m.main(argv, backend, out, **kw)
    if rc == m.EXIT.get("notfound", 3):
        ap = argparse.ArgumentParser(add_help=False)
        ap.add_argument("--port")
        port = ap.parse_known_args(sys.argv[1:] if argv is None else argv)[0].port
        try:
            names = (backend or m.MidoBackend()).input_names()
            print(f"tx_install: {tx_ports.why_none(names, port)}", file=sys.stderr)
        except Exception:                       # noqa: BLE001 (the hint only)
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
