#!/usr/bin/env python3
"""Generate firmware/src/ui/pet_sprites.h from the official Codex spritesheet.

Reproducible 1-bit packing used by the companion page:

  * Official v2 atlas is 1536x2288, 8x11 cells of 192x208.
  * Frames: idle (r0c0), running (r7c0), running_b (r7c2),
    waiting (r6c2), review (r8c1), failed (r5c2).
  * Each cell is composited on white, LANCZOS-scaled to 128x139,
    then Floyd-Steinberg dithered to 1-bit. No midtone darken
    (that left a speckled halo in earlier trials).
  * Bits are packed MSB-leftmost, 16 bytes per row, for u8g2 drawBitmap.

The color spritesheet is copyrighted OpenAI art and is not stored in
this repo. The script looks for it in this order:

  1. --input PATH (or $CODEX_SPRITESHEET)
  2. A previously extracted webp next to this script
  3. ChatGPT.app's app.asar, via `npx --yes @electron/asar extract-file`

Usage (from the firmware directory or the repo root):

  python3 tools/gen_pet_sprites.py
  python3 tools/gen_pet_sprites.py --input /path/to/codex-spritesheet-v6.webp
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageOps

CELL_W, CELL_H = 192, 208
PET_W = 128
PET_H = int(round(CELL_H * PET_W / CELL_W))  # 139
ROW_BYTES = (PET_W + 7) // 8  # 16
ATLAS_W, ATLAS_H = 1536, 2288
COLS, ROWS = 8, 11

# (enum name, row, col) — order is the firmware PetSpriteId order.
FRAMES = (
    ("Idle", 0, 0),
    ("Running", 7, 0),
    ("RunningB", 7, 2),
    ("Waiting", 6, 2),
    ("Review", 8, 1),
    ("Failed", 5, 2),
)

ASAR_NAME_RE = re.compile(r"codex-spritesheet-v6[^/\\]*\.webp$")


def repo_paths() -> tuple[Path, Path]:
    here = Path(__file__).resolve()
    # firmware/tools/gen_pet_sprites.py -> firmware/
    firmware = here.parent.parent
    header = firmware / "src" / "ui" / "pet_sprites.h"
    return firmware, header


def find_in_asar(asar: Path) -> str | None:
    try:
        listing = subprocess.check_output(
            ["npx", "--yes", "@electron/asar", "list", str(asar)],
            text=True,
            stderr=subprocess.STDOUT,
        )
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"asar list failed: {exc}", file=sys.stderr)
        return None
    for line in listing.splitlines():
        path = line.strip().lstrip("/")
        if ASAR_NAME_RE.search(path):
            return path
    return None


def extract_from_app(app_asar: Path, dest: Path) -> Path | None:
    inner = find_in_asar(app_asar)
    if not inner:
        print(f"no codex-spritesheet-v6*.webp in {app_asar}", file=sys.stderr)
        return None
    dest.parent.mkdir(parents=True, exist_ok=True)
    # extract-file writes the basename into the current working directory.
    with tempfile.TemporaryDirectory() as tmp:
        try:
            subprocess.check_call(
                ["npx", "--yes", "@electron/asar", "extract-file", str(app_asar), inner],
                cwd=tmp,
            )
        except (OSError, subprocess.CalledProcessError) as exc:
            print(f"asar extract-file failed: {exc}", file=sys.stderr)
            return None
        produced = Path(tmp) / Path(inner).name
        if not produced.is_file():
            print(f"extract-file did not write {produced.name}", file=sys.stderr)
            return None
        dest.write_bytes(produced.read_bytes())
    print(f"extracted {inner} -> {dest}")
    return dest


def resolve_sheet(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).expanduser()
        if not path.is_file():
            sys.exit(f"spritesheet not found: {path}")
        return path
    env = os.environ.get("CODEX_SPRITESHEET", "").strip()
    if env:
        path = Path(env).expanduser()
        if not path.is_file():
            sys.exit(f"$CODEX_SPRITESHEET not found: {path}")
        return path

    firmware, _ = repo_paths()
    local_candidates = [
        Path(__file__).resolve().parent / "codex-spritesheet-v6.webp",
        firmware / "tools" / "codex-spritesheet-v6.webp",
    ]
    for cand in local_candidates:
        if cand.is_file():
            return cand

    app_asar = Path("/Applications/ChatGPT.app/Contents/Resources/app.asar")
    cache = Path(__file__).resolve().parent / ".cache" / "codex-spritesheet-v6.webp"
    if app_asar.is_file():
        extracted = extract_from_app(app_asar, cache)
        if extracted:
            return extracted
    sys.exit(
        "Could not find the official Codex spritesheet. Pass --input PATH, "
        "set CODEX_SPRITESHEET, or install ChatGPT.app so the script can "
        "extract webview/assets/codex-spritesheet-v6*.webp from app.asar."
    )


def frame_1bit(atlas: Image.Image, row: int, col: int) -> Image.Image:
    if atlas.size != (ATLAS_W, ATLAS_H):
        sys.exit(f"unexpected atlas size {atlas.size}, expected {ATLAS_W}x{ATLAS_H}")
    if row >= ROWS or col >= COLS:
        sys.exit(f"cell r{row}c{col} is outside the {COLS}x{ROWS} atlas")
    cell = atlas.crop((col * CELL_W, row * CELL_H, (col + 1) * CELL_W, (row + 1) * CELL_H))
    bg = Image.new("RGBA", cell.size, (255, 255, 255, 255))
    bg.alpha_composite(cell)
    gray = ImageOps.grayscale(bg.convert("RGB"))
    gray = gray.resize((PET_W, PET_H), Image.Resampling.LANCZOS)
    return gray.convert("1", dither=Image.FLOYDSTEINBERG)


def pack_msb(im: Image.Image) -> bytes:
    """Pack a 1-bit image, MSB = leftmost pixel, 16 bytes per row."""
    if im.size != (PET_W, PET_H):
        sys.exit(f"frame size {im.size}, expected {PET_W}x{PET_H}")
    src = im.convert("L")
    out = bytearray(PET_H * ROW_BYTES)
    for y in range(PET_H):
        for x in range(PET_W):
            # PIL mode "1"/L: 0 is black ink, 255 is paper.
            if src.getpixel((x, y)) < 128:
                out[y * ROW_BYTES + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def c_array(name: str, data: bytes, indent: str = "  ") -> str:
    lines = [f"{indent}// {name}: {len(data)} bytes"]
    chunk = 16
    for off in range(0, len(data), chunk):
        part = ", ".join(f"0x{b:02X}" for b in data[off : off + chunk])
        comma = "," if off + chunk < len(data) else ""
        lines.append(f"{indent}{part}{comma}")
    return "\n".join(lines)


HEADER_PREFIX = """\
#pragma once

