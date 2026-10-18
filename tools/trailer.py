#!/usr/bin/env python3
"""Render the Urbz trailer: `python tools/trailer.py [--manifest PATH] [--out PATH]`.

Reads a capture manifest (JSON list of {file, what, best_in, best_out, note})
plus the clips beside it, and renders a 25-35 s, 1920x1080 @ 60 fps,
H.264/yuv420p trailer with title/end cards, lower-thirds, xfade dissolves
and an audio bed taken from the game's own intro movie.

`python tools/trailer.py --placeholders` first builds proof clips from
stills in out/ (zoompan) into out/trailer/scratch/ and renders from those.
Re-rendering after a clip changes is this one command.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

# the repo root is the parent of tools/. every binary is invoked by absolute path
# because the shells used here cannot exec a .exe through a relative path.
ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "tools" / "bin"
EXE = ".exe" if os.name == "nt" else ""
FFMPEG = BIN / f"ffmpeg{EXE}"
FFPROBE = BIN / f"ffprobe{EXE}"
INTRO_XMV = ROOT / "assets" / "movies" / "urbz_intro.xmv"
DEFAULT_MANIFEST = ROOT / "out" / "trailer" / "raw" / "manifest.json"
DEFAULT_OUT = ROOT / "out" / "trailer" / "urbz_trailer.mp4"
SCRATCH = ROOT / "out" / "trailer" / "scratch"

FONT_BOLD = Path(r"C:/Windows/Fonts/segoeuib.ttf")
FONT_SEMI = Path(r"C:/Windows/Fonts/seguisb.ttf")
FONT_REG = Path(r"C:/Windows/Fonts/segoeui.ttf")

W, H, FPS = 1920, 1080, 60
TITLE_DUR, END_DUR = 3.2, 4.4
XFADE = 0.5
TARGET_TOTAL = 28.5
INTRO_AUDIO_START = 16.0  # seconds into urbz_intro.xmv; musical bed under the cut
GOLD = "0xD8B45A"

# Truthful lines only — each traceable to README.md / docs/status.md.
TITLE_LINES = [
    ("THE URBZ", FONT_BOLD, 96, "white"),
    ("S I M S   I N   T H E   C I T Y", FONT_SEMI, 34, GOLD),
    ("Native PC port — statically recompiled from the Xbox XBE", FONT_REG, 30, "0xC9D1D9"),
]
END_LINES = [
    ("github.com/abilliontokens/urbz-recomp", FONT_SEMI, 46, "white"),
    ("v0.3.1", FONT_REG, 30, "0xC9D1D9"),
    ("Bring your own disc.", FONT_REG, 26, "0x8A94A0"),
]
# Cut list: manifest file -> (kicker, line) lower-third, in trailer order.
# A planned slug missing from the manifest is filled from the remaining
# entries in manifest order, so a dropped shot never leaves a gap.
SHOT_PLAN: list[tuple[str, tuple[str, str] | None]] = [
    ("station_establish.mp4", None),
    ("camera_zoom.mp4", ("PERFORMANCE", "60 fps at every camera distance")),
    ("walk.mp4", ("FRAME RATE", "Any frame rate, real game speed — 183–278 fps uncapped")),
    ("hud_motives.mp4", ("MODERN HUD", "4K vector HUD over a 640×480 original")),
    ("pie_menu.mp4", ("INPUT", "Keyboard, mouse and controllers")),
    ("fps_graph.mp4", None),
]
PLACEHOLDER_STILLS = [
    "gameplay_city.png", "n31_city1.png", "sw33_startmenu.png",
    "handplay_ready.png", "gameplay_rooftop.png", "gameplay_city2.png",
]


def run(cmd: list[str]) -> None:
    print("+", " ".join(f'"{c}"' if " " in c else c for c in cmd))
    subprocess.run(cmd, check=True)


def esc(path: Path) -> str:
    """Escape a Windows path for use inside a filtergraph value."""
    return str(path).replace("\\", "/").replace(":", "\\:")


def drawtext(font: Path, size: int, color: str, text_file: Path,
             x: str, y: str, extra: str = "") -> str:
    return (f"drawtext=fontfile='{esc(font)}':textfile='{esc(text_file)}'"
            f":fontsize={size}:fontcolor={color}:x={x}:y={y}{extra}")


def make_placeholders() -> Path:
    """Build 6 s zoompan proof clips from out/ stills + a scratch manifest."""
    dest = SCRATCH / "placeholder"
    dest.mkdir(parents=True, exist_ok=True)
    manifest = []
    for name in PLACEHOLDER_STILLS:
        still = ROOT / "out" / name
        clip = dest / (still.stem + ".mp4")
        if not clip.exists():
            vf = ("scale=1920:1080:force_original_aspect_ratio=increase,"
                  "crop=1920:1080,zoompan=z='1+0.10*on/360':x='iw/2-(iw/zoom/2)'"
                  ":y='ih/2-(ih/zoom/2)':d=360:s=1920x1080:fps=60,format=yuv420p")
            run([str(FFMPEG), "-y", "-loop", "1", "-i", str(still), "-vf", vf,
                 "-t", "6", "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
                 "-r", "60", str(clip)])
        manifest.append({"file": clip.name, "what": f"placeholder from {name}",
                         "best_in": 0.4, "best_out": 5.8, "note": "zoompan proof clip"})
    mpath = dest / "manifest.json"
    mpath.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return mpath


def plan_shots(entries: list[dict]) -> list[dict]:
    """Order the manifest by SHOT_PLAN, fill gaps from leftovers, size to 25-35 s."""
    by_file = {e["file"]: e for e in entries}
    shots: list[dict] = []
    used: set[str] = set()
    for fname, lt in SHOT_PLAN:
        if fname in by_file:
            e = by_file[fname]
            avail = float(e["best_out"]) - float(e["best_in"])
            if avail >= 1.6:
                shots.append({**e, "avail": avail, "lt": lt})
                used.add(fname)
    for e in entries:  # rebalance: fill up to 6 from whatever else was captured
        if len(shots) >= 6:
            break
        if e["file"] in used:
            continue
        avail = float(e["best_out"]) - float(e["best_in"])
        if avail >= 1.6:
            shots.append({**e, "avail": avail, "lt": None})
            used.add(e["file"])
    if not shots:
        raise SystemExit("manifest has no usable clips (need best_out-best_in >= 1.6 s)")
    n = len(shots)
    joins = n + 1  # title->shots...->end
    per = (TARGET_TOTAL - TITLE_DUR - END_DUR + joins * XFADE) / n
    per = max(2.6, min(3.8, per))
    for s in shots:
        s["dur"] = min(s["avail"], per)
    return shots


def detect_crop(clip: Path, start: float, dur: float) -> str | None:
    """cropdetect over the used window; returns ffmpeg crop w:h:x:y or None."""
    import re
    r = subprocess.run([str(FFMPEG), "-v", "info", "-ss", f"{start:.3f}", "-t", f"{dur:.3f}",
                        "-i", str(clip), "-vf", "cropdetect=limit=24:round=2", "-f", "null", "-"],
                       capture_output=True, text=True)
    crops = re.findall(r"crop=(\d+:\d+:\d+:\d+)", r.stderr)
    return crops[-1] if crops else None


def render(manifest_path: Path, out_path: Path) -> None:
    for p in (FFMPEG, FONT_BOLD, FONT_SEMI, FONT_REG, INTRO_XMV):
        if not p.exists():
            raise SystemExit(f"missing prerequisite: {p}")
    raw_dir = manifest_path.parent
    entries = json.loads(manifest_path.read_text(encoding="utf-8"))
    shots = plan_shots(entries)
    total = TITLE_DUR + END_DUR + sum(s["dur"] for s in shots) - (len(shots) + 1) * XFADE
    if not 25.0 <= total <= 35.0:
        raise SystemExit(f"planned total {total:.1f}s outside 25-35s; adjust TARGET_TOTAL")
    work = SCRATCH / "build"
    work.mkdir(parents=True, exist_ok=True)
    txt = {}
    def tf(key: str, text: str) -> Path:
        p = work / f"{key}.txt"
        p.write_text(text, encoding="utf-8")
        txt[key] = p
        return p
    filters: list[str] = []
    for i, s in enumerate(shots):
        crop = detect_crop(raw_dir / s["file"], float(s["best_in"]), s["dur"])
        s["crop"] = crop
        head = f"crop={crop}," if crop else ""
        f = (f"[{i}:v]{head}scale=1920:1080:flags=lanczos,"
             f"setsar=1,fps=60,eq=contrast=1.05:saturation=1.07,"
             f"format=yuv420p")
        lt = s.get("lt")
        if lt:
            kicker, main = lt
            filters.append(
                f"color=c=black@0.0:s=1920x1080:r=60:d={s['dur']:.3f},format=rgba,"
                + drawtext(FONT_BOLD, 23, GOLD, tf(f"lt{i}k", kicker), "120", "812")
                + "," + drawtext(FONT_SEMI, 40, "white", tf(f"lt{i}m", main), "120", "848")
                + f",fade=t=in:st=0.30:d=0.45:alpha=1,fade=t=out:st={s['dur']-0.55:.3f}:d=0.45:alpha=1[lt{i}]")
            f += f"[vb{i}];[vb{i}][lt{i}]overlay=0:0,drawbox=x=96:y=818:w=6:h=76:color={GOLD}@0.95:t=fill[v{i}]"
        else:
            f += f"[v{i}]"
        filters.append(f)
    def card(key: str, lines, dur: float, ys) -> str:
        parts = [f"color=c=0x0B0E13:s=1920x1080:r=60:d={dur},format=yuv420p"]
        for j, ((text, font, size, color), y) in enumerate(zip(lines, ys)):
            parts.append(drawtext(font, size, color, tf(f"{key}{j}", text), "(w-text_w)/2", str(y)))
        parts.append(f"fade=t=in:st=0:d=0.6,fade=t=out:st={dur-0.7:.3f}:d=0.7")
        return ",".join(parts) + f"[{key}v]"
    filters.append(card("title", TITLE_LINES, TITLE_DUR, [400, 540, 640]))
    filters.append(card("end", END_LINES, END_DUR, [470, 560, 640]))
    labels = ["[titlev]"] + [f"[v{i}]" for i in range(len(shots))] + ["[endv]"]
    durs = [TITLE_DUR] + [s["dur"] for s in shots] + [END_DUR]
    prev, elapsed = labels[0], durs[0]
    for k in range(1, len(labels)):
        off = elapsed - XFADE
        out_label = "[xf]" if k == len(labels) - 1 else f"[x{k}]"
        filters.append(f"{prev}{labels[k]}xfade=transition=fade:duration={XFADE}:offset={off:.3f}{out_label}")
        elapsed += durs[k] - XFADE
        prev = out_label
    aidx = len(shots)
    filters.append(f"[{aidx}:a]loudnorm=I=-16:TP=-1.5:LRA=11,aresample=48000,"
                   f"afade=t=in:st=0:d=0.8,afade=t=out:st={total-2.6:.3f}:d=2.6[aout]")
    script = work / "filtergraph.txt"
    script.write_text(";\n".join(filters) + "\n", encoding="utf-8")
    cmd = [str(FFMPEG), "-y"]
    for s in shots:
        cmd += ["-ss", f"{float(s['best_in']):.3f}", "-t", f"{s['dur']:.3f}", "-i", str(raw_dir / s["file"])]
    cmd += ["-ss", f"{INTRO_AUDIO_START:.3f}", "-t", f"{total:.3f}", "-i", str(INTRO_XMV),
            "-filter_complex", script.read_text(encoding="utf-8"), "-map", "[xf]", "-map", "[aout]",
            "-c:v", "libx264", "-preset", "medium", "-crf", "17", "-profile:v", "high",
            "-pix_fmt", "yuv420p", "-r", "60", "-c:a", "aac", "-b:a", "192k",
            "-movflags", "+faststart", str(out_path)]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    run(cmd)
    print(f"shots={len(shots)} total={total:.2f}s -> {out_path}")
    for i, s in enumerate(shots):
        print(f"  shot{i+1} {s['file']} in={s['best_in']} dur={s['dur']:.2f} crop={s.get('crop')} lt={s.get('lt')}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--placeholders", action="store_true",
                    help="build zoompan proof clips from out/ stills and render from them")
    a = ap.parse_args()
    manifest = make_placeholders() if a.placeholders else a.manifest
    render(manifest, a.out)


if __name__ == "__main__":
    main()