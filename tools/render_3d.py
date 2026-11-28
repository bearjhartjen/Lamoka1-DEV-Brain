#!/usr/bin/env python3
"""Regenerate the Lamoka1-DEV-Brain 3D renders from the KiCad board.

Run by .githooks/pre-commit, but safe to run by hand:

    python3 tools/render_3d.py          # render only if the top view changed
    python3 tools/render_3d.py --force  # always re-render
    python3 tools/render_3d.py --check  # report what would happen, render nothing

Rule: the top view is the gate. If a fresh top-view render is visually
identical to the committed one, nothing is written. Otherwise all three
views are re-rendered and replaced in place.

Needs: kicad-cli (KiCad 9+), python3 + Pillow. ~40 s when it renders.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageChops

# --- paths (repo-relative; the hook runs from the repo root) ----------------
REPO = Path(__file__).resolve().parent.parent
BOARD = REPO / "HARDWARE" / "Kicad files" / "Lamoka1-DEV-board.kicad_pcb"
IMAGES = REPO / "Images"
TOP = IMAGES / "lamoka1-dev-brain-render-top.png"
BOTTOM = IMAGES / "lamoka1-dev-brain-render-bottom.png"
ISO = IMAGES / "lamoka1-dev-brain-render-iso.png"

# --- render settings --------------------------------------------------------
LONG_EDGE = 1080          # largest image dimension, per user request
TOP_RATIO = (7, 3)        # the DEV-Brain is a 70 x 30 mm board
ISO_RATIO = (3, 2)
ZOOM_TOP = 2.0            # fill the frame without clipping
ZOOM_ISO = 1.3
PRESET = "follow_plot_settings"
QUALITY = "high"
ROTATE_ISO = "-45,0,45"

# Pixels whose strongest channel differs by <= DIFF_FLOOR are ignored as
# render noise. A change below MIN_CHANGED pixels is not visible to a human.
DIFF_FLOOR = 12
MIN_CHANGED = 600          # ~0.05 % of a 1080 x 1080 image


def dims(ratio: tuple[int, int]) -> tuple[int, int]:
    """Scale a ratio so the long edge is LONG_EDGE."""
    w, h = ratio
    if w >= h:
        return LONG_EDGE, round(LONG_EDGE * h / w)
    return round(LONG_EDGE * w / h), LONG_EDGE


def find_kicad_cli() -> str:
    found = shutil.which("kicad-cli")
    if not found:
        sys.exit(
            "render_3d: kicad-cli not found on PATH.\n"
            "  If this ran from a Flatpak app (GitKraken), the hook needs\n"
            "  'flatpak-spawn --host' permission: flatpak override --user \\\n"
            "    --talk-name=org.freedesktop.Flatpak com.axosoft.GitKraken"
        )
    return found


def render(out: Path, side: str | None, rotate: str | None) -> None:
    w, h = dims(TOP_RATIO if side in ("top", "bottom") else ISO_RATIO)
    cmd = [
        find_kicad_cli(), "pcb", "render", str(BOARD),
        "-o", str(out),
        "-w", str(w), "-h", str(h),
        "--quality", QUALITY,
        "--preset", PRESET,
        "--use-board-stackup-colors",   # black mask + HASL pads, as fabricated
        "--background", "transparent",
        "--zoom", str(ZOOM_TOP if side in ("top", "bottom") else ZOOM_ISO),
    ]
    if side:
        cmd += ["--side", side]
    if rotate:
        cmd += ["--rotate", rotate]

    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if proc.returncode != 0 or not out.exists():
        sys.stderr.write(
            "render_3d: kicad-cli failed\n"
            f"  command: {' '.join(cmd)}\n"
            f"  stdout: {proc.stdout.strip()[-2000:]}\n"
            f"  stderr: {proc.stderr.strip()[-2000:]}\n"
        )
        sys.exit(1)


def write_webp(src: Path, dst: Path) -> None:
    """Write the WebP twin the website links to (small, fast first paint)."""
    with Image.open(src) as im:
        im.save(dst, "WEBP", quality=82, method=6)


def changed(a: Path, b: Path) -> bool:
    """True if the two images differ by a visible amount."""
    with Image.open(a) as ia, Image.open(b) as ib:
        if ia.size != ib.size:
            return True
        diff = ImageChops.difference(ia.convert("RGBA"), ib.convert("RGBA"))
    bands = diff.split()
    strongest = bands[0]
    for band in bands[1:]:
        strongest = ImageChops.lighter(strongest, band)
    return sum(strongest.histogram()[DIFF_FLOOR + 1:]) >= MIN_CHANGED


def dirty() -> list[str]:
    """Paths among the board + renders that differ from HEAD or are untracked."""
    paths = [BOARD, TOP, BOTTOM, ISO]
    proc = subprocess.run(
        ["git", "status", "--porcelain", "--", *[str(p) for p in paths]],
        cwd=REPO, capture_output=True, text=True,
    )
    if proc.returncode != 0:      # not a git checkout: don't block on it
        return ["<no git>"]
    return [ln for ln in proc.stdout.splitlines() if ln.strip()]



def prune_orphans() -> None:
    """Delete tracked render images this script no longer produces."""
    keep = {p.name for p in (TOP, BOTTOM, ISO)}
    keep |= {p.with_suffix(".webp").name for p in (TOP, BOTTOM, ISO)}
    proc = subprocess.run(
        ["git", "ls-files", "--", str(IMAGES)],
        cwd=REPO, capture_output=True, text=True,
    )
    for line in proc.stdout.splitlines():
        rel = Path(line)
        if rel.parent != IMAGES.relative_to(REPO) or "render" not in rel.name.lower():
            continue
        if rel.name in keep:
            continue
        (REPO / rel).unlink(missing_ok=True)
        print(f"render_3d: removed stale {rel} (no longer generated)")


def check_readme_images() -> list[str]:
    """README must never point at an image that is not in the repo."""
    readme = REPO / "README.md"
    if not readme.exists():
        return []
    text = readme.read_text(encoding="utf-8", errors="replace")
    refs = re.findall(r'(?:src="|\]\()\.?/?(Images/[^")\s]+)', text)
    return [r for r in refs if not (REPO / r).exists()]


def missing_models() -> list[str]:
    """3D models the board refers to but cannot find on this machine.

    A render with models missing silently produces a bare-board picture, so
    refuse to write anything instead of committing a degraded image.
    """
    text = BOARD.read_text(encoding="utf-8", errors="replace")
    refs = re.findall(r'\(model "([^"]+)"', text)
    vars = {
        "KIPRJMOD": BOARD.parent,
        "KICAD10_3DMODEL_DIR": Path("/usr/share/kicad/3dmodels"),
        "KICAD9_3DMODEL_DIR": Path("/usr/share/kicad/3dmodels"),
        "KISYSMOD": Path("/usr/share/kicad/3dmodels"),
    }
    for env, path in vars.items():
        if not path.exists():
            print(f"render_3d: warning: {env} points at a missing dir: {path}")
    missing = []
    for ref in set(refs):
        path = ref
        for name, base in vars.items():
            path = path.replace("${%s}" % name, str(base))
        if "$" in path:            # unknown variable: cannot verify
            missing.append(ref)
        elif not Path(path).exists():
            missing.append(ref)
    return sorted(missing)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--force", action="store_true", help="re-render even if unchanged")
    ap.add_argument("--check", action="store_true", help="report only, write nothing")
    args = ap.parse_args()

    if not BOARD.exists():
        sys.exit(f"render_3d: board not found: {BOARD}")

    problems = check_readme_images()
    for ref in problems:
        sys.stderr.write(f"render_3d: README references a missing image: {ref}\n")

    # Fast path: nothing staged or edited that could change the pictures.
    if not args.force and not args.check and not dirty():
        print("render_3d: board and renders match the last commit, nothing to do.")
        return 1 if problems else 0

    missing = missing_models()
    if missing:
        sys.stderr.write(
            f"render_3d: {len(missing)} 3D model(s) missing; refusing to render.\n"
            + "".join(f"  {m}\n" for m in missing[:10])
            + "  Rendering now would replace good images with a bare board.\n"
        )
        return 2

    with tempfile.TemporaryDirectory(prefix="lamoka1-render-") as tmp:
        tmpdir = Path(tmp)
        new_top = tmpdir / "top.png"
        print("render_3d: rendering top view (change gate)...", flush=True)
        render(new_top, "top", None)

        stale = args.force or not TOP.exists() or changed(TOP, new_top)
        if not stale:
            print("render_3d: top view unchanged, keeping existing renders.")
            return 0

        if args.check:
            print("render_3d: top view CHANGED (would re-render all three).")
            return 0

        print("render_3d: visible change, rendering bottom + iso...", flush=True)
        new_bottom = tmpdir / "bottom.png"
        new_iso = tmpdir / "iso.png"
        render(new_bottom, "bottom", None)
        render(new_iso, None, ROTATE_ISO)

        for src, dst in ((new_top, TOP), (new_bottom, BOTTOM), (new_iso, ISO)):
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(src), dst)
            print(f"render_3d: wrote {dst.relative_to(REPO)} ({dst.stat().st_size // 1024} KiB)")
            webp = dst.with_suffix(".webp")
            write_webp(dst, webp)
            print(f"render_3d: wrote {webp.relative_to(REPO)} ({webp.stat().st_size // 1024} KiB)")
        prune_orphans()
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
