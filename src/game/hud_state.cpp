// The modern HUD's two halves that only make sense for this image: reading what the game's own HUD
// shows out of guest memory, and keeping the game from drawing the parts the host replaces.
//
// Values. The HUD's cache object ("HUDData", 0x978 bytes) is *[0x35A740]. The game deletes it on
// Create-an-Urb, quit-to-menu and HUD_transition (sub_AC080, sub_AC270, sub_992E0, sub_992A0) and
// zeroes that pointer straight after, so the host reads the pointer afresh every time and keeps no
// copy of it:
//
//   H = *[0x35A740], player p = [H+0xF9] & 1
//   H + 0xA8 + 6p + 0..4  five motive percents, 0..100, in the order the HUD draws them
//                         (bladder, hygiene, hunger, fun, energy - Sim attributes 9, 8, 7, 15, 5)
//   H + 0xA8 + 6p + 5     mood percent, 0..100, a weighted average of the five
//   H + 0x96C/0x970/0x974 + 2p   reputation bar base, current and next unlock, int16 (see Read)
//   H + 0xE5, H + 0xE6    whether the HUD movie exists and what its show/hide is doing (HudUp)
//   H + 0xFC              player one's action queue controller, built by sub_12FF00(H, H + 0xFC)
//   G = *[0x2EDF60]; int16 [G+0x14] hour 0..23, int16 [G+0x1E] minute 0..59
//
// The percent bytes are deliberate: two writers use the raw int16 twin at H+0xB4 with different
// scales (one stores the -100..100 motive, the other (value+100)/2), while the percent byte is
// always 0..100 and is exactly what the movie is handed.
//
// The motive and reputation fields are caches the movie pulls. The HUD_bar{i} getter refreshes its
// byte (sub_142430), and the HUD_rep and HUD_next_rep_unlock getters theirs (sub_142180,
// sub_142200), only when the movie asks - and what asks is mc_meterClockPoll, a nine-frame loop in
// the HUD whose frames call updateClock, updateMoney, updateRep and updateBar0..5 in turn. The
// values are exactly as fresh as that clip is alive, which is one more reason the walk hook at the
// bottom of this file must never stop it.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "common/hud.h"
#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

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

uint8_t R8(uint32_t va) { return *static_cast<const uint8_t*>(kernel::HostPtr(va)); }

float Percent(uint32_t va) { return std::clamp(float(R8(va)) / 100.0f, 0.0f, 1.0f); }

constexpr uint32_t kHudDataPtr = 0x35A740u;
constexpr uint32_t kHudDataSize = 0x978u;

// HUDData as the game holds it right now, or 0 when there is none
uint32_t LiveHudData() {
    const uint32_t h = kernel::R32(kHudDataPtr);
    return GuestPtr(h, kHudDataSize) ? h : 0u;
}

// Whether the game is showing its own HUD. The movie runs that as a small state machine and
// reports every step through the HUD variable setter sub_141170, which keeps it in HUDData:
//   [H+0xE5]  1 from HUD_initialized to HUD_destroy; 0 while the HUD movie is gone, which it is
//             behind the pause menu, the XAM and Create-an-Urb
//   [H+0xE6]  HUD_state as tsc3_ingame's showHud/hideHud set it: 0 hidden, 3 showing, 2 shown,
//             1 hiding
// The game's own HUD controller (sub_1429C0) and its hideHud (sub_142520) read the same two bytes.
// "Showing or shown" puts the modern HUD up as the old one starts coming in, and takes it down as
// the old one starts going out for a cutscene, the pause menu or the phone.
bool HudUp(uint32_t h) {
    const uint8_t state = R8(h + 0xE6u);
    return R8(h + 0xE5u) != 0 && (state == 2 || state == 3);
}

// The HUDData the movie last read through the game's own variable getter (sub_1416C0, `this` in
// ecx). In every logged run it has been the object *[0x35A740] holds; the motive refresh writes
// only when the two agree, which they do not between a delete and the movie's first read of the
// object that replaces it.
std::atomic<uint32_t> g_hud_data{0};

}  // namespace

