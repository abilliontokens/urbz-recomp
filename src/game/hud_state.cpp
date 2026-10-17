// The modern HUD's two halves that only make sense for this image: reading what the game's own HUD
// shows out of guest memory, and keeping the game from drawing the parts the host replaces.
//
// Values. The HUD's own cache object ("HUDData", 0x978 bytes) hangs off [0x35A740]; the game fills
// it on the title thread once a frame and the Apt movie reads it through HUD_bar{i}/HUD_rep. The
// host reads the same cache:
//
//   H = *[0x35A740], player p = [H+0xF9] & 1
//   H + 0xA8 + 6p + 0..4  five motive percents, 0..100, in the order the HUD draws them
//                         (bladder, hygiene, hunger, fun, energy - Sim attributes 9, 8, 7, 15, 5)
//   H + 0xA8 + 6p + 5     mood percent, 0..100, a weighted average of the five
//   H + 0x96C/0x970/0x974 + 2p   reputation bar base, current and next unlock, int16
//   G = *[0x2EDF60]; int16 [G+0x14] hour 0..23, int16 [G+0x1E] minute 0..59
//
// The percent bytes are deliberate: two writers use the raw int16 twin at H+0xB4 with different
// scales (one stores the -100..100 motive, the other (value+100)/2), while the percent byte is
// always 0..100 and is exactly what the movie is handed.
//
// Hiding. The game's HUD is the Apt movie `tsc3_ingame`, whose clips `hud0`/`hud1` hold both corner
// groups *and* things the modern HUD does not replace - the interaction pie menu
// (mc_actionQueue/mc_actionProto), simoleons, the ringtone and job icons - while NPC dialogue, the
// lot name and modeless dialogs sit beside them on the `ingame` root. So neither the game's own
// `hideHud` nor hiding hud0/hud1 wholesale is right: both take the pie menu with them.
//
// Instead the host skips exactly the corner clips in the Apt per-clip renderer `sub_002153B0`
// (clip in ecx, three stack arguments, `ret 0x0C`). Its callers ignore its return value and the
// transform stack is pushed and popped in the caller, so an early return is what the renderer
// already does for a clip with the hidden bit 0x8000 set. The movie keeps running and keeps
// refreshing the caches above - only the drawing is skipped.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "common/hud.h"
#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

// the clips that make up the two corner groups the modern HUD draws; everything else in hud0/hud1
// keeps drawing, which is what leaves the pie menu, the simoleons and the job icons alone
bool CornerClip(const char* name) {
    // every display object in the movie comes through here each frame, so reject on the prefix
    // before any compare: every name below starts with "mc_"
    if (name[0] != 'm' || name[1] != 'c' || name[2] != '_') return false;
    static const char* const kNames[] = {
        "mc_rep",        "mc_repstar", "mc_repTextDisplay", "mc_clockText", "mc_meterClockPoll",
        "mc_meters",     "mc_mood",    "mc_meterbar0",      "mc_meterbar1", "mc_meterbar2",
        "mc_meterbar3",  "mc_meterbar4",
    };
    for (const char* n : kNames)
        if (!std::strcmp(name + 3, n + 3)) return true;
    return false;
}

