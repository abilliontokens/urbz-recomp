// The console cheats, entered the game's own way (common/cheats.h, offered in the F1 panel).
//
// Recogniser. sub_000126A0 runs at the top of the per-frame update sub_00011E00 whenever the active
// pad has a fresh press. It keeps the last six single-button presses (each within a second of the
// one before) and matches them against a 12-row table of up to six button masks that sub_0004A300
// writes into the options object at 0x35A868 + 6, rows 12 bytes apart, in the game's own pad layout
// (sub_001923A0: 0x1 White, 0x2 Black, 0x4 L, 0x8 R, 0x10 Y, 0x20 B, 0x40 A, 0x80 X, 0x1000 Up,
// 0x2000 Right, 0x4000 Down, 0x8000 Left). On a match it calls sub_0004A280(eax = 0x35A868, ecx =
// row): bit `row` of the word at 0x35A868 + 0xAE (0x35A916) flips and the cheat-on or cheat-off
// sound plays; row 11 only plays a sound. The front end's state entry sub_000A87F0 clears the word.
//
// Effect. The word's only reader is the SimAntics global getter sub_00116200, which hands it out as
// global 46. The cheat gnome (object zz_move_a__tron, DATA/edithtre.arc) shows itself while flag 1, 2
// or 3 of global 46 is set ("Cheat Code - Show Gnome") and offers one pie-menu entry per set flag.
// Edith counts flags from 1, so flag n+1 is row n:
//
//   row 0   Left Y Down A X          Get Shorty: the gnome appears
//   row 2   R L Down Black Left B    Fill All Motives (motives 5 7 8 9 14 15 := 100)
//   row 3   L Black Right X Left     Skill Objects (nine objects into the inventory)
//   row 5   Black Up Y Down R        Unlock All Socials (global 25 flag 5)
//   row 7   L R A Down Black         Max Physical Skill (person data 17 := 900)
//   row 8   L B A Black Down Y       Max Mental Skill (person data 18 := 900)
//   row 9   Y Down Black A B         Max Artistic Skill (person data 15 := 900)
//   row 10  Down Black Right X Left  Give All Powersocial Items
//
// So the host enters a code the way a matched sequence does: the same call, from the same place in
// the frame. The override of sub_00011E00 below calls sub_0004A280 for every code the F1 panel asked
// for, on the title thread, before the frame's own input pass.
//
// Team photo. The credits movie (tsc3_credits.apt) keeps the last six input codes in its on_input
// and attaches the development team's photo when they read DPAD_UP DPAD_DOWN SELECT DPAD_UP DPAD_DOWN
// SELECT; native code posts SELECT for action 5, the A button (sub_0001D630). Those six presses go
// to port 0 through the pad hook, and only while the credits are up.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iterator>

#include "common/cheats.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace {

// A guest address the host may read: inside one of the regions the guest maps (sdk/runtime/kernel/
// memory.h) and on committed pages, since a stale pointer into the reserved but uncommitted user
// range faults the host on the first read. The same rule as hud_state.cpp's.
bool GuestPtr(uint32_t va, uint32_t size) {
    if (size == 0 || va + size < va) return false;
    const uint32_t end = va + size;
    const bool in_range =
        (va >= mem::kUserLow && end <= mem::kUserHigh) ||
        (va >= mem::kContiguousBase && end <= uint32_t(mem::kContiguousBase + mem::kPhysicalSize)) ||
        (va >= mem::kSystemBase && end <= uint32_t(mem::kSystemBase + mem::kSystemSize));
    if (!in_range) return false;
    for (uint32_t page = va & ~(mem::kPage - 1u); page < end; page += mem::kPage)
        if (!mem::IsCommitted(page)) return false;
    return true;
}

// The options object the code table and the mask live in. [+0x284] is the developer switch
// enable_all_player_cheats (registered by sub_00025290), which turns every code on for the getter.
constexpr uint32_t kOptions = 0x35A868u;
constexpr uint32_t kMask = kOptions + 0xAEu;
constexpr uint32_t kAllCheats = kOptions + 0x284u;