namespace hud {

bool Read(State& out) {
    const uint32_t h = LiveHudData();
    if (!h || !HudUp(h)) return false;  // no city yet, or the game's own HUD is not up
    LOG_FIRST(3, "hud: read, HUDData %08X, globals %08X", h, kernel::R32(0x2EDF60u));
    const uint32_t p = R8(h + 0xF9u) & 1u;
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

    // Reputation, as the movie's own meter (mc_rep, inside mc_meters) shows it:
    //   +0x970  HUD_rep, the sim's reputation, copied out of the sim by sub_142180 each time the
    //           movie asks; it also lowers +0x96C to it if it fell below
    //   +0x96C  HUD_base_rep, the highest unlock threshold already reached (sub_142200)
    //   +0x974  HUD_next_rep_unlock, the lowest one not yet reached, or 999 when none is left
    //           (sub_142200; the meter asks when it starts and again on every level-up)
    // The meter's calculate_target_frame shows frame 2 + round((cur - base) * 104 / (next - base)),
    // clamped to its 2..106, so this fraction clamped to 0..1 is the same bar. Once sub_142200 has
    // run, base <= cur < next; before it ever has, all three are zero and the bar is empty, as the
    // meter's own is. A level-up writes the three in turn, so a frame can see a torn set: clamping
    // covers that too.
    const int16_t base = int16_t(kernel::R16(h + 0x96Cu + 2u * p));
    const int16_t cur = int16_t(kernel::R16(h + 0x970u + 2u * p));
    const int16_t next = int16_t(kernel::R16(h + 0x974u + 2u * p));
    out.progress = next > base ? std::clamp(float(cur - base) / float(next - base), 0.0f, 1.0f) : 0.0f;
    out.rep = std::clamp(int(cur), 0, 9999);

    const uint32_t g = kernel::R32(0x2EDF60u);
    if (GuestPtr(g, 0x20)) {
        out.hour = std::clamp(int(int16_t(kernel::R16(g + 0x14u))), 0, 23);
        out.minute = std::clamp(int(int16_t(kernel::R16(g + 0x1Eu))), 0, 59);
    }

    // The action queue, for the strip that replaces the game's bottom-right panel. Player one's
    // controller is part of HUDData itself: an array of queued actions at +168 with a 20 byte
    // stride, how many of them at +336 and which one is running at +777. The slots are movie clips
    // whose pictures the game renders, which is why the modern strip redraws the queue rather than
    // restyling it.
    const uint32_t q = h + 0xFCu;
    const uint32_t count = kernel::R32(q + 336u);
    out.queued = count <= 9u ? int(count) : 0;
    const int active = int(R8(q + 777u));
    out.queue_active = active < out.queued ? active : -1;
    return true;
}

}  // namespace hud

X86_FUNC(__imp__sub_00229760);
X86_FUNC(__imp__sub_00142430);
X86_FUNC(__imp__sub_001416C0);

// sub_001416C0(this = HUDData in ecx, name): the movie's variable getter, and the one place that
// says which HUDData the movie is reading from.
X86_FUNC(sub_001416C0) {
    const uint32_t object = ctx.ecx;
    if (GuestPtr(object, kHudDataSize) && g_hud_data.load(std::memory_order_relaxed) != object) {
        g_hud_data.store(object, std::memory_order_relaxed);
        LOGI("hud: hud object %08X (global holds %08X)", object, kernel::R32(kHudDataPtr));
    }
    __imp__sub_001416C0(ctx, base);
}