// What counts as a guest pointer. The address space is not one flat low region: the image and heap
// sit in the user range, but every Apt display object the HUD is built from lives in the physical
// window at 0x80000000 (clips log as 0x83xxxxxx), and host-side scratch comes from the system pool
// at 0xB0000000. An earlier bound of 0x8000000 covered only the first of those, so every clip
// pointer, every dataset pointer and every probe argument was rejected as "not a pointer" and the
// lookups silently returned nothing - the one constant behind the dead probes, the unsuppressed
// HUD and the missing reputation level. Ranges mirror sdk/runtime/kernel/memory.h.
// The range alone is not enough: the user range is 2 GB of mostly uncommitted address space, so a
// value that happens to be string bytes ("ise" -> 0x00657369) passes a range test and faults the
// host on the first read. Every page the read spans must therefore be committed as well. The HUD
// only ever reads a handful of fields per frame, so the page check costs nothing that matters.
bool GuestPtr(uint32_t va, uint32_t size = 1) {
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


float Percent(uint32_t va) { return std::clamp(float(*static_cast<const uint8_t*>(kernel::HostPtr(va))) / 100.0f, 0.0f, 1.0f); }

// The movie's own visibility bit: the display-list walk (sub_229760) renders a child only when
// bit 0x8000 of [child+0x04] is set, and that bit is what ActionScript's _visible writes.
constexpr uint32_t kVisibleBit = 0x8000u;

// hud0 itself, the container the status clips hang under. It is never hidden - the pie menu and
// the lot name live under it too - but its own visibility is the game saying "my HUD is up".
std::atomic<uint32_t> g_hud_container{0};

// The action queue's controller, captured from its constructor (sub_12FF00, `this` in esi). It
// owns the bottom-right panel: an array of queued actions at +168 with a 20 byte stride, how
// many of them at +336, which one is running at +777, and the player it belongs to at +789.
// The slots themselves are movie clips whose pictures the game renders, which is why the modern
// strip redraws the queue rather than restyling it.
std::atomic<uint32_t> g_queue_ctrl{0};

// The queue's clips. The slot plates and arrows are hidden like the rest of the old HUD; the
// picture clips are kept, because what the sim is about to do is drawn into them by the game and
// the host has no way to redraw it.
std::atomic<uint32_t> g_queue_clip{0};
std::atomic<uint32_t> g_queue_icons[9];

// The HUD object the game itself works on. sub_142430 refreshes a motive byte on it and
// sub_141700 reads that byte straight back out, so the object passed to the refresh is the one
// whose +0xA8 block holds live values - and it is not what *[0x35A740] points at, which is why
// the motives read as zeroes while the game's own bars were full. Captured from the game's call
// rather than guessed from a global.
std::atomic<uint32_t> g_hud_data{0};

uint32_t HudData() {
    const uint32_t captured = g_hud_data.load(std::memory_order_relaxed);
    return captured ? captured : kernel::R32(0x35A740u);
}

}  // namespace

namespace hud {

// Whether the game is showing its own HUD right now. It hides it for cutscenes, the XAM, the
// pause menu, Create-an-Urb and the way back to the main menu, and the modern HUD goes with it.
//
// This used to compare a frame counter bumped here against a stamp written by the render hook,
// which starved the HUD: the two run at different rates, so the stamp read "stale" on almost
// every frame and Read bailed out. The container's own visibility bit answers the same question
// directly, with no clocks to drift - it is the bit the game's script writes, and the status
// clips hidden underneath it are separate clips whose own bits are the ones cleared.
bool GameHudVisible() {
    const uint32_t container = g_hud_container.load(std::memory_order_relaxed);
    return container != 0 && (kernel::R32(container + 0x4u) & kVisibleBit) != 0;
}

bool Read(State& out) {
    if (!GameHudVisible()) return false;
    const uint32_t h = HudData();
    LOG_FIRST(3, "hud: read, HUDData %08X, globals %08X", h, kernel::R32(0x2EDF60u));
    if (!GuestPtr(h, 0x980)) return false;  // before the city is up there is no HUD data
    const uint32_t p = *static_cast<const uint8_t*>(kernel::HostPtr(h + 0xF9u)) & 1u;
    const uint32_t m = h + 0xA8u + 6u * p;
    out.bladder = Percent(m + 0);
    out.hygiene = Percent(m + 1);
    out.hunger = Percent(m + 2);
    out.fun = Percent(m + 3);
    out.energy = Percent(m + 4);
    out.mood = Percent(m + 5);
    {
        static std::atomic<uint32_t> n{0};
        if (const uint32_t i = n.fetch_add(1, std::memory_order_relaxed); i % 300 == 0) {
            const uint8_t* b = static_cast<const uint8_t*>(kernel::HostPtr(m));
            LOGI("hud: motives H %08X p %u @%08X: %u %u %u %u %u mood %u", h, p, m, b[0], b[1], b[2],
                 b[3], b[4], b[5]);
        }
    }

    // HUD_rep, the number in the game's own box, is the reputation itself: sub_142180 returns
    // [H+0x970+2p] and the movie formats it straight into the box. The twin at +0x96C is the
    // low-water mark that function keeps, and +0x974 the next unlock, which is what the bar
    // beside the box fills towards.
    const int16_t base = int16_t(kernel::R16(h + 0x96Cu + 2u * p));
    const int16_t cur = int16_t(kernel::R16(h + 0x970u + 2u * p));
    const int16_t next = int16_t(kernel::R16(h + 0x974u + 2u * p));
    // a level-up writes the three in turn, so a frame can see a torn set: clamping is enough
    out.progress = next > base ? std::clamp(float(cur - base) / float(next - base), 0.0f, 1.0f) : 0.0f;
    out.rep = std::clamp(int(cur), 0, 9999);

    const uint32_t g = kernel::R32(0x2EDF60u);
    if (g >= 0x10000u) {
        out.hour = std::clamp(int(int16_t(kernel::R16(g + 0x14u))), 0, 23);
        out.minute = std::clamp(int(int16_t(kernel::R16(g + 0x1Eu))), 0, 59);
    }

    // the action queue, for the strip that replaces the game's bottom-right panel
    const uint32_t q = g_queue_ctrl.load(std::memory_order_relaxed);
    if (GuestPtr(q, 0x320)) {
        const uint32_t count = kernel::R32(q + 336u);
        out.queued = count <= 9u ? int(count) : 0;
        const int active = int(*static_cast<const uint8_t*>(kernel::HostPtr(q + 777u)));
        out.queue_active = active < out.queued ? active : -1;
    }
    return true;
}

}  // namespace hud

