# drive.py: deterministic screenshot-driven input for bring-up. Requires the game
# running with URBZ_STEP=<step file> (src/common/game_info.cpp StepPad). Usage:
#   python tools/drive.py START shot:title A RIGHTx9 wait:2 ...
# Tokens are NAME or NAMExN (counted pulses); shot:<name> saves out/<name>.png via
# shot.ps1 and verifies the game window is foreground. Never launched for hand play.
import time, ctypes, subprocess, pathlib, sys
step=pathlib.Path(r'D:/Urbz/out/step.txt')
u=ctypes.windll.user32
hwnd=u.FindWindowW(None, "The Urbz: Sims in the City")
def press(tok, hold=None, settle=1.2):
    if hold is None:
        n=1
        if 'x' in tok and tok.rsplit('x',1)[1].isdigit(): n=int(tok.rsplit('x',1)[1])
        hold = 0.35*n + 0.35
    step.write_text(tok); time.sleep(hold); step.write_text("NONE"); time.sleep(settle); print("pressed", tok, flush=True)
def shot(name):
    for attempt in range(6):
        u.ShowWindow(hwnd,5); u.BringWindowToTop(hwnd)
        u.SetCursorPos(1280+200,587+100); u.mouse_event(2,0,0,0,0); u.mouse_event(4,0,0,0,0)
        u.SetForegroundWindow(hwnd)
        sh = pathlib.Path(__file__).with_name('shot.ps1')
        out=subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(sh),'-Out',f'D:/Urbz/out/{name}.png'], capture_output=True, text=True).stdout
        print(name, out.strip().replace('\n',' | '), flush=True)
        if 'isGame=True' in out: return
        time.sleep(0.4)
if __name__=='__main__':
    for a in sys.argv[1:]:
        if a.startswith('shot:'): shot(a[5:])
        elif a.startswith('wait:'): time.sleep(float(a[5:]))
        else: press(a)