// sub_0004A280: the options object in eax, the table row in ecx
constexpr uint32_t kEnterCode = 0x4A280u;
constexpr hle::Map kEnterAbi = hle::Make({hle::eax, hle::ecx});

// The front end's flow. The movies write flowGoto; sub_0012C140 turns the target into a number at
// [flow+0x84] (flowGoto_request) and the state machine copies it to [flow+0x88] (flowGoto_current)
// once it has switched. The flow object hangs off the 'SIMS' system at +0x88 (sub_000AA260); the
// systems are the vector at *[0x2EDD18] (begin, end, capacity: sub_00011300), each tagged with its
// FourCC at +0x18 (sub_00011180).
constexpr uint32_t kSystems = 0x2EDD18u;
constexpr uint32_t kSimsTag = 0x53494D53u;
constexpr uint32_t kFlowIngame = 4;  // "ingame" and every "HUD_*" target
constexpr uint32_t kFlowCredits = 19;

// the loaded lot's SimAntics globals: built with the lot (sub_000BE4C0), zeroed with it (sub_000BE6C0)
constexpr uint32_t kLotGlobals = 0x2EDF60u;

bool Flow(uint32_t& request, uint32_t& current) {
    const uint32_t list = kernel::R32(kSystems);
    if (!GuestPtr(list, 12)) return false;
    const uint32_t begin = kernel::R32(list), end = kernel::R32(list + 4u);
    if (end <= begin || end - begin > 32u * 4u || (end - begin) % 4u != 0 || !GuestPtr(begin, end - begin))
        return false;
    for (uint32_t p = begin; p < end; p += 4u) {
        const uint32_t system = kernel::R32(p);
        if (!GuestPtr(system, 0x90) || kernel::R32(system + 0x18u) != kSimsTag) continue;
        const uint32_t flow = kernel::R32(system + 0x88u);
        if (!GuestPtr(flow, 0x8C)) return false;
        request = kernel::R32(flow + 0x84u);
        current = kernel::R32(flow + 0x88u);
        return true;
    }
    return false;
}

// the cheat mask as the gnome sees it
uint32_t GameMask() {
    if (!GuestPtr(kOptions, 0x288)) return 0;
    if (*static_cast<const uint8_t*>(kernel::HostPtr(kAllCheats)) != 0) return 0xFFFFu;
    return kernel::R16(kMask);
}

const char* Name(int code) {
    switch (code) {
        case cheats::kGnome: return "Get Shorty (cheat gnome)";
        case cheats::kFillMotives: return "Fill All Motives";
        case cheats::kSkillObjects: return "Acquire Skill (Skill Objects)";
        case cheats::kSocials: return "Unlock All Socials";
        case cheats::kMaxPhysical: return "Max Physical Skill";
        case cheats::kMaxMental: return "Max Mental Skill";
        case cheats::kMaxArtistic: return "Max Artistic Skill";
        case cheats::kPowerSocials: return "Power Social (Give All Powersocial Items)";
    }
    return "unknown";
}

// codes the panel asked to enter, one bit each; a second click before the title thread takes the
// first cancels it, as entering the code twice would
std::atomic<uint32_t> g_toggle{0};