// Identity, the game's own way. The renderer gets a display object with no name I could read
// reliably - the first attempt at [obj+0x0C] matched nothing at all - but the game resolves its
// HUD's parts by name itself, with sub_001FA850(parent, name, 0) returning the child in eax.
// Hooking that gives the exact pointers the HUD is built from, so hiding needs no layout
// guesswork. Every distinct name the game asks for is logged once.
namespace {

constexpr int kMaxClips = 32;
std::atomic<uint32_t> g_hud_clips[kMaxClips];

void RememberClip(uint32_t clip) {
    for (int i = 0; i < kMaxClips; i++) {
        uint32_t cur = g_hud_clips[i].load(std::memory_order_relaxed);
        if (cur == clip) return;
        if (cur == 0 && g_hud_clips[i].compare_exchange_strong(cur, clip, std::memory_order_relaxed)) return;
    }
}

// The movie's own visibility bit. The display-list walk (sub_229760) renders a child only when
// bit 0x8000 of [child+0x04] is set, chaining siblings through [child+0x58]; that bit is what
// ActionScript's _visible writes. Clearing it for the legacy status clips makes the game skip
// them and their whole subtree exactly as it skips anything the script hid, so nothing is drawn
// and then covered up.
//
// The bit stays cleared. Clearing it only around the walk put the HUD straight back on screen:
// the walk records the clips and the draw happens after it returns, so a bit restored at the end
// of the walk is restored before anything reads it. Re-clearing every walk also covers the
// script turning its clips back on when it animates the HUD in.
void HideLegacyClips() {
    for (int i = 0; i < kMaxClips; i++) {
        const uint32_t clip = g_hud_clips[i].load(std::memory_order_relaxed);
        if (clip == 0) break;
        const uint32_t flags = kernel::R32(clip + 0x4u);
        if (flags & kVisibleBit) kernel::W32(clip + 0x4u, flags & ~kVisibleBit);
    }
}

// a guest C string, bounded
bool GuestString(uint32_t va, char (&buf)[64]) {
    if (!GuestPtr(va, sizeof buf)) return false;
    const char* p = static_cast<const char*>(kernel::HostPtr(va));
    for (size_t i = 0; i < sizeof buf; i++) {
        const char c = p[i];
        if (c == '\0') {
            buf[i] = '\0';
            return i > 0;
        }
        if (c < 0x20 || c > 0x7E) return false;
        buf[i] = c;
    }
    return false;
}

void LogNameOnce(const char* name) {
    static std::set<std::string> seen;
    static std::mutex m;
    std::lock_guard lock(m);
    if (seen.size() > 400 || !seen.insert(name).second) return;
    LOGI("hud: apt child \"%s\"", name);
}

}  // namespace


