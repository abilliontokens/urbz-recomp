#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

 #include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace {

// Opt-in self-test (URBZ_AUTOSTART): ~2 s after the first poll, hold Xbox Start
// (wButtons bit 4, 0x0010) for 500 ms on port 0, then release; repeat the pulse
// every 3 s. The intro/attract movies run for minutes and ignore an early press,
// so a single 2 s pulse can never reach the title screen; repeating pulses mean
// one lands whenever the game becomes responsive. Unset: no-op, pad untouched.
void AutostartPad(int port, input::Pad& pad) {
    static const bool enabled = [] {
        const char* v = std::getenv("URBZ_AUTOSTART");
        return v && v[0] && std::strcmp(v, "0") != 0;
    }();
    if (!enabled || port != 0) return;
    using namespace std::chrono;
    static std::atomic<int64_t> first_ms{0};
    static std::atomic<bool> pressed{false};
    const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t expected = 0;
    if (first_ms.compare_exchange_strong(expected, now)) {
        LOGI("urbz autostart: enabled, first poll on port 0; Start in ~2 s");
        return;
    }
    const int64_t elapsed = now - first_ms.load(std::memory_order_relaxed);
    const bool down = elapsed >= 2000 && (elapsed - 2000) % 3000 < 500;
    if (down) {
        pad.connected = true;
        pad.buttons = uint16_t(pad.buttons | 0x0010);  // Xbox Start
        if (!pressed.exchange(true)) LOGI("urbz autostart: synthetic Start pressed (port 0, wButtons=0x%04X)", pad.buttons);
    } else if (pressed.exchange(false)) {
        // Only ever OR Start in: never clear it, so a real press on the same
        // pad is not suppressed on non-press frames.
        LOGI("urbz autostart: synthetic Start released (port 0)");
    }
}
}  // namespace

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"The Urbz: Sims in the City",  // window and splash
    .short_title = L"The Urbz",              // message boxes
    .name = L"Urbz",                         // window classes, crash dumps, handover event
    .log = "urbz",                           // log file prefix
    .settings = "settings.toml",             // settings file beside the exe
    .splash = L"splash",                     // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its DATA and movies folders beside it.",
    .images = {{"default.xbe", "Urbz"}},     // each recompiled xbe and its exe, for title launches
    .options = {},                           // game settings kept in the settings file
    .load = nullptr,                         // reads more from the settings file's toml::table
    .save = nullptr,                         // writes more into it
    .init = nullptr,                         // after the settings load, before the window and boot
    .loaded = nullptr,                       // image in memory, before its entry point runs
    .window = nullptr,                       // the game window is up
    .message = nullptr,                      // window messages first; true takes one
    .ui = nullptr,                           // ImGui drawing each frame
    .pad = &AutostartPad,                    // changes a polled pad (URBZ_AUTOSTART self-test)
    .rumble = nullptr,                       // changes a rumble before it is set
};