namespace {

// The motive percentages are a cache filled by sub_142430: it asks the sim for one motive through
// its virtual table and writes the byte at +0xA8 + 6*player + bar. The movie's poll clip has the
// HUD_bar getter make that call for one bar at a time, a movie frame apart; the host makes all six
// every frame so the modern bars move as smoothly as the sim does. The bar index goes in ebx and the
// object and player on the stack the way the function is written; registers and the stack are put
// back, so the caller sees nothing.
//
// It writes into HUDData, so it runs only while that object is certainly alive and in use: the
// game's own pointer still names it, the movie is reading from that same object, and the HUD is up.
// Between a delete (Create-an-Urb, quit-to-menu, HUD_transition) and the movie's first read of the
// replacement the first two disagree, and an old copy of the pointer would have this writing six
// bytes and six int16 into freed memory every frame.
void RefreshMotives(X86Context& ctx, uint8_t* base) {
    const uint32_t object = LiveHudData();
    const uint32_t captured = g_hud_data.load(std::memory_order_relaxed);
    if (object != captured) {
        static uint32_t told_object = ~0u, told_captured = ~0u;  // title thread only
        if (object != told_object || captured != told_captured) {
            told_object = object;
            told_captured = captured;
            LOG_FIRST(64, "hud: motive refresh skipped, movie reads %08X, game holds %08X", captured,
                      kernel::R32(kHudDataPtr));
        }
        return;
    }
    if (!object || !HudUp(object)) return;
    const uint32_t player = R8(object + 0xF9u) & 1u;
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

// Keeping the game from drawing its own status clips - and why doing it wrong killed the pause menu.
//
// Every display object carries a bit 0x8000 in [obj+0x04] that the walk below (sub_229760) tests
// before drawing a child, and clearing it does hide the clip and its whole subtree. But it is not
// _visible - that is the float at [[obj+0x48]+0x2C], property 8 of the getter sub_218120, and the
// renderer tests it separately. It is the display list's "live" bit: PlaceObject (sub_22A220) sets
// it, removal (sub_22AF50) clears it, and the rest of the runtime takes an object without it for
// one that has been removed:
//   - the per-frame advance (sub_2298C0 -> sub_22B8B0) skips it: its timeline, its clip events and
//     everything under it stop;
//   - code queued to run on it is dropped (sub_20FE40 and sub_208A00 test the bit first);
//   - method calls on it do nothing (sub_206620 tests the bit before it looks the method up).
//
// The HUD's show and hide are carried by exactly those clips (tsc3_ingame.apt, read out of
// DATA/flashes.arc). ingame.init leaves inputHandler on onIngameInputIgnore; hud.showMeters plays
// mc_meters (mc_meters_job in job mode) from "show", and only that segment's last frame (17/18:
// stop(); _parent.onMeterShow()) reaches ingame.onHudShowComplete, the one place that switches
// inputHandler to onIngameInput and sets HUD_state 2. Hiding mirrors it: onIngameInput's PAUSE
// branch sets HUD_pause and calls hideHud("launchPauseMenu"), and launchPauseMenu (destroyHUD,
// then the "Pause" flow state) runs from the end of mc_meters' "hide" segment (31/58: stop();
// _parent.onMeterHide()). An earlier version of this hook cleared the bit for good from the
// moment the HUD was built, before it had shown: mc_meters froze, the show never completed, Start
// and the phone button stayed on onIngameInputIgnore - nothing opened and nothing paused - and
// mc_meterClockPoll froze with it, which stopped the motive and reputation caches refreshing.
//
// So the bit is cleared for the walk alone and put back the moment it returns. Script, timelines,
// input and teardown all see the clips exactly as the game left them, and switching the modern HUD
// off in the F1 panel gives the old one back on the next frame with nothing to repair. The clips
// are found afresh at the start of every walk, by name, from the root list the walk itself is
// given, because the game throws the whole HUD movie away on the way into the pause menu or the
// phone (launchPauseMenu and launchPhone both call destroyHUD) and builds a new one on the way
// back: a pointer kept from an earlier HUD points into freed memory that is soon reused.
constexpr uint32_t kLiveBit = 0x8000u;
constexpr uint32_t kMaxSiblings = 512;  // bounds every walk along a sibling chain

bool Live(uint32_t obj) { return (kernel::R32(obj + 0x4u) & kLiveBit) != 0; }

// The first child in a display list, reached the way the game's own child lookup sub_1FA850 reaches
// it: the list holder points at the list, the list's first dword is its head object, the first child
// is [head+0x58], and siblings chain through [child+0x58]. Every hop is checked because the movie can
// be halfway through being built or torn down when the host looks.
uint32_t FirstInList(uint32_t holder) {
    if (!GuestPtr(holder, 4)) return 0;
    const uint32_t list = kernel::R32(holder);
    if (!GuestPtr(list, 4)) return 0;
    const uint32_t head = kernel::R32(list);
    if (!GuestPtr(head, 0x5C)) return 0;
    return kernel::R32(head + 0x58u);
}

// only a live sprite (type 0x0D) or movie (0x12) has children; its list holder is [obj+0x50]+0x24
uint32_t FirstChild(uint32_t obj) {
    if (!GuestPtr(obj, 0x5C)) return 0;
    const uint32_t flags = kernel::R32(obj + 0x4u);
    const uint32_t type = flags & 0x3Fu;
    if (!(flags & kLiveBit) || (type != 0x0Du && type != 0x12u)) return 0;
    const uint32_t data = kernel::R32(obj + 0x50u);
    return GuestPtr(data, 0x28) ? FirstInList(data + 0x24u) : 0u;
}

// A child's instance name, as sub_1FA850 compares it: [obj+0x0C] is a string object whose
// NUL-terminated characters start at +0x08.
bool ChildName(uint32_t obj, char (&buf)[24]) {
    const uint32_t s = kernel::R32(obj + 0x0Cu);
    if (!GuestPtr(s, 8u + sizeof buf)) return false;
    const char* text = static_cast<const char*>(kernel::HostPtr(s + 8u));
    for (size_t i = 0; i < sizeof buf; i++)
        if ((buf[i] = text[i]) == '\0') return true;
    return false;  // longer than any name looked for here
}

// parent's live child called `name`, or 0
uint32_t Child(uint32_t parent, const char* name) {
    char buf[24];
    uint32_t c = FirstChild(parent);
    for (uint32_t i = 0; i < kMaxSiblings && GuestPtr(c, 0x5C); i++, c = kernel::R32(c + 0x58u))
        if (Live(c) && ChildName(c, buf) && !std::strcmp(buf, name)) return c;
    return 0;
}

// The clips the walk is not to draw this frame, and the record of what to put back afterwards.
struct Hidden {
    static constexpr int kCapacity = 64;
    uint32_t clip[kCapacity];
    int count = 0;

    void Add(uint32_t c) {
        const uint32_t flags = kernel::R32(c + 0x4u);
        if (!(flags & kLiveBit) || count == kCapacity) return;
        kernel::W32(c + 0x4u, flags & ~kLiveBit);
        clip[count++] = c;
    }
    void Restore() const {
        for (int i = 0; i < count; i++) kernel::W32(clip[i] + 0x4u, kernel::R32(clip[i] + 0x4u) | kLiveBit);
    }
};

// hud0's own status children: exactly what the HUD symbol (character 162 of tsc3_ingame.apt) places
// on its third frame. The reputation meter, mood face and clock (mc_rep, mc_mood, mc_clockText, ...)
// live inside mc_meters and mc_meters_job and go with them. mc_actionProto, the pie menu and queue
// the script adds beside them, stays: it is interaction, not status.
const char* const kStatusClips[] = {
    "mc_meters",    "mc_meters_job", "mc_meterbar0", "mc_meterbar1",      "mc_meterbar2",
    "mc_meterbar3", "mc_meterbar4",  "mc_simoleons", "mc_meterClockPoll", "mc_ringtone",
};

bool StatusClip(const char* name) {
    for (const char* n : kStatusClips)
        if (!std::strcmp(name, n)) return true;
    return false;
}

// The action queue's slot plates "aq0".."aq8" and arrows "aqa0".."aqa8" (sub_12FF00 binds them). The
// pictures "aqi%d" stay: the game renders what the sim is about to do into them, the host cannot,
// and the modern chips are drawn around them.
bool QueuePlate(const char* n) {
    if (n[0] != 'a' || n[1] != 'q') return false;
    const char* digit = n[2] == 'a' ? n + 3 : n + 2;
    return digit[0] >= '0' && digit[0] <= '9' && digit[1] == '\0';
}

void HideHud(uint32_t hud, Hidden& out) {
    char name[24];
    uint32_t proto = 0;
    uint32_t c = FirstChild(hud);
    for (uint32_t i = 0; i < kMaxSiblings && GuestPtr(c, 0x5C); i++, c = kernel::R32(c + 0x58u)) {
        if (!Live(c) || !ChildName(c, name)) continue;
        if (StatusClip(name)) out.Add(c);
        else if (!std::strcmp(name, "mc_actionProto")) proto = c;
    }
    c = FirstChild(Child(proto, "mc_actionQueue"));
    for (uint32_t i = 0; i < kMaxSiblings && GuestPtr(c, 0x5C); i++, c = kernel::R32(c + 0x58u))
        if (Live(c) && ChildName(c, name) && QueuePlate(name)) out.Add(c);
}

struct HudMovie {
    uint32_t ingame = 0, hud0 = 0, hud1 = 0;
};

// From the list the outermost walk is given - the root's (dword_2E6140 + 4652, from sub_1F6100) -
// to the clips: the root movie holds "ingame", the HUD movie, which holds hud0 (and hud1 in a
// two-player game) and the two NPC dialogue boxes the modern chat card replaces.
HudMovie HideLegacyHud(uint32_t walk_list, Hidden& out) {
    HudMovie found;
    uint32_t top = FirstInList(walk_list);
    for (uint32_t i = 0; i < kMaxSiblings && !found.ingame && GuestPtr(top, 0x5C); i++) {
        found.ingame = Child(top, "ingame");
        top = kernel::R32(top + 0x58u);
    }
    char name[24];
    uint32_t c = FirstChild(found.ingame);
    for (uint32_t i = 0; i < kMaxSiblings && GuestPtr(c, 0x5C); i++, c = kernel::R32(c + 0x58u)) {
        if (!Live(c) || !ChildName(c, name)) continue;
        if (!std::strcmp(name, "hud0")) found.hud0 = c;
        else if (!std::strcmp(name, "hud1")) found.hud1 = c;
        else if (!std::strcmp(name, "mc_npcDialog") || !std::strcmp(name, "mc_npc2Dialog")) out.Add(c);
    }
    if (found.hud0) HideHud(found.hud0, out);
    if (found.hud1) HideHud(found.hud1, out);
    return found;
}

// One line whenever the HUD movie the walk finds changes - first seen, gone behind the pause menu,
// rebuilt at new addresses - with how many clips it keeps off screen.
void NoteHud(const HudMovie& now, int hidden) {
    static HudMovie seen;  // title thread only
    static int seen_hidden = -1;
    if (now.ingame == seen.ingame && now.hud0 == seen.hud0 && now.hud1 == seen.hud1 && hidden == seen_hidden)
        return;
    LOG_FIRST(256, "hud: legacy HUD now ingame %08X hud0 %08X hud1 %08X (was %08X %08X %08X), %d clips hidden "
              "while drawing", now.ingame, now.hud0, now.hud1, seen.ingame, seen.hud0, seen.hud1, hidden);
    seen = now;
    seen_hidden = hidden;
}

}  // namespace

// The Apt display-list walk: once a frame from sub_1F6100 for the root's list, and again for every
// clip's children through sub_2153B0, so only the outermost call does anything here.
X86_FUNC(sub_00229760) {
    LOG_FIRST(2, "hud: display list walk hook reached");
    // Each half can be switched off for a run, the way the frame-rate wrappers can, so a defect
    // can be bisected against stock behaviour without a rebuild: URBZ_NO_HUDHIDE leaves the
    // legacy clips drawing, URBZ_NO_HUDREFRESH stops the host pulling the motive values.
    static const bool no_hide = [] {
        const char* v = std::getenv("URBZ_NO_HUDHIDE");
        return v && v[0] && std::strcmp(v, "0") != 0;
    }();
    static const bool no_refresh = [] {
        const char* v = std::getenv("URBZ_NO_HUDREFRESH");
        return v && v[0] && std::strcmp(v, "0") != 0;
    }();
    static thread_local int depth = 0;
    if (depth > 0 || !hud::g_enabled) {
        ++depth;
        __imp__sub_00229760(ctx, base);
        --depth;
        return;
    }
    Hidden hidden;
    if (!no_hide) {
        const HudMovie found = HideLegacyHud(ctx.ecx, hidden);
        NoteHud(found, hidden.count);
    }
    if (!no_refresh) RefreshMotives(ctx, base);
    ++depth;
    __imp__sub_00229760(ctx, base);
    --depth;
    hidden.Restore();
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
    if (!GuestPtr(str_obj, 8)) return;
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
    const uint32_t h = LiveHudData();
    if (!h) return false;
    const uint32_t state = kernel::R32(h + 0xE0u);
    const uint32_t d = kernel::R32(h + 0xD4u);
    if (!GuestPtr(d, 0x10) || (state != 1u && state != 2u)) return false;
    ReadWide(kernel::R32(d + 0x8u), out.speaker, int(sizeof out.speaker));
    ReadWide(kernel::R32(d + 0x4u), out.body, int(sizeof out.body), out.spans, &out.span_count);
    return out.body[0] != '\0';
}

}  // namespace hud