X86_FUNC(__imp__sub_001FA850);
X86_FUNC(__imp__sub_00229760);
X86_FUNC(__imp__sub_00142430);
X86_FUNC(__imp__sub_001416C0);
X86_FUNC(__imp__sub_0012FF00);

// The movie's own ActionScript binds HUD_bar{i} to its bars, so native code never resolves the
// meters, mood, reputation or clock clips by name - the capture only ever saw the pie menu. The
// same helper resolves any child by name though, so the host asks it for the HUD's own clips,
// reusing a parent the game just used.
//
// The names are not guesses: they are the movie's own symbols, read out of DATA/flashes.arc, where
// the HUD script does attachMovie("HUD", "hud0"), then touches mc_meters, mc_meters_job,
// mc_actionProto and mc_actionQueue under it and sets mc_npcDialog._visible beside it. Each one
// that resolves is logged with its pointer, so a name that is wrong here is visible, not silent.
namespace {

// The helper resolves ONE path segment per call: the game itself walks "ingame" -> "hud0" ->
// "mc_actionProto" -> "mc_actionMenu", each from the previous result (seen in the log as four
// separate lookups with the parent threading through). Whole-path probes therefore never matched
// anything. The host walks the same way, from the root the game just used.
//
// Only the status read-outs are listed. The pie menu (mc_actionProto, mc_actionQueue) and the lot
// name stay on screen: they are interaction, not status, and the modern HUD does not replace them.
const char* const kLegacyHudClips[] = {
    "mc_meters",   "mc_meters_job", "mc_simoleons",        "mc_rep",
    "mc_repstar",  "mc_repTextDisplay", "mc_mood",         "mc_moodgreen",
    "mc_faces",    "mc_faces_back", "mc_clockText",        "mc_meterClockPoll",
    "mc_meterbar0", "mc_meterbar1", "mc_meterbar2",        "mc_meterbar3",
    "mc_meterbar4", "mc_jobIcons",  "mc_ringtone",
};

uint32_t g_probe_strings = 0;
std::atomic<bool> g_probed{false};

// one guest call to the child-by-name helper, with our own arguments on the guest stack
uint32_t ChildByName(X86Context& ctx, uint8_t* base, uint32_t parent, uint32_t name_va) {
    const uint32_t esp = ctx.esp, eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    ctx.esp -= 16;
    kernel::W32(ctx.esp + 0x0u, 0);  // return address, the body does not use it
    kernel::W32(ctx.esp + 0x4u, parent);
    kernel::W32(ctx.esp + 0x8u, name_va);
    kernel::W32(ctx.esp + 0xCu, 0);
    __imp__sub_001FA850(ctx, base);
    const uint32_t child = ctx.eax;
    ctx.esp = esp;
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
    return child;
}

uint32_t Segment(X86Context& ctx, uint8_t* base, uint32_t parent, const char* name) {
    if (!GuestPtr(parent) || !g_probe_strings) return 0;
    std::strncpy(static_cast<char*>(kernel::HostPtr(g_probe_strings)), name, 63);
    return ChildByName(ctx, base, parent, g_probe_strings);
}

// Hide every status clip under one parent. The HUD movie is attached twice in two-player games
// (hud0/hud1, each with a player_hud0 inside), so the same names are asked for under each parent
// that exists rather than assuming one fixed depth.
int HideStatusClips(X86Context& ctx, uint8_t* base, uint32_t parent, const char* what) {
    int hidden = 0;
    for (const char* n : kLegacyHudClips) {
        const uint32_t clip = Segment(ctx, base, parent, n);
        if (!GuestPtr(clip)) continue;
        RememberClip(clip);
        hidden++;
        LOGI("hud: hiding %s/%s -> %08X", what, n, clip);
    }
    return hidden;
}

void ProbeHudClips(X86Context& ctx, uint8_t* base, uint32_t root) {
    static std::atomic<int> tries{0};
    if (g_probed.load(std::memory_order_relaxed)) return;
    if (tries.fetch_add(1, std::memory_order_relaxed) > 60) return;
    if (!g_probe_strings) g_probe_strings = mem::AllocSystem(64);
    if (!g_probe_strings) return;

    const uint32_t ingame = Segment(ctx, base, root, "ingame");
    LOGI("hud: walk root %08X -> ingame %08X", root, ingame);
    if (!GuestPtr(ingame)) return;

    int hidden = 0;
    for (const char* hud : {"hud0", "hud1"}) {
        const uint32_t h = Segment(ctx, base, ingame, hud);
        if (!GuestPtr(h)) continue;
        g_hud_container.store(h, std::memory_order_relaxed);  // "the game's HUD is up" comes from this
        hidden += HideStatusClips(ctx, base, h, hud);
        for (const char* inner : {"player_hud0", "player_hud1"}) {
            const uint32_t ph = Segment(ctx, base, h, inner);
            if (GuestPtr(ph)) hidden += HideStatusClips(ctx, base, ph, inner);
        }
        // The action queue is the panel in the bottom right: a translucent purple strip of slots
        // with a picture of whatever the sim is doing in each. The pictures are worth keeping -
        // they say *what* is queued, and the game renders them into clips the host cannot draw -
        // so only the strip's own artwork goes. Each slot is three clips under the queue
        // (sub_12FF00 binds them): "aq%d" the plate, "aqa%d" the arrow, "aqi%d" the picture.
        // Hiding the first two and keeping the third leaves the icons floating, and the modern
        // chips are drawn around them.
        const uint32_t proto = Segment(ctx, base, h, "mc_actionProto");
        const uint32_t queue = proto ? Segment(ctx, base, proto, "mc_actionQueue") : 0u;
        LOGI("hud: %s/mc_actionProto -> %08X, mc_actionQueue -> %08X", hud, proto, queue);
        if (GuestPtr(queue, 4)) {
            g_queue_clip.store(queue, std::memory_order_relaxed);
            for (int i = 0; i < 9; i++) {
                char name[16];
                std::snprintf(name, sizeof name, "aq%d", i);
                const uint32_t plate = Segment(ctx, base, queue, name);
                std::snprintf(name, sizeof name, "aqa%d", i);
                const uint32_t arrow = Segment(ctx, base, queue, name);
                std::snprintf(name, sizeof name, "aqi%d", i);
                const uint32_t icon = Segment(ctx, base, queue, name);
                LOGI("hud: queue slot %d plate %08X arrow %08X icon %08X", i, plate, arrow, icon);
                if (GuestPtr(plate, 4)) {
                    RememberClip(plate);
                    hidden++;
                }
                if (GuestPtr(arrow, 4)) {
                    RememberClip(arrow);
                    hidden++;
                }
                if (GuestPtr(icon, 4)) g_queue_icons[i].store(icon, std::memory_order_relaxed);
            }
        }
    }
    // the dialogue boxes hang off ingame, beside hud0 - the script sets mc_npcDialog._visible there
    for (const char* n : {"mc_npcDialog", "mc_npc2Dialog"}) {
        const uint32_t clip = Segment(ctx, base, ingame, n);
        LOGI("hud: ingame/%s -> %08X", n, clip);
        if (GuestPtr(clip)) {
            RememberClip(clip);
            hidden++;
        }
    }
    LOGI("hud: %d legacy clips hidden", hidden);
    if (hidden) g_probed.store(true, std::memory_order_relaxed);
}

}  // namespace

