#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
#  run_stability.py - start/exit and long-run checks for NimbusVN ONS
#
#  Copyright (c) 2026 NimbusVN contributors.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
"""Short-cycle shutdown check (10x) and optional 6000-frame runs.

The engine writes envdata/gloval.sav on exit, so every run here is given an
explicit --save-dir under out/ ; nothing is written into the game folders.
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parents[2]
DEFAULT_EXE = REPO_ROOT / "out" / "windows-ons" / "Release" / "onscripter.exe"
OUT_DIR = REPO_ROOT / "out"


def run(exe, root, save_dir, frames, auto_click, click, dump, log_path, timeout):
    save_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(exe), str(root), "--ons-offscreen",
        "--ons-frames", str(frames),
        "--ons-auto-click", str(auto_click),
        "--ons-click", click,
        "--save-dir", str(save_dir),
        "--ons-dump-frame", str(dump),
    ]
    start = time.time()
    with log_path.open("w", encoding="utf-8", errors="replace") as fp:
        proc = subprocess.run(cmd, cwd=str(REPO_ROOT), stdout=fp,
                              stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, time.time() - start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--game", help="game directory for the start/exit cycles")
    ap.add_argument("--clannad", help="optional game directory for long run A")
    ap.add_argument("--ever17", help="optional game directory for long run B")
    ap.add_argument("--click", default="470,220",
                    help="logical click point for the cycles (default 470,220)")
    ap.add_argument("--cycles", type=int, default=10)
    ap.add_argument("--long", action="store_true",
                    help="also run 6000-frame tests for --clannad/--ever17")
    ap.add_argument("--timeout", type=int, default=180)
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.is_file():
        sys.exit(f"engine not found: {exe}")

    cycle_game = Path(args.game) if args.game else None
    if cycle_game is None and args.clannad:
        cycle_game = Path(args.clannad)
    if cycle_game is None and args.ever17:
        cycle_game = Path(args.ever17)
    if cycle_game is None:
        sys.exit("pass --game <dir> (or --clannad/--ever17) for the cycles")

    print(f"== {args.cycles}x start/exit ==")
    worst = 0.0
    for i in range(1, args.cycles + 1):
        log = OUT_DIR / f"ons_s6_cycle_{i:02d}.log"
        rc, secs = run(exe, cycle_game, OUT_DIR / "ons_cycle", 120, 60,
                       args.click,
                       OUT_DIR / f"ons_s6_cycle_{i:02d}.png", log,
                       args.timeout)
        worst = max(worst, secs)
        print(f"cycle {i:02d}: exit={rc} wall={secs:.1f}s")
        if rc != 0:
            print("FAIL: non-zero exit")
            print(log.read_text(encoding="utf-8", errors="replace")[-800:])
            return 1
    print(f"cycles OK (worst {worst:.1f}s)")

    if not args.long:
        return 0

    long_runs = []
    if args.clannad:
        long_runs.append(("clannad", Path(args.clannad), 50, 600, args.click))
    if args.ever17:
        long_runs.append(("ever17", Path(args.ever17), 60, 600, "405,327"))
    if not long_runs:
        sys.exit("--long needs --clannad and/or --ever17")

    for name, root, auto_ms, wall_cap, click in long_runs:
        log = OUT_DIR / f"ons_s6_long_{name}.log"
        dump = OUT_DIR / f"ons_s6_long_{name}.png"
        print(f"== {name} 6000 frames ==")
        rc, secs = run(exe, root, OUT_DIR / f"ons_save_{name}", 6000, auto_ms,
                       click, dump, log, wall_cap)
        print(f"{name}: exit={rc} wall={secs:.1f}s")
        text = log.read_text(encoding="utf-8", errors="replace")
        for line in text.splitlines():
            if "working_set" in line or "no frame for" in line:
                print("  ", line)
        if rc != 0:
            print("FAIL: long run exited non-zero")
            return 1
        if not dump.is_file():
            print("FAIL: no screenshot")
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