// Up Down A Up Down A, each held 130 ms with 130 ms between: URBZ_STEP's own pulse (game_info.cpp),
// which every menu takes one press at a time
constexpr int64_t kPulseMs = 130;
enum class Press : uint8_t { Up, Down, A };
constexpr Press kPhotoPresses[] = {Press::Up, Press::Down, Press::A, Press::Up, Press::Down, Press::A};
std::atomic<int64_t> g_photo_since{0};  // steady ms the presses began, 0 when none are going out

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The title thread, at the top of its frame: enter what the panel asked for. The world is checked
// again here because the click came from the render thread a frame ago.
void EnterPending(X86Context& ctx) {
    const uint32_t pending = g_toggle.exchange(0, std::memory_order_relaxed);
    if (!pending) return;
    if (!cheats::InCity()) {
        LOGI("cheats: codes %04X not entered, the city was gone by the next frame", pending);
        return;
    }
    // flags are live only across tail calls, but the update reads them on entry: keep them as found
    const uint32_t cf = ctx.cf, zf = ctx.zf, sf = ctx.sf, of = ctx.of, pf = ctx.pf, df = ctx.df;
    for (int code = 0; code < 16; code++) {
        if (!(pending & (1u << code))) continue;
        hle::Call(ctx, kEnterCode, kEnterAbi, {kOptions, uint32_t(code)});
        const uint32_t mask = GameMask();
        LOGI("cheats: entered %s (code %d): %s, the game's cheat mask is now %04X", Name(code), code,
             (mask >> code) & 1u ? "on" : "off", mask);
    }
    ctx.cf = cf;
    ctx.zf = zf;
    ctx.sf = sf;
    ctx.of = of;
    ctx.pf = pf;
    ctx.df = df;
}

}  // namespace

namespace cheats {

bool InCity() {
    uint32_t request = 0, current = 0;
    if (!Flow(request, current) || request != kFlowIngame || current != kFlowIngame) return false;
    return GuestPtr(kernel::R32(kLotGlobals), 0x78);
}

bool InCredits() {
    uint32_t request = 0, current = 0;
    return Flow(request, current) && request == kFlowCredits && current == kFlowCredits;
}

bool On(Code c) {
    const uint32_t bit = 1u << c;
    return ((GameMask() ^ g_toggle.load(std::memory_order_relaxed)) & bit) != 0;
}

void Toggle(Code c) {
    if (InCity()) g_toggle.fetch_xor(1u << c, std::memory_order_relaxed);
}

bool TeamPhoto() {
    if (!InCredits()) return false;
    int64_t idle = 0;
    if (!g_photo_since.compare_exchange_strong(idle, NowMs(), std::memory_order_relaxed)) return false;
    LOGI("cheats: team photo, pressing Up Down A Up Down A on port 0");
    return true;
}

bool TeamPhotoRunning() { return g_photo_since.load(std::memory_order_relaxed) != 0; }

// Every pad poll, on the guest threads and the port poller: time-based, so any number of pollers
// see the same press. Only adds to what the controller and the keyboard reported.
void Pad(int port, input::Pad& pad) {
    if (port != 0) return;
    const int64_t since = g_photo_since.load(std::memory_order_relaxed);
    if (!since) return;
    const int64_t step = std::max<int64_t>(NowMs() - since, 0) / kPulseMs;
    const bool done = step >= 2 * int64_t(std::size(kPhotoPresses));
    // the player may close the credits halfway: the rest must not land on the main menu
    if (done || !InCredits()) {
        int64_t running = since;
        if (g_photo_since.compare_exchange_strong(running, 0, std::memory_order_relaxed)) {
            if (done) LOGI("cheats: team photo, sequence sent");
            else LOGI("cheats: team photo stopped, the credits closed");
        }
        return;
    }
    if (step & 1) return;  // released between presses
    pad.connected = true;
    switch (kPhotoPresses[step / 2]) {
        case Press::Up: pad.buttons = uint16_t(pad.buttons | 0x0001u); break;
        case Press::Down: pad.buttons = uint16_t(pad.buttons | 0x0002u); break;
        case Press::A: pad.analog[0] = 0xFF; break;
    }
}

}  // namespace cheats

X86_FUNC(__imp__sub_00011E00);

// sub_00011E00: the application's per-frame update (vtable 0x2BD0F0), whose first act is the
// game's own cheat-code recogniser; pending codes are entered just ahead of it
X86_FUNC(sub_00011E00) {
    LOG_FIRST(1, "cheats: frame hook reached");
    if (g_toggle.load(std::memory_order_relaxed)) EnterPending(ctx);
    __imp__sub_00011E00(ctx, base);
}