// sub_001FA850(parent, name, flag) -> child in eax; cdecl, the caller pops
X86_FUNC(sub_001FA850) {
    LOG_FIRST(3, "hud: child lookup hook reached");
    char name[64];
    const uint32_t parent = kernel::R32(ctx.esp + 0x4u);  // read before the call: it pops on return
    const bool named = GuestString(kernel::R32(ctx.esp + 0x8u), name);
    __imp__sub_001FA850(ctx, base);
    if (!named) return;
    LogNameOnce(name);
    LOG_FIRST(60, "hud: game lookup \"%s\" parent %08X -> %08X", name, parent, ctx.eax);
    if (CornerClip(name) && GuestPtr(ctx.eax)) RememberClip(ctx.eax);
    // a path the game resolved itself names a parent the HUD hangs under: ask it for ours too
    if (!std::strncmp(name, "ingame/", 7)) ProbeHudClips(ctx, base, parent);
}

// sub_001416C0(this = HUD object in ecx, name): the movie's variable getter. Whatever the game
// passes here is the live HUD object by definition, so the host takes its base from the game
// rather than trusting a global to still mean what it meant at boot.
X86_FUNC(sub_001416C0) {
    const uint32_t object = ctx.ecx;
    if (GuestPtr(object, 0x980) && g_hud_data.load(std::memory_order_relaxed) != object) {
        g_hud_data.store(object, std::memory_order_relaxed);
        LOGI("hud: hud object %08X (global holds %08X)", object, kernel::R32(0x35A740u));
    }
    __imp__sub_001416C0(ctx, base);
}

