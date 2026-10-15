#!/usr/bin/env python3
"""offline 6x upscale of the game's .xmv movies into assets/movies_4k/<stem>.mp4.

the host samples these as a drop-in replacement for the guest's own movie texture,
so every output is exactly 6x the source pixel grid, video only, and constant frame
rate equal to the source.

the upscaler is realesrgan-ncnn-vulkan running the realesr-animevideov3 model at x4,
followed by a 1.5x lanczos resample to the exact 6x target. a light hqdn3d pre-filter
runs during frame extraction so the net is not fed the sources' wmv2 block noise - the
net amplifies that noise into visible blotches on flat walls and skin otherwise.

run from the repo root:
    python tools/upscale_movies.py
    python tools/upscale_movies.py --only urbz_intro --force
    python tools/upscale_movies.py --dry-run
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path

# the repo root is the parent of tools/. every binary is invoked by absolute path
# because the shells used here cannot exec a .exe through a relative path.
ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "tools" / "bin"
EXE = ".exe" if os.name == "nt" else ""
FFMPEG = BIN / f"ffmpeg{EXE}"
FFPROBE = BIN / f"ffprobe{EXE}"
REALESRGAN_DIR = BIN / "realesrgan"
REALESRGAN = REALESRGAN_DIR / f"realesrgan-ncnn-vulkan{EXE}"
REALESRGAN_MODELS = REALESRGAN_DIR / "models"
REALESRGAN_MODEL = "realesr-animevideov3"

SRC_DIR = ROOT / "assets" / "movies"
OUT_DIR = ROOT / "assets" / "movies_4k"
TMP_DIR = ROOT / "out" / "upscale_tmp"

SCALE = 6
NET_SCALE = 4

# light spatial+temporal denoise. strengths judged on stills from urbz_intro,
# urbz_attract and ea_games: this removes the flat-area block mottling that the net
# would otherwise sharpen into blotches, while leaving hair, lettering and fabric
# detail untouched. stronger settings start eating low-contrast gradients.
PREFILTER = "hqdn3d=1.5:1.5:6:6"

# png load / net / png save thread counts. the png codecs are the bottleneck on this
# box, so the i/o sides get far more threads than the gpu side.
REALESRGAN_JOBS = "6:2:6"

X264_PRESET = "medium"
X264_CRF = "17"


class PipelineError(RuntimeError):
    pass


@dataclass
class Movie:
    stem: str
    src: Path
    out: Path
    width: int
    height: int
    frames: int
    fps: Fraction

    @property
    def target_width(self) -> int:
        return self.width * SCALE

    @property
    def target_height(self) -> int:
        return self.height * SCALE


def check_tools() -> None:
    missing = [str(p) for p in (FFMPEG, FFPROBE, REALESRGAN) if not p.is_file()]
    if not (REALESRGAN_MODELS / f"{REALESRGAN_MODEL}-x{NET_SCALE}.param").is_file():
        missing.append(str(REALESRGAN_MODELS / f"{REALESRGAN_MODEL}-x{NET_SCALE}.param"))
    if missing:
        raise PipelineError(
            "missing upscaler binaries/models:\n  "
            + "\n  ".join(missing)
            + "\nrealesrgan-ncnn-vulkan comes from the Real-ESRGAN v0.2.5.0 windows release;\n"
            "unpack realesrgan-ncnn-vulkan.exe, vcomp140.dll and models/ into tools/bin/realesrgan/."
        )


def run(cmd: list[str], *, capture: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(cmd, capture_output=capture, text=True)
    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout or "")[-2000:]
        raise PipelineError(f"command failed ({proc.returncode}): {cmd[0]}\n{tail}")
    return proc


def probe_stream(path: Path, entries: str, *, count_frames: bool = False) -> dict:
    cmd = [
        str(FFPROBE), "-v", "error", "-select_streams", "v:0",
        "-show_entries", f"stream={entries}", "-of", "json", str(path),
    ]
    if count_frames:
        cmd.insert(-1, "-count_frames")
    data = json.loads(run(cmd).stdout)
    streams = data.get("streams") or []
    if not streams:
        raise PipelineError(f"no video stream in {path}")
    return streams[0]


def source_frame_count(path: Path) -> int:
    # .xmv carries no nb_frames, and decoding the whole file is only a couple of
    # seconds at 640x448, so just count what the decoder actually emits.
    st = probe_stream(path, "nb_read_frames", count_frames=True)
    return int(st["nb_read_frames"])


def output_frame_count(path: Path) -> int:
    # one packet per frame in an h.264 mp4; far cheaper than decoding 4k.
    st = json.loads(run([
        str(FFPROBE), "-v", "error", "-select_streams", "v:0", "-count_packets",
        "-show_entries", "stream=nb_read_packets", "-of", "json", str(path),
    ]).stdout)["streams"][0]
    return int(st["nb_read_packets"])


def has_audio(path: Path) -> bool:
    out = run([
        str(FFPROBE), "-v", "error", "-select_streams", "a",
        "-show_entries", "stream=index", "-of", "csv=p=0", str(path),
    ]).stdout.strip()
    return bool(out)


def discover(only: str | None) -> list[Movie]:
    movies: list[Movie] = []
    for src in sorted(SRC_DIR.glob("*.xmv")):
        if only and src.stem != only:
            continue
        st = probe_stream(src, "width,height,r_frame_rate")
        movies.append(Movie(
            stem=src.stem,
            src=src,
            out=OUT_DIR / f"{src.stem}.mp4",
            width=int(st["width"]),
            height=int(st["height"]),
            frames=source_frame_count(src),
            fps=Fraction(st["r_frame_rate"]),
        ))
    if only and not movies:
        raise PipelineError(f"no source named {only}.xmv under {SRC_DIR}")
    if not movies:
        raise PipelineError(f"no .xmv sources under {SRC_DIR}")
    return movies


def extract_frames(movie: Movie, dst: Path) -> list[Path]:
    dst.mkdir(parents=True, exist_ok=True)
    run([
        str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(movie.src),
        "-an", "-vf", PREFILTER, "-fps_mode", "passthrough",
        "-start_number", "0", str(dst / "%08d.png"),
    ])
    frames = sorted(dst.glob("*.png"))
    if len(frames) != movie.frames:
        raise PipelineError(
            f"{movie.stem}: extracted {len(frames)} frames, source has {movie.frames}"
        )
    return frames


def start_encoder(movie: Movie) -> subprocess.Popen:
    movie.out.parent.mkdir(parents=True, exist_ok=True)
    fps = f"{movie.fps.numerator}/{movie.fps.denominator}"
    cmd = [
        str(FFMPEG), "-hide_banner", "-loglevel", "error", "-y",
        "-f", "image2pipe", "-framerate", fps, "-i", "-",
        "-vf", f"scale={movie.target_width}:{movie.target_height}:flags=lanczos",
        "-an",
        "-c:v", "libx264", "-profile:v", "high", "-preset", X264_PRESET,
        "-crf", X264_CRF, "-pix_fmt", "yuv420p",
        "-fps_mode", "cfr", "-r", fps,
        "-movflags", "+faststart",
        str(movie.out),
    ]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE)


def feeder(enc: subprocess.Popen, work: queue.Queue) -> None:
    """streams finished chunks into the encoder so the gpu can run the next chunk."""
    try:
        while True:
            chunk = work.get()
            if chunk is None:
                return
            chunk_dir, expected = chunk
            files = sorted(chunk_dir.glob("*.png"))
            if len(files) != expected:
                raise PipelineError(
                    f"upscaler produced {len(files)} of {expected} frames in {chunk_dir}"
                )
            for f in files:
                enc.stdin.write(f.read_bytes())
            shutil.rmtree(chunk_dir, ignore_errors=True)
    except BaseException as exc:  # surfaced by the main thread after join
        feeder.error = exc  # type: ignore[attr-defined]
        # keep draining so neither the producer's next put nor its sentinel can wedge
        # on the bounded queue while this thread is gone.
        while True:
            chunk = work.get()
            if chunk is None:
                return
            shutil.rmtree(chunk[0], ignore_errors=True)


def upscale(movie: Movie, work_dir: Path, chunk_size: int) -> None:
    src_dir = work_dir / "src"
    frames = extract_frames(movie, src_dir)

    enc = start_encoder(movie)
    work: queue.Queue = queue.Queue(maxsize=1)
    feeder.error = None  # type: ignore[attr-defined]
    thread = threading.Thread(target=feeder, args=(enc, work), daemon=True)
    thread.start()

    try:
        for i in range(0, len(frames), chunk_size):
            batch = frames[i:i + chunk_size]
            chunk_in = work_dir / f"in_{i:08d}"
            chunk_out = work_dir / f"out_{i:08d}"
            chunk_in.mkdir()
            chunk_out.mkdir()
            for f in batch:
                f.rename(chunk_in / f.name)
            run([
                str(REALESRGAN),
                "-i", str(chunk_in), "-o", str(chunk_out),
                "-n", REALESRGAN_MODEL, "-s", str(NET_SCALE), "-f", "png",
                "-m", str(REALESRGAN_MODELS), "-g", "0", "-j", REALESRGAN_JOBS,
            ])
            shutil.rmtree(chunk_in, ignore_errors=True)
            work.put((chunk_out, len(batch)))
            if feeder.error:  # type: ignore[attr-defined]
                raise feeder.error  # type: ignore[attr-defined,misc]
            done = min(i + chunk_size, len(frames))
            print(f"    {movie.stem}: {done}/{len(frames)} frames", flush=True)
    finally:
        work.put(None)
        thread.join()
        if enc.stdin and not enc.stdin.closed:
            enc.stdin.close()
        err = enc.stderr.read().decode("utf-8", "replace") if enc.stderr else ""
        rc = enc.wait()

    if feeder.error:  # type: ignore[attr-defined]
        raise feeder.error  # type: ignore[attr-defined,misc]
    if rc != 0:
        raise PipelineError(f"{movie.stem}: encoder failed ({rc})\n{err[-2000:]}")


def verify(movie: Movie) -> tuple[bool, str]:
    if not movie.out.is_file():
        return False, "missing"
    st = probe_stream(movie.out, "width,height,profile,pix_fmt,r_frame_rate")
    problems = []
    if (int(st["width"]), int(st["height"])) != (movie.target_width, movie.target_height):
        problems.append(f"size {st['width']}x{st['height']} != {movie.target_width}x{movie.target_height}")
    frames = output_frame_count(movie.out)
    if frames != movie.frames:
        problems.append(f"frames {frames} != {movie.frames}")
    if has_audio(movie.out):
        problems.append("has an audio stream")
    if st.get("pix_fmt") != "yuv420p":
        problems.append(f"pix_fmt {st.get('pix_fmt')}")
    if st.get("profile") != "High":
        problems.append(f"profile {st.get('profile')}")
    if Fraction(st["r_frame_rate"]) != movie.fps:
        problems.append(f"fps {st['r_frame_rate']} != {movie.fps}")
    if problems:
        return False, "; ".join(problems)
    return True, f"{st['width']}x{st['height']} {frames}f {st['r_frame_rate']} no audio"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", metavar="STEM", help="process a single movie by .xmv stem")
    ap.add_argument("--force", action="store_true", help="re-encode even if the output exists")
    ap.add_argument("--dry-run", action="store_true", help="print the plan and exit")
    ap.add_argument("--verify-only", action="store_true", help="re-probe existing outputs, encode nothing")
    ap.add_argument("--chunk", type=int, default=300, metavar="N",
                    help="frames per upscaler batch; bounds temp disk use (default 300)")
    args = ap.parse_args()

    try:
        if not args.verify_only:
            check_tools()
        movies = discover(args.only)

        print(f"{'source':24s} {'src size':>10s} {'target':>12s} {'frames':>7s} {'fps':>10s}")
        for m in movies:
            print(f"{m.stem:24s} {m.width}x{m.height:<6d} {m.target_width}x{m.target_height:<7d}"
                  f" {m.frames:7d} {str(m.fps):>10s}")
        if args.dry_run:
            return 0

        if not args.verify_only:
            TMP_DIR.mkdir(parents=True, exist_ok=True)
            for m in movies:
                if m.out.is_file() and not args.force:
                    print(f"  skip {m.stem} (output exists; --force to redo)", flush=True)
                    continue
                work_dir = TMP_DIR / m.stem
                shutil.rmtree(work_dir, ignore_errors=True)
                work_dir.mkdir(parents=True)
                print(f"  upscaling {m.stem} -> {m.target_width}x{m.target_height}", flush=True)
                try:
                    upscale(m, work_dir, args.chunk)
                finally:
                    shutil.rmtree(work_dir, ignore_errors=True)
            shutil.rmtree(TMP_DIR, ignore_errors=True)

        failed = []
        print("\nverification:")
        for m in movies:
            ok, detail = verify(m)
            print(f"  {'ok  ' if ok else 'FAIL'} {m.stem:24s} {detail}")
            if not ok:
                failed.append(m.stem)
        if failed:
            print(f"\n{len(failed)} output(s) failed verification: {', '.join(failed)}")
            return 1
        print(f"\n{len(movies)} output(s) verified in {OUT_DIR}")
        return 0
    except PipelineError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
