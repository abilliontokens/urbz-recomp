"""Capture trailer footage from the built game, unattended.

Owns the live session end to end: backs up settings.toml, writes a capture
profile (windowed 1920x1080, 60 fps cap, modern HUD), launches Urbz.exe,
moves its window (found by PID, never title) to the top-left of the primary
display and pins it topmost, then records with ffmpeg gdigrab on the
client-area desktop region. (OBS win-capture/WGC/game-capture/monitor all
returned pure black for this game on this machine while GDI grabs were
bright, so gdigrab under the topmost pin is the working path.) Every take
is gated: two stills must agree with a cropped client-area grab while the
game is foreground, must not be frozen, and the clip must probe as
1920x1080/60 H.264 - a failing take is deleted, never shipped. Then drives
boot -> skip movies -> title -> 1 Player -> Load Game -> the owner's save
-> gameplay, and records each shot as its own clip under out/trailer/raw/,
plus manifest.json (file/what/best_in/best_out/note). Restores
settings.toml and kills the game on exit.

Run:  python tools/capture_trailer.py [--only slug,slug] [--raw DIR] [--keep-open]
Requires: out/build/release/Urbz.exe, tools/bin/ffmpeg.exe, PIL, and a save
to load (hdd/E/UDATA/45410060/*/SaveGame beside the exe).
"""
import ctypes
import json
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(r"D:/Urbz")
RUN = ROOT / "out/build/release"
RAW = ROOT / "out/trailer/raw"
DBG = ROOT / "out/trailer/debug"
FF = ROOT / "tools/bin/ffmpeg.exe"
FFP = ROOT / "tools/bin/ffprobe.exe"
SETTINGS = RUN / "settings.toml"
BACKUP = ROOT / "out/trailer/settings.toml.bak"

sys.path.insert(0, str(ROOT / "tools"))
import drive  # noqa: E402
from PIL import Image, ImageStat  # noqa: E402

user32 = ctypes.WinDLL("user32", use_last_error=True)
VK = drive.VK
RECT = {"x": 0, "y": 0, "w": 1920, "h": 1080}
MANIFEST = []  # filled by keep()
REQUIRE_HUD = False  # set once the save is loaded: gameplay takes must show the HUD



def say(msg):
    print(f"[capture] {msg}", flush=True)


# ---------------------------------------------------------------- settings
def write_profile():
    BACKUP.parent.mkdir(parents=True, exist_ok=True)
    RAW.mkdir(parents=True, exist_ok=True)
    DBG.mkdir(parents=True, exist_ok=True)
    if SETTINGS.exists() and not BACKUP.exists():
        # Never snapshot a file that is already the capture profile: a previous run that
        # died between write_profile() and restore_settings() would otherwise make the
        # capture profile the "original" and the owner's real settings unrecoverable.
        cur = SETTINGS.read_text()
        if 'display_mode = "windowed"' in cur and "width = 1920" in cur and "fps_cap = 60" in cur:
            say("WARNING: settings.toml already looks like the capture profile; not "
                "overwriting the backup. Restore the owner's settings by hand if needed.")
        else:
            shutil.copy2(SETTINGS, BACKUP)
    lines = SETTINGS.read_text().splitlines() if SETTINGS.exists() else []
    want = {"display_mode": '"windowed"', "width": "1920", "height": "1080",
            "vsync": "false", "fps_cap": "60", "modern_hud": "true"}
    seen = set()
    out = []
    for ln in lines:
        key = ln.split("=")[0].strip() if "=" in ln else ""
        if key in want:
            out.append(f"{key} = {want[key]}")
            seen.add(key)
        else:
            out.append(ln)
    for k, v in want.items():
        if k not in seen:
            out.append(f"{k} = {v}")
    SETTINGS.write_text("\n".join(out) + "\n")
    say(f"capture profile written (backup {BACKUP})")


def restore_settings():
    if BACKUP.exists():
        shutil.copy2(BACKUP, SETTINGS)
        BACKUP.unlink()
        say("settings.toml restored")
    else:
        say("WARNING: no settings backup to restore - settings.toml still holds the "
            "capture profile (windowed 1920x1080, 60 fps cap)")


# ------------------------------------------------------------------ window
def client_rect(hwnd):
    pt = wintypes.POINT(0, 0)
    rc = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    user32.ClientToScreen(hwnd, ctypes.byref(pt))
    return pt.x, pt.y, rc.right - rc.left, rc.bottom - rc.top