// sub_0012FF00(this in esi): the action queue controller's constructor, which is where it binds
// the bottom-right panel's clips. Capturing `this` is the only way to the queue's own state -
// how many actions are lined up and which one is running - since none of it is exposed as a HUD
// variable. Only player one's controller is kept; the second one belongs to hud1.
X86_FUNC(sub_0012FF00) {
    const uint32_t ctrl = ctx.esi;  // read first: the body uses esi as scratch
    __imp__sub_0012FF00(ctx, base);
    if (!GuestPtr(ctrl, 0x320)) return;
    if (*static_cast<const uint8_t*>(kernel::HostPtr(ctrl + 789u)) != 0) return;
    g_queue_ctrl.store(ctrl, std::memory_order_relaxed);
    LOGI("hud: action queue controller %08X", ctrl);
}

// The motive percentages are a cache, not state: sub_142430 asks the sim for one motive through
// its virtual table and writes the byte at +0xA8 + 6*player + bar that the HUD then reads back.
// Nothing fills that cache on its own - the movie's bars pull it while they are on screen, and
// hiding them stops the pulling, which is why the bars read as flat zeroes once the legacy HUD
// was suppressed. So the host pulls instead: six calls a frame, the same six the game makes when
// its own bars are up, with the bar index in ebx and the object and player on the stack the way
// the function is written. Registers and the stack are put back, so the caller sees nothing.
void RefreshMotives(X86Context& ctx, uint8_t* base) {
    const uint32_t object = g_hud_data.load(std::memory_order_relaxed);
    if (!GuestPtr(object, 0x980)) return;
    const uint32_t player = *static_cast<const uint8_t*>(kernel::HostPtr(object + 0xF9u)) & 1u;
    const uint32_t esp = ctx.esp, eax = ctx.eax, ebx = ctx.ebx, ecx = ctx.ecx, edx = ctx.edx;
    for (uint32_t bar = 0; bar < 6; bar++) {
        ctx.ebx = bar;
        ctx.esp = esp - 16;
        kernel::W32(ctx.esp + 0x0u, 0);  // return address, the body does not use it
        kernel::W32(ctx.esp + 0x4u, object);
        kernel::W32(ctx.esp + 0x8u, player);
        __imp__sub_00142430(ctx, base);
    }
    ctx.esp = esp;
    ctx.eax = eax;
    ctx.ebx = ebx;
    ctx.ecx = ecx;
    ctx.edx = edx;
}

// The Apt display-list walk, once per frame per movie. Two things ride on it: the legacy status
// clips get their visibility bit cleared so the game itself skips them (no second HUD painted
// over the first), and the motive cache the modern HUD reads is refreshed, because suppressing
// the game's bars also stops them pulling those values.
//
// The bit stays cleared - clearing it only around the walk put the HUD straight back on screen,
// since the draw happens after the walk returns. Re-clearing every frame also covers the script
// turning its clips back on when it animates the HUD in. The walk recurses (a clip renders its
// children through sub_2153B0, which calls back in here), so only the outermost call does this.
X86_FUNC(sub_00229760) {
    LOG_FIRST(2, "hud: display list walk hook reached");
    static thread_local int depth = 0;
    const bool outer = depth++ == 0;
    if (outer && hud::g_enabled) {
        HideLegacyClips();
        RefreshMotives(ctx, base);
    }
    __imp__sub_00229760(ctx, base);
    --depth;
}

