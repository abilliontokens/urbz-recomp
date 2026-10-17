"""Drive the built game from the host: launch, send keys, grab frames, match menu states.

Timing-blind key sequences desync whenever a load takes longer than the sleep, so every step here
waits for the screen to settle and matches it against a reference frame before deciding what to
press. Reference frames live in out/shots/ref/ and are captured from a real run.
"""

import ctypes
import os
import subprocess
import time
from ctypes import wintypes
from pathlib import Path

from PIL import Image, ImageGrab

ROOT = Path(r"D:/Urbz")
RUN = ROOT / "out/build/release"
SHOTS = ROOT / "out/shots"
REF = SHOTS / "ref"
SHOTS.mkdir(parents=True, exist_ok=True)
REF.mkdir(parents=True, exist_ok=True)

user32 = ctypes.WinDLL("user32", use_last_error=True)

VK = {
    "ENTER": 0x0D, "ESC": 0x1B, "SPACE": 0x20, "TAB": 0x09,
    "LEFT": 0x25, "UP": 0x26, "RIGHT": 0x27, "DOWN": 0x28,
    "F1": 0x70, "F2": 0x71,
    **{c: ord(c) for c in "WASDQERFZXCV"},
}

WM_KEYDOWN, WM_KEYUP, WM_CHAR = 0x100, 0x101, 0x102


def launch():
    """Start the game detached so it outlives this call."""
    return subprocess.Popen([str(RUN / "Urbz.exe")], cwd=str(RUN),
                            creationflags=0x8 | 0x200)


def running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Urbz.exe"],
                         capture_output=True, text=True).stdout
    return "Urbz.exe" in out


def kill():
    subprocess.run(["taskkill", "/F", "/IM", "Urbz.exe"], capture_output=True)
    time.sleep(2)


def window():
    """Top-level window of the running game, or 0."""
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        if user32.IsWindowVisible(hwnd) and "Urbz" in buf.value:
            found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    return found[0] if found else 0


def focus(hwnd=None, verify=True):
    """Really take the foreground.

    The game drops every key unless its window has focus (input_kbm gates on WM_SETFOCUS /
    WM_ACTIVATE), and a bare SetForegroundWindow from a background process is ignored by the
    Windows foreground lock - which is exactly how a whole drive run silently pressed nothing.
    Attaching to the window's input queue first lifts that restriction.
    """
    hwnd = hwnd or window()
    if not hwnd:
        return 0
    SW_RESTORE = 9
    me = ctypes.windll.kernel32.GetCurrentThreadId()
    other = user32.GetWindowThreadProcessId(hwnd, None)
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.AttachThreadInput(me, other, True)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.SetActiveWindow(hwnd)
    user32.SetFocus(hwnd)
    user32.AttachThreadInput(me, other, False)
    if verify and user32.GetForegroundWindow() != hwnd:
        time.sleep(0.3)
        user32.SetForegroundWindow(hwnd)
    return hwnd


def tap(vk, after=0.4, hold=0.06, hwnd=None):
    """Post a keystroke to the game window (it reads host keyboard messages)."""
    hwnd = hwnd or window()
    if not hwnd:
        return False
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0x00000001)
    time.sleep(hold)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0000001)
    time.sleep(after)
    return True


def grab():
    return ImageGrab.grab()


def shot(name):
    path = SHOTS / f"{name}.png"
    grab().resize((1280, 720)).save(path)
    return str(path)


def _sig(img, n=24):
    """Small grayscale signature used for frame comparison."""
    return list(img.convert("L").resize((n, n), Image.BILINEAR).getdata())


def _dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def settled(timeout=90, quiet=1.6, poll=0.5, tol=4.0):
    """Wait until consecutive frames stop changing; returns the settled frame."""
    last, stable_since = None, None
    deadline = time.time() + timeout
    while time.time() < deadline:
        img = grab()
        sig = _sig(img)
        if last is not None and _dist(sig, last) < tol:
            if stable_since is None:
                stable_since = time.time()
            elif time.time() - stable_since >= quiet:
                return img
        else:
            stable_since = None
        last = sig
        time.sleep(poll)
    return grab()


def save_ref(name, img=None):
    (img or grab()).resize((1280, 720)).save(REF / f"{name}.png")


def match(img=None, tol=14.0):
    """Nearest reference frame for the current screen, or None."""
    img = img or grab()
    sig = _sig(img)
    best, best_d = None, 1e9
    for p in sorted(REF.glob("*.png")):
        d = _dist(sig, _sig(Image.open(p)))
        if d < best_d:
            best, best_d = p.stem, d
    return (best, round(best_d, 1)) if best_d <= tol else (None, round(best_d, 1))


def log_path():
    return sorted((RUN / "logs").glob("urbz_*.log"), key=lambda f: f.stat().st_mtime)[-1]


def log_lines(*needles, limit=40, strip=True):
    txt = log_path().read_text(errors="ignore")
    out = [l for l in txt.splitlines() if any(n in l for n in needles)]
    if strip:
        out = [l.split("] ")[-1] for l in out]
    return out[:limit]