def place_window():
    hwnd = 0
    for _ in range(60):
        hwnd = drive.window()
        if hwnd:
            break
        time.sleep(1)
    if not hwnd:
        raise RuntimeError("game window never appeared")
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    CX, CY = 1920, 1024  # client origin: bottom-right quadrant, bottom edge
    # above the taskbar (owner's overlay bars sit over the top-left, and the
    # taskbar overlays the screen's bottom strip; the game owns this rect)
    for _ in range(10):
        wr = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(wr))
        x, y, cw, ch = client_rect(hwnd)
        ow = (wr.right - wr.left) - cw + 1920
        oh = (wr.bottom - wr.top) - ch + 1080
        # outer position so the client area lands exactly at (CX, CY)
        user32.SetWindowPos(hwnd, 0, CX - (x - wr.left), CY - (y - wr.top),
                            ow, oh, 0x0004)  # SWP_NOZORDER
        time.sleep(0.4)
        x, y, cw, ch = client_rect(hwnd)
        if (x, y, cw, ch) == (CX, CY, 1920, 1080):
            break
    RECT.update(x=x, y=y, w=cw, h=ch)
    drive.focus(hwnd)
    say(f"client rect {RECT}")
    if (RECT["w"], RECT["h"]) != (1920, 1080):
        raise RuntimeError(f"client area is {RECT['w']}x{RECT['h']}, not 1920x1080")
    return hwnd

def pin_topmost():
    """Keep the game above every other window (incl. this session's terminal)."""
    hwnd = drive.window()
    if not hwnd:
        return 0
    x, y, w, h = client_rect(hwnd)
    if (w, h) != (1920, 1080):
        place_window()
        x, y, w, h = client_rect(hwnd)
    RECT.update(x=x, y=y, w=w, h=h)
    user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002)  # TOPMOST, no size/move... keep pos
    # SetWindowPos above with NOSIZE|NOMOVE keeps geometry; ensure client at its place.
    drive.focus(hwnd)
    return hwnd


# ----------------------------------------------------------------- capture


