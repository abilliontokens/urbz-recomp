// Keyboard and mouse as the console's pad on port 0.
//
// The game reads nothing but an XINPUT_GAMEPAD (docs/input-and-draw-path.md): there is no second
// input path to add a PC control scheme to, so the keyboard and the mouse are translated into the
// same report a controller produces. Port 0 is reported connected whether or not a pad is plugged
// in - a PC always has a keyboard - and a real pad on that port is merged with, never overridden:
// buttons are ORed in and an axis is only moved if the keys push it further than the stick already
// does. Both can be used at once, which is what the camera-on-the-mouse, walk-on-the-keys layout
// below is for.
//
// Layout (the console input each binding drives):
//
//   W A S D                  left stick          walk; menus that take the stick
//   arrow keys               D-pad               menu and dialog navigation
//   mouse, right button held right stick         camera: rotate and tilt, as a stick deflected
//                                                from where the button went down
//   Q / E                    right stick X       camera rotate left / right
//   R / F                    right stick Y       camera tilt-zoom up / down
//   mouse wheel              right stick Y pulse camera zoom in / out
//   space                    A                   accept, interact
//   escape, backspace        B                   cancel, back
//   X / Y                    X / Y
//   Z / C                    left / right trigger
//   1 / 2                    black / white
//   enter                    Start               pause menu; "press Start"
//   tab                      Back
//   shift / ctrl, middle btn left / right stick click
//
// Camera rotation and zoom direction follow the game's own options (invert camera rotation, auto
// zoom) - this layer only reports stick deflection.
//
// Keys are only read while the game window has focus, and are dropped when it loses focus, so an
// alt-tabbed game does not keep walking. While the F1 settings panel is up the overlay owns the
// keyboard and the mouse and nothing here reaches the game.
#include "common/input_kbm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iterator>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "common/overlay.h"
#include "core/log.h"