// The NPC dialogue, from the queue HUDData owns. Native enqueues a record and the movie *pulls*
// the text out through Apt getters, so the record is the host's read point too:
//
//   H     = *[0x35A740]                 the same HUDData the motives come from
//   state = [H+0xE0]   1 = opening, 2 = visible, 3 = hiding, 0 = idle
//   D     = [H+0xD4]   the queue head, the record on screen
//   [D+0x04] body, [D+0x08] speaker, [D+0x0C] continue label: string objects whose +0x00 is a
//   UTF-16LE buffer and +0x04 its length
//
// In state 3 the head has already advanced to the *next* dialogue, so nothing is drawn then.
// Colour lives in the text as <COLOR_AARRGGBB> tags, which switch colour until the next one; every
// other <...> tag (PARTICLE, FONT, BUTTON, PULSE) is display-irrelevant and dropped.
namespace {

// one UTF-16 code unit appended as UTF-8; the game's strings are Latin text plus punctuation
void AppendUtf8(char* out, int& n, int cap, uint32_t c) {
    if (c < 0x80 && n + 1 < cap) {
        out[n++] = char(c);
    } else if (c < 0x800 && n + 2 < cap) {
        out[n++] = char(0xC0 | (c >> 6));
        out[n++] = char(0x80 | (c & 0x3F));
    } else if (n + 3 < cap) {
        out[n++] = char(0xE0 | (c >> 12));
        out[n++] = char(0x80 | ((c >> 6) & 0x3F));
        out[n++] = char(0x80 | (c & 0x3F));
    }
}

// a string object -> UTF-8 in buf, with <COLOR_...> runs recorded as spans when asked for
void ReadWide(uint32_t str_obj, char* buf, int cap, hud::Span* spans = nullptr, int* span_count = nullptr) {
    buf[0] = '\0';
    if (span_count) *span_count = 0;
    if (!GuestPtr(str_obj)) return;
    const uint32_t len = kernel::R32(str_obj + 0x4u);
    const uint32_t text = kernel::R32(str_obj + 0x0u);
    if (!len || len > 4096u || !GuestPtr(text, len * 2u)) return;
    int n = 0;
    uint32_t colour = 0;
    int span_start = -1;
    for (uint32_t i = 0; i < len && n + 4 < cap; i++) {
        const uint32_t c = kernel::R16(text + i * 2u);
        if (c == '<') {
            char tag[32];
            uint32_t t = 0;
            for (i++; i < len && t + 1 < sizeof tag; i++) {
                const uint32_t tc = kernel::R16(text + i * 2u);
                if (tc == '>') break;
                tag[t++] = char(tc < 0x80 ? tc : '?');
            }
            tag[t] = '\0';
            if (std::strncmp(tag, "COLOR_", 6) != 0) continue;  // not a colour: drop the tag
            const uint32_t argb = uint32_t(std::strtoul(tag + 6, nullptr, 16));
            const uint32_t rgb = argb & 0xFFFFFFu;
            // close the run that was open, if any
            if (spans && span_count && span_start >= 0 && *span_count < 16) {
                spans[*span_count].start = uint16_t(span_start);
                spans[*span_count].length = uint16_t(n - span_start);
                spans[*span_count].colour = colour;
                (*span_count)++;
            }
            span_start = -1;
            // white is the body colour: treat it as "no span" rather than a run of default text
            colour = (rgb == 0xFFFFFFu) ? 0u : rgb;
            if (colour) span_start = n;
            continue;
        }
        if (c == '\r') continue;
        AppendUtf8(buf, n, cap, c);
    }
    if (spans && span_count && span_start >= 0 && *span_count < 16) {
        spans[*span_count].start = uint16_t(span_start);
        spans[*span_count].length = uint16_t(n - span_start);
        spans[*span_count].colour = colour;
        (*span_count)++;
    }
    buf[n] = '\0';
}

}  // namespace

namespace hud {

bool ReadChat(Chat& out) {
    const uint32_t h = kernel::R32(0x35A740u);
    if (!GuestPtr(h)) return false;
    const uint32_t state = kernel::R32(h + 0xE0u);
    const uint32_t d = kernel::R32(h + 0xD4u);
    if (!GuestPtr(d) || (state != 1u && state != 2u)) return false;
    ReadWide(kernel::R32(d + 0x8u), out.speaker, int(sizeof out.speaker));
    ReadWide(kernel::R32(d + 0x4u), out.body, int(sizeof out.body), out.spans, &out.span_count);
    return out.body[0] != '\0';
}

}  // namespace hud