// Auto-generated 1-bit Codex pet sprites. Do not edit by hand.
// Regenerate with: python3 firmware/tools/gen_pet_sprites.py
//
// Layout: 128x139, 16 bytes/row, MSB is the leftmost pixel (u8g2 drawBitmap).
// Source: official ChatGPT.app codex-spritesheet-v6 atlas, cells listed below,
// composited on white, LANCZOS scale, Floyd-Steinberg. No midtone darken.

#include <stdint.h>

#ifdef ARDUINO
#include <pgmspace.h>
#else
#ifndef PROGMEM
#define PROGMEM
#endif
#endif

constexpr int kPetSpriteW = 128;
constexpr int kPetSpriteH = 139;
constexpr int kPetSpriteRowBytes = 16;
constexpr int kPetSpriteCount = 6;
constexpr int kPetSpriteBytes = kPetSpriteH * kPetSpriteRowBytes;  // 2224

enum PetSpriteId : int {
  kPetSpriteIdle = 0,
  kPetSpriteRunning = 1,
  kPetSpriteRunningB = 2,
  kPetSpriteWaiting = 3,
  kPetSpriteReview = 4,
  kPetSpriteFailed = 5,
};

// Packed bits for each frame, in PetSpriteId order.
const uint8_t kPetSprites[kPetSpriteCount][kPetSpriteBytes] PROGMEM = {
"""


def write_header(path: Path, frames: list[tuple[str, bytes, int, int]]) -> None:
    parts = [HEADER_PREFIX]
    for i, (name, data, row, col) in enumerate(frames):
        parts.append(f"  {{  // {name}  atlas r{row}c{col}\n")
        parts.append(c_array(name, data, "    "))
        parts.append("\n  }")
        parts.append(",\n" if i + 1 < len(frames) else "\n")
    parts.append("};\n\n")
    parts.append(
        "inline const uint8_t* petSpriteBits(int id) {\n"
        "  if (id < 0 || id >= kPetSpriteCount) id = kPetSpriteIdle;\n"
        "  return kPetSprites[id];\n"
        "}\n"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(parts), encoding="utf-8")
    print(f"wrote {path} ({path.stat().st_size} bytes, {len(frames)} frames, {PET_W}x{PET_H})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", help="path to codex-spritesheet-v6.webp")
    parser.add_argument("--output", help="path to pet_sprites.h (default: firmware/src/ui/pet_sprites.h)")
    parser.add_argument(
        "--png-dir",
        help="optional directory to also write the 1-bit PNG frames (debug/preview)",
    )
    args = parser.parse_args()

    sheet = resolve_sheet(args.input)
    atlas = Image.open(sheet).convert("RGBA")
    frames: list[tuple[str, bytes, int, int]] = []
    png_dir = Path(args.png_dir) if args.png_dir else None
    if png_dir:
        png_dir.mkdir(parents=True, exist_ok=True)
    for name, row, col in FRAMES:
        im = frame_1bit(atlas, row, col)
        data = pack_msb(im)
        ink = sum(bin(b).count("1") for b in data)
        print(f"  {name:8} r{row}c{col}  ink={ink} / {PET_W * PET_H}")
        if png_dir:
            im.save(png_dir / f"bit_{name.lower()}_{PET_W}.png")
        frames.append((name, data, row, col))

    _, default_header = repo_paths()
    output = Path(args.output).expanduser() if args.output else default_header
    write_header(output, frames)
    return 0


if __name__ == "__main__":
    sys.exit(main())
