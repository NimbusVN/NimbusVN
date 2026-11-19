#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
#  run_save_test.py - deterministic save/load acceptance for NimbusVN ONS
#
#  Copyright (c) 2026 NimbusVN contributors.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
"""Build the tiny_save fixture and verify save/load across two processes.

The script below is written once, encoded to GBK and XOR 0x84 (the format used
by the retail games).  Two separate engine processes run it:

  1. *save_phase  writes save1.dat, then changes the variable in memory and
     loads the slot back.  The report screen is green only when the variable
     was restored to 12345.
  2. *load_only   starts from a fresh process, paints the screen red, loads
     slot 1 and reaches the same report.  Green proves the value came from
     disk, because this process never executes the `mov %0,12345` line.

Neither process ends the script (no `end`) so --ons-frames dumps the report.
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parents[2]
GAME_DIR = TEST_DIR / "tiny_save"
DEFAULT_EXE = REPO_ROOT / "out" / "windows-ons" / "Release" / "onscripter.exe"
OUT_DIR = REPO_ROOT / "out"

# ---------------------------------------------------------------- fixture ---

SCRIPT_UTF8 = """\
;mode 640,480
;value 200
;gameid ons_tiny
*define
game
savenumber 3
*start
goto *save_phase

*save_phase
mov %0,12345
mov %201,1
bg "white.png"
*save_it
savegame 1:goto *after_save
*after_save
if %201=1 goto *do_load
goto *report

*do_load
mov %0,77777
mov %201,0
bg "red.png"
loadgame 1
end

*report
if %0=12345 goto *good
bg "red.png"
goto *show
*good
bg "green.png"
*show
存档测试 变量= %0
click
end

*load_only
bg "red.png"
loadgame 1
end
"""


def xor_encode(text: str) -> bytes:
    return bytes(b ^ 0x84 for b in text.encode("gbk"))


def write_png(path: Path, rgb, width=640, height=480):
    """Minimal solid-colour PNG writer (no Pillow needed)."""
    row = bytes(rgb) * width
    raw = b"".join(b"\x00" + row for _ in range(height))

    def chunk(tag: bytes, data: bytes) -> bytes:
        out = struct.pack(">I", len(data)) + tag + data
        return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def serialise_lines(path: Path):
    """The engine does not need line endings beyond \n."""
    text = SCRIPT_UTF8.replace("\r\n", "\n")
    if not text.endswith("\n"):
        text += "\n"
    path.write_bytes(text.encode("utf-8"))


def prepare_fixture(font_override: str):
    GAME_DIR.mkdir(parents=True, exist_ok=True)

    # nscript.dat: GBK + XOR 0x84 (same on-disk format as the retail games).
    (GAME_DIR / "nscript.dat").write_bytes(xor_encode(SCRIPT_UTF8))
    serialise_lines(GAME_DIR / "nscript.txt")
    write_png(GAME_DIR / "white.png", (255, 255, 255))
    write_png(GAME_DIR / "green.png", (0, 255, 0))
    write_png(GAME_DIR / "red.png", (255, 0, 0))

    font = GAME_DIR / "default.ttf"
    if not font.exists():
        candidates = [Path(font_override)] if font_override else []
        for src in candidates:
            if src.is_file():
                font.write_bytes(src.read_bytes())
                print(f"font: copied {src} -> {font} ({font.stat().st_size} bytes)")
                break
        else:
            sys.exit("font: default.ttf missing; pass --font or set NIMBUS_ONS_FONT")


# ------------------------------------------------------------------- run ---

def run_engine(exe: Path, label: str, frames: int, dump: Path, log: Path,
               save_dir: Path):
    cmd = [
        str(exe),
        str(GAME_DIR),
        "--ons-offscreen",
        "--ons-frames", str(frames),
        "--ons-start-label", label,
        "--save-dir", str(save_dir),
        "--ons-dump-frame", str(dump),
        "--ons-encoding", "gbk",
    ]
    print("run:", " ".join(cmd))
    with log.open("w", encoding="utf-8", errors="replace") as fp:
        proc = subprocess.run(cmd, cwd=str(REPO_ROOT), stdout=fp,
                              stderr=subprocess.STDOUT, timeout=180)
    fp_tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-8:]
    print("  exit:", proc.returncode)
    for line in fp_tail:
        print("  |", line)
    return proc.returncode


def image_info(path: Path):
    from PIL import Image  # only needed for the pixel assertions
    im = Image.open(path).convert("RGB")
    corner = im.getpixel((4, 4))
    lit = 0
    for y in range(0, im.height, 4):
        for x in range(0, im.width, 4):
            r, g, b = im.getpixel((x, y))
            if r > 180 and g > 180 and b > 180:
                lit += 1
    return im.size, corner, lit


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(DEFAULT_EXE))
    ap.add_argument("--font", default=os.environ.get("NIMBUS_ONS_FONT", ""))
    ap.add_argument("--frames", type=int, default=24)
    args = ap.parse_args()

    exe = Path(args.exe)
    if not exe.is_file():
        sys.exit(f"engine not found: {exe}")

    prepare_fixture(args.font)

    save_dir = OUT_DIR / "ons_save"
    save_dir.mkdir(parents=True, exist_ok=True)
    for stale in save_dir.glob("save*.dat"):
        stale.unlink()
    for stale in save_dir.glob("envdata"):
        stale.unlink()

    dump1 = OUT_DIR / "ons_save_run1.png"
    dump2 = OUT_DIR / "ons_save_run2.png"
    for p in (dump1, dump2):
        if p.exists():
            p.unlink()

    rc1 = run_engine(exe, "save_phase", args.frames, dump1,
                     OUT_DIR / "ons_save_run1.log", save_dir)
    saves = sorted(p.name for p in save_dir.glob("save*.dat"))
    print("saves after run 1:", saves)
    if rc1 != 0:
        print("FAIL: run 1 exit code", rc1)
        return 1
    slot1 = save_dir / "save1.dat"
    if not slot1.is_file():
        print("FAIL: save1.dat was not created")
        return 1
    head = slot1.read_bytes()[:5]
    if head != b"ONS\x02\x06":
        print("FAIL: save1.dat magic is", head)
        return 1
    print(f"save1.dat: {slot1.stat().st_size} bytes, magic ONS 2.6")

    rc2 = run_engine(exe, "load_only", args.frames, dump2,
                     OUT_DIR / "ons_save_run2.log", save_dir)
    log2 = (OUT_DIR / "ons_save_run2.log").read_text(encoding="utf-8",
                                                     errors="replace")
    if "ons: loadgame slot 1" not in log2:
        print("FAIL: run 2 did not log a loadgame")
        return 1
    if "ons: savegame slot 1" in log2 and \
            log2.index("ons: savegame slot 1") < log2.index("ons: loadgame slot 1"):
        print("FAIL: run 2 saved before loading; it must only load")
        return 1
    if rc2 != 0:
        print("FAIL: run 2 exit code", rc2)
        return 1

    ok = True
    for name, path in (("run1", dump1), ("run2", dump2)):
        if not path.is_file():
            print(f"FAIL: {name} did not produce {path}")
            ok = False
            continue
        size, corner, lit = image_info(path)
        print(f"{name}: {path} size={size[0]}x{size[1]} corner={corner} "
              f"lit_samples={lit}")
        if not (corner[0] < 60 and corner[1] > 120 and corner[2] < 60):
            print(f"FAIL: {name} report screen is {corner}, expected green")
            ok = False
        if lit < 40:
            print(f"FAIL: {name} report text missing (lit={lit})")
            ok = False

    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