namespace kbm {

namespace {

// the window thread writes these, the guest's polling threads read them
std::atomic<bool> g_down[256];      // by virtual-key code
std::atomic<bool> g_focus{false};   // the game window has the keyboard
std::atomic<bool> g_rmb{false};     // right mouse button: the camera stick
std::atomic<bool> g_mmb{false};     // middle mouse button: right stick click
std::atomic<int> g_mouse_x{0}, g_mouse_y{0};    // client pixels, last move
std::atomic<int> g_anchor_x{0}, g_anchor_y{0};  // where the right button went down
// the wheel is a notch, not a position: each notch holds the zoom axis for a while
std::atomic<int64_t> g_zoom_until{0};  // steady-clock milliseconds
std::atomic<int> g_zoom_dir{0};        // +1 in, -1 out

constexpr float kDragRadius = 140.0f;  // pixels from the press point for a fully deflected stick
constexpr int64_t kZoomMsPerNotch = 90;
constexpr int64_t kZoomMsMax = 450;  // a spun wheel does not queue up seconds of zoom

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool Down(int vk) { return g_down[vk].load(std::memory_order_relaxed); }

void ReleaseAll() {
    for (std::atomic<bool>& key : g_down) key.store(false, std::memory_order_relaxed);
    g_rmb.store(false, std::memory_order_relaxed);
    g_mmb.store(false, std::memory_order_relaxed);
    g_zoom_until.store(0, std::memory_order_relaxed);
}

// -1, 0 or +1 from a pair of keys
float Axis(int neg, int pos) { return float(Down(pos) ? 1 : 0) - float(Down(neg) ? 1 : 0); }

// a direction into the console's stick: clamped to the circle a real stick reaches, so a diagonal
// is not 1.41 times a cardinal, and only applied where it pushes further than the pad already does
void Deflect(float x, float y, int16_t& out_x, int16_t& out_y) {
    const float len = std::sqrt(x * x + y * y);
    if (len > 1.0f) {
        x /= len;
        y /= len;
    }
    const int vx = int(x * 32767.0f), vy = int(y * 32767.0f);
    if (std::abs(vx) > std::abs(int(out_x))) out_x = int16_t(vx);
    if (std::abs(vy) > std::abs(int(out_y))) out_y = int16_t(vy);
}

void Press(uint8_t& analog) { analog = 0xFF; }  // the Xbox pressure byte; the game tests != 0

}  // namespace

bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    (void)hwnd;
    (void)result;
    switch (msg) {
        case WM_KEYDOWN:
            if (wp < 256) g_down[wp].store(true, std::memory_order_relaxed);
            break;
        case WM_KEYUP:
            if (wp < 256) g_down[wp].store(false, std::memory_order_relaxed);
            break;
        // alt is the window's (alt-tab, alt-F4): WM_SYSKEY* is deliberately not tracked
        case WM_SETFOCUS:
            g_focus.store(true, std::memory_order_relaxed);
            break;
        case WM_KILLFOCUS:
            g_focus.store(false, std::memory_order_relaxed);
            ReleaseAll();  // keys held at alt-tab get no WM_KEYUP
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                g_focus.store(false, std::memory_order_relaxed);
                ReleaseAll();
            } else {
                g_focus.store(true, std::memory_order_relaxed);
            }
            break;
        case WM_MOUSEMOVE:
            g_mouse_x.store(int(int16_t(lp & 0xFFFF)), std::memory_order_relaxed);
            g_mouse_y.store(int(int16_t((lp >> 16) & 0xFFFF)), std::memory_order_relaxed);
            break;
        case WM_RBUTTONDOWN:
            g_anchor_x.store(int(int16_t(lp & 0xFFFF)), std::memory_order_relaxed);
            g_anchor_y.store(int(int16_t((lp >> 16) & 0xFFFF)), std::memory_order_relaxed);
            g_mouse_x.store(g_anchor_x.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_mouse_y.store(g_anchor_y.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_rmb.store(true, std::memory_order_relaxed);
            break;
        case WM_RBUTTONUP:
            g_rmb.store(false, std::memory_order_relaxed);
            break;
        case WM_MBUTTONDOWN:
            g_mmb.store(true, std::memory_order_relaxed);
            break;
        case WM_MBUTTONUP:
            g_mmb.store(false, std::memory_order_relaxed);
            break;
        case WM_MOUSEWHEEL: {
            const int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (notches == 0) break;
            const int dir = notches > 0 ? 1 : -1;
            const int64_t now = NowMs();
            // a reversed wheel drops what is left of the other direction
            int64_t until = g_zoom_dir.exchange(dir, std::memory_order_relaxed) == dir
                                ? std::max(g_zoom_until.load(std::memory_order_relaxed), now)
                                : now;
            until += kZoomMsPerNotch * std::abs(notches);
            g_zoom_until.store(std::min(until, now + kZoomMsMax), std::memory_order_relaxed);
            break;
        }
        default:
            break;
    }
    return false;  // never takes a message
}

void Apply(int port, input::Pad& pad) {
    if (port != 0) return;
    pad.connected = true;  // the keyboard is the port 0 pad when no controller is plugged in
    // no focus: nothing is held. The overlay's settings panel owns both devices while it is up
    if (!g_focus.load(std::memory_order_relaxed) || overlay::SettingsOpen()) return;

    static std::atomic<bool> logged{false};

    // left stick: walk, and the menus that read the stick
    const float lx = Axis('A', 'D'), ly = Axis('S', 'W');
    if (lx != 0.0f || ly != 0.0f) Deflect(lx, ly, pad.lx, pad.ly);

    // right stick: the camera. Keys, the right-button drag and the wheel's zoom pulse add up
    float rx = Axis('Q', 'E'), ry = Axis('F', 'R');
    if (g_rmb.load(std::memory_order_relaxed)) {
        rx += float(g_mouse_x.load(std::memory_order_relaxed) - g_anchor_x.load(std::memory_order_relaxed)) / kDragRadius;
        // screen y grows downward, the stick's upward
        ry -= float(g_mouse_y.load(std::memory_order_relaxed) - g_anchor_y.load(std::memory_order_relaxed)) / kDragRadius;
    }
    if (NowMs() < g_zoom_until.load(std::memory_order_relaxed))
        ry += float(g_zoom_dir.load(std::memory_order_relaxed));
    if (rx != 0.0f || ry != 0.0f) Deflect(rx, ry, pad.rx, pad.ry);

    uint16_t buttons = 0;
    if (Down(VK_UP)) buttons |= 0x0001;
    if (Down(VK_DOWN)) buttons |= 0x0002;
    if (Down(VK_LEFT)) buttons |= 0x0004;
    if (Down(VK_RIGHT)) buttons |= 0x0008;
    if (Down(VK_RETURN)) buttons |= 0x0010;  // Start
    if (Down(VK_TAB)) buttons |= 0x0020;     // Back
    if (Down(VK_SHIFT)) buttons |= 0x0040;   // left stick click
    if (Down(VK_CONTROL) || g_mmb.load(std::memory_order_relaxed)) buttons |= 0x0080;  // right stick click
    pad.buttons = uint16_t(pad.buttons | buttons);

    uint8_t analog[8] = {};
    if (Down(VK_SPACE)) Press(analog[0]);                    // A
    if (Down(VK_ESCAPE) || Down(VK_BACK)) Press(analog[1]);  // B
    if (Down('X')) Press(analog[2]);                         // X
    if (Down('Y')) Press(analog[3]);                         // Y
    if (Down('1')) Press(analog[4]);                         // black
    if (Down('2')) Press(analog[5]);                         // white
    if (Down('Z')) Press(analog[6]);                         // left trigger
    if (Down('C')) Press(analog[7]);                         // right trigger
    for (int i = 0; i < 8; i++)
        if (analog[i] > pad.analog[i]) pad.analog[i] = analog[i];

    const bool active = buttons != 0 || lx != 0.0f || ly != 0.0f || rx != 0.0f || ry != 0.0f ||
                        std::any_of(std::begin(analog), std::end(analog), [](uint8_t b) { return b != 0; });
    if (active && !logged.exchange(true))
        LOGI("input: keyboard and mouse on port 0 (WASD walk, arrows D-pad, right drag and wheel camera)");
}

}  // namespace kbm
