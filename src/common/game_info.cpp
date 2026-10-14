#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>

 #include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace {

// Opt-in self-test (URBZ_AUTOSTART): a scripted pad on port 0, repeating a 10 s
// cycle from ~2 s after the first poll: Start (wButtons 0x0010) held 0-0.5 s,
// D-pad Up (wButtons 0x0001) held 3-3.5 s, then A (analog[0] = 0xFF, the Xbox
// pressure byte, not a wButtons bit) held 5-5.5 s, all released between. The
// intro/attract movies run for minutes and ignore early presses, so the cycle
// repeats: a Start lands whenever the title is responsive and an A whenever a
// menu is. Only ever ORs bits/bytes in,
// never clears a real press. Unset: no-op, pad untouched.
void AutostartPad(int port, input::Pad& pad) {
    static const bool enabled = [] {
        const char* v = std::getenv("URBZ_AUTOSTART");
        return v && v[0] && std::strcmp(v, "0") != 0;
    }();
    if (!enabled || port != 0) return;
    using namespace std::chrono;
    static std::atomic<int64_t> first_ms{0};
    static std::atomic<bool> start_down{false}, a_down{false}, up_down{false};
    const int64_t now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    int64_t expected = 0;
    if (first_ms.compare_exchange_strong(expected, now)) {
        LOGI("urbz autostart: enabled, first poll on port 0; scripted Start/A cycle from ~2 s");
        return;
    }
    const int64_t elapsed = now - first_ms.load(std::memory_order_relaxed);
    const int64_t phase = elapsed >= 2000 ? (elapsed - 2000) % 10000 : -1;
    const bool start = phase >= 0 && phase < 500;
    const bool up = phase >= 3000 && phase < 3500;
    const bool a = phase >= 5000 && phase < 5500;
    if (start) {
        pad.connected = true;
        pad.buttons = uint16_t(pad.buttons | 0x0010);  // Xbox Start
        if (!start_down.exchange(true))
            LOGI("urbz autostart: synthetic Start pressed (port 0, wButtons=0x%04X)", pad.buttons);
    } else if (start_down.exchange(false)) {
        LOGI("urbz autostart: synthetic Start released (port 0)");
    }
    if (up) {
        pad.connected = true;
        pad.buttons = uint16_t(pad.buttons | 0x0001);  // Xbox D-pad Up (e.g. No -> Yes)
        if (!up_down.exchange(true)) LOGI("urbz autostart: synthetic DPadUp pressed (port 0, wButtons=0x%04X)", pad.buttons);
    } else if (up_down.exchange(false)) {
        LOGI("urbz autostart: synthetic DPadUp released (port 0)");
    }
    if (a) {
        pad.connected = true;
        if (pad.analog[0] < 0xFF) pad.analog[0] = 0xFF;  // Xbox A, pressure byte
        if (!a_down.exchange(true)) LOGI("urbz autostart: synthetic A pressed (port 0, analog[0]=0xFF)");
    } else if (a_down.exchange(false)) {
        LOGI("urbz autostart: synthetic A released (port 0)");
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