def record(slug, seconds, action=None):
    """One gdigrab take of the client rect (game pinned topmost + foreground).

    OBS win-capture/WGC/game-capture/monitor all returned pure black for
    this game on this machine (Notepad WGC worked; client grabs were
    bright), so the recorder is ffmpeg gdigrab on the desktop region -
    safe now that the game window is PID-matched and pinned topmost.
    """
    pin_topmost()
    out = RAW / f"{slug}.mp4"
    log = open(DBG / f"ffmpeg_{slug}.log", "wb")
    p = subprocess.Popen(
        [str(FF), "-y", "-f", "gdigrab", "-framerate", "60", "-draw_mouse", "0",
         "-offset_x", str(RECT["x"]), "-offset_y", str(RECT["y"]),
         "-video_size", f"{RECT['w']}x{RECT['h']}", "-i", "desktop",
         "-c:v", "libx264", "-preset", "veryfast", "-crf", "16",
         "-pix_fmt", "yuv420p", "-r", "60", str(out)],
        stdin=subprocess.PIPE, stdout=log, stderr=log)
    time.sleep(0.6)
    t0 = time.time()
    if action:
        action()
    left = seconds - (time.time() - t0)
    if left > 0:
        time.sleep(left)
    try:
        p.communicate(b"q", timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    log.close()
    say(f"recorded {slug} -> {out} ({out.stat().st_size/1e6:.1f} MB)")
    return out if gate_take(slug, out) else None


def key_hold(vk, seconds, pulses=None):
    hwnd = drive.window()
    def go():
        if pulses:
            n, on, off = pulses
            for _ in range(n):
                user32.PostMessageW(hwnd, 0x100, vk, 1)
                time.sleep(on)
                user32.PostMessageW(hwnd, 0x101, vk, 0xC0000001)
                time.sleep(off)
        else:
            user32.PostMessageW(hwnd, 0x100, vk, 1)
            time.sleep(seconds)
            user32.PostMessageW(hwnd, 0x101, vk, 0xC0000001)
    return go


def probe(path, at=1.0):
    out = DBG / f"probe_{path.stem}_{at}.png"
    subprocess.run([str(FF), "-y", "-ss", str(at), "-i", str(path),
                    "-frames:v", "1", str(out)], capture_output=True)
    return out


def frame_stats(img_path):
    im = Image.open(img_path).convert("L")
    stat = ImageStat.Stat(im)
    px = im.resize((64, 36))
    colours = len(set(px.getdata()))
    return stat.mean[0], stat.stddev[0], colours


def client_grab():
    """Screenshot cropped to the game client area (what gdigrab should see)."""
    full = drive.grab()
    return full.crop((RECT["x"], RECT["y"], RECT["x"] + RECT["w"], RECT["y"] + RECT["h"]))


def gate_take(slug, path):
    """A take ships only if it is verifiably the game. Failure deletes it."""
    s1, s2 = probe(path, 1.0), probe(path, 2.5)
    ok = s1.exists() and s2.exists()
    if ok:
        pin_topmost()
        mean, std, colours = frame_stats(s1)
        grab_d = drive._dist(drive._sig(Image.open(s1)), drive._sig(client_grab()))
        frozen = drive._dist(drive._sig(Image.open(s1)), drive._sig(Image.open(s2)))
        pr = subprocess.run([str(FFP), "-v", "error", "-select_streams", "v:0",
                             "-show_entries", "stream=width,height,r_frame_rate,codec_name",
                             "-of", "csv=p=0", str(path)], capture_output=True, text=True).stdout.strip()
        say(f"gate {slug}: mean={mean:.1f} std={std:.1f} colours={colours} "
            f"grab_diff={grab_d:.1f} motion={frozen:.2f} probe={pr}")
        ok = (mean >= 12 and std >= 4 and colours >= 24 and grab_d <= 55
              and (frozen >= 0.15 or grab_d <= 10)
              and "1920,1080" in pr and "h264" in pr)
        if ok and REQUIRE_HUD:
            hud_ok, n = hud_present(Image.open(s1))
            if not hud_ok:
                hud_ok, n = hud_present(Image.open(s2))
            say(f"gate {slug}: HUD bars={n} px -> {'gameplay' if hud_ok else 'MENU FRAME'}")
            ok = hud_ok
    if not ok:
        Path(path).unlink(missing_ok=True)
        say(f"GATE FAILED for {slug}: deleted, not shipped")
        return False
    return True

def hud_present(img, min_px=300):
    """True when the modern HUD's motive bars are on screen.

    The bars are the one thing no menu or dialog draws: measured 883-2621 bright-green
    pixels in the bottom-left band on gameplay frames and exactly 0 on the title screen
    and on the blue save/operation dialogs. That makes it the honest "am I in the game"
    test - screen motion is not, because a static establishing shot has none.
    """
    im = img.convert("RGB")
    w, h = im.size
    band = im.crop((int(w * 0.02), int(h * 0.88), int(w * 0.40), int(h * 0.98)))
    n = sum(1 for r, g, b in band.getdata() if g > 110 and g > r * 1.25 and g > b * 1.6)
    return n >= min_px, n


def wait_for_gameplay(timeout=150):
    """Block until the HUD says we are actually in the world."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ok, n = hud_present(client_grab())
        if ok:
            say(f"in gameplay (HUD bars: {n} px)")
            return True
        time.sleep(2)
    return False


def validity_gate():
    say("validity gate: probe take")
    for attempt in range(3):
        p = record("__probe", 4)
        if p is not None:
            p.unlink(missing_ok=True)
            say("validity gate PASSED")
            return
        if attempt == 2:
            raise RuntimeError("VALIDITY GATE FAILED after 3 probes")
        say("probe failed (likely static boot splash); waiting 6 s and retrying")
        time.sleep(6)


# ------------------------------------------------------------------ driving
def snap(name):
    img = client_grab().resize((1280, 720))
    p = DBG / f"{name}.png"
    img.save(p)
    m = drive.match(img)
    say(f"  [{name}] match={m}")
    return str(p)


def press(key, after=0.8):
    pin_topmost()
    drive.tap(VK[key] if isinstance(key, str) else key, after=after)
    drive.settled(timeout=25)


def keep(slug, what, best_in, best_out, note):
    if not (RAW / f"{slug}.mp4").exists():
        say(f"skip {slug}: no gated clip on disk")
        return
    MANIFEST.append({"file": f"{slug}.mp4", "what": what,
                     "best_in": best_in, "best_out": best_out, "note": note})
    say(f"keep {slug} [{best_in}-{best_out}] {note}")


def main():
    global RAW, REQUIRE_HUD
    only = None
    if "--only" in sys.argv:
        only = set(sys.argv[sys.argv.index("--only") + 1].split(","))
    if "--raw" in sys.argv:
        # a verification run must not clobber footage a trailer is already cut from
        RAW = Path(sys.argv[sys.argv.index("--raw") + 1])
        RAW.mkdir(parents=True, exist_ok=True)
    def want(slug):
        return only is None or slug in only

    crashes_before = set()
    cd = Path.home() / "AppData/Local/CrashDumps"
    if cd.exists():
        crashes_before = {p.name for p in cd.glob("Urbz*")}

    def boot():
        if drive.running():
            drive.kill()
        drive.launch()
        hwnd = place_window()
        drive.focus(hwnd)
        time.sleep(6)  # boot splash -> movies begin
        return hwnd

    def to_menu():
        """Skip movies, land on the main menu (1/2 Player, Credits)."""
        for _ in range(4):
            if drive.match(client_grab().resize((1280, 720)))[0] == "title":
                break
            press("ENTER", after=1.0)
        drive.settled(timeout=30)

    write_profile()
    try:
        # ================= session 1: movie, title, creation ============
        boot()
        validity_gate()
        if want("intro_movie"):
            record("intro_movie", 8)
            keep("intro_movie", "intro movie playing (gamma-bypass movie path)",
                 0.8, 7.2, "movie frames, check stills")
        to_menu()
        snap("title")
        if want("title"):
            record("title", 6)
            keep("title", "title / attract screen", 0.8, 5.2, "logo screen, steady")
        press("ENTER", after=1.2)  # main menu -> (already there) / start
        snap("menu")
        press("SPACE", after=1.2)  # A on "1 Player" (highlighted)
        snap("playermenu")
        drive.kill()
        time.sleep(2)

        # ================= session 2: load the save -> city =============
        boot()
        to_menu()
        press("ENTER", after=1.2)
        press("SPACE", after=1.2)  # 1 Player
        snap("playermenu2")
        pin_topmost()
        drive.tap(VK["DOWN"], after=0.5)  # New Game -> Load Game
        # The save/operation dialogs all look alike to a 24x24 signature, so advance on
        # the one thing that cannot be confused: SPACE until the HUD says we are in the
        # world. Each press is one "A" on a default-highlighted button (Load Game ->
        # the Urb -> Start), and a wrong extra press lands on a dialog that SPACE
        # dismisses, so the loop converges instead of wandering off like a blind
        # fixed-length sequence did.
        for attempt in range(8):
            press("SPACE", after=1.6)
            snap(f"load_step{attempt}")
            if hud_present(client_grab())[0]:
                break
            say(f"  load step {attempt}: not in the world yet, waiting")
            drive.settled(timeout=45, quiet=2.0)
            if hud_present(client_grab())[0]:
                break
        if not wait_for_gameplay(150):
            raise RuntimeError("never reached gameplay: the save did not load")
        REQUIRE_HUD = True
        snap("city_free")

        if want("station_establish"):
            record("station_establish", 7)
            keep("station_establish", "wide still view of the platform",
                 1.0, 6.0, "static establish; verify framing in stills")
        if want("station_orbit"):
            record("station_orbit", 9, action=key_hold(VK["E"], 0, pulses=(7, 0.7, 0.35)))
            keep("station_orbit", "slow camera rotation over the platform",
                 1.0, 8.0, "E pulses; smooth slow orbit")
        if want("camera_zoom"):
            def zoom():
                key_hold(VK["F"], 3.2)()
                time.sleep(0.4)
                key_hold(VK["R"], 3.2)()
            record("camera_zoom", 9, action=zoom)
            keep("camera_zoom", "slow zoom out to the overhead reveal, then in",
                 0.8, 8.2, "F out, R in")
        if want("walk"):
            record("walk", 8, action=key_hold(VK["W"], 5.5))
            keep("walk", "the sim walking (WASD)", 1.0, 6.5, "W held")
        if want("hud_motives"):
            record("hud_motives", 7)
            keep("hud_motives", "modern HUD: motive bars, reputation/clock, bottom-right task",
                 1.0, 6.0, "verify HUD regions in stills")
        if want("pie_menu"):
            # walk a touch toward something/NPC then A to open interactions
            key_hold(VK["W"], 1.2)()
            record("pie_menu", 8, action=lambda: (time.sleep(1.0), press("SPACE", after=0.2), time.sleep(4)))
            keep("pie_menu", "interaction pie menu opening", 1.5, 7.0,
                 "A near object/NPC; verify pie in stills, skip if absent")
            press("ESC", after=0.6)
        if want("dialogue"):
            record("dialogue", 8, action=lambda: (time.sleep(1.0), press("SPACE", after=0.2), time.sleep(4)))
            keep("dialogue", "NPC dialogue card", 1.5, 7.0,
                 "attempt only; verify card in stills, skip if absent")
            press("ESC", after=0.6)
        if want("fps_graph"):
            drive.focus()
            drive.tap(VK["F2"], after=0.8)
            record("fps_graph", 7, action=key_hold(VK["E"], 0, pulses=(4, 0.6, 0.4)))
            keep("fps_graph", "F2 frame-time graph over gameplay",
                 1.0, 6.0, "graph overlay visible; verify in stills")
            drive.focus()
            drive.tap(VK["F2"], after=0.5)

        snap("final")
    finally:
        (RAW / "manifest.json").write_text(json.dumps(MANIFEST, indent=2))
        say(f"manifest: {len(MANIFEST)} entries")
        if "--keep-open" not in sys.argv:
            drive.kill()
        restore_settings()
        cd = Path.home() / "AppData/Local/CrashDumps"
        new = {p.name for p in cd.glob("Urbz*")} - crashes_before if cd.exists() else set()
        say(f"new CrashDumps entries: {new or 'none'}; game running: {drive.running()}")


if __name__ == "__main__":
    main()
