// Hooks for default.xbe. A strong definition replaces the recompiled function at that address.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include "cpu/x86_ops.h"
#include "xdk/hle.h"

// the frame step the engine is working with, seconds, published by sub_0018E360 below
namespace {
std::atomic<double> g_dt{1.0 / 60.0};
}  // namespace

// D3DDevice_InsertCallback(Flags, pCallback, Context) at 0x24E320.
//
// On the console this writes a callback token into the push buffer; the D3D interrupt handler runs
// pCallback(Context) once the GPU reaches it. The SDK has no push-buffer callback path - the tokens
// land in a scratch buffer that the renderer walks for draws only - so the callback never ran and
// the Renderer thread's command 5 (sub_001D4AE0) deadlocked the engine thread, which waits on the
// semaphore that callback releases (sub_001D2980 -> NtReleaseSemaphore).
//
// The host renderer has no GPU timeline the guest can be behind, so the callback is run immediately:
// by the time this returns, everything queued before it has been handed to the render thread.
HLE_STDCALL(D3DDevice_InsertCallback, 3) {
    uint32_t callback = a[1], context = a[2];
    if (callback != 0) ::kernel::CallGuest(ctx, callback, {context});
    return 0;
}

// The frame step, sub_0018E360(float elapsed_seconds), called once a frame from the frame tick
// sub_0018E480 at 0x18E4FF.
//
// The engine measures real time (the clock object at [0x358914] reads rdtsc through XAPI
// QueryPerformanceCounter over a fixed 733,333,333 Hz), but then quantises it:
//
//     ticks = clamp((int)(elapsed / [0x2F024C] + 0.5), 1, 15)     // 0x2F024C = 1/60 s
//     dt    = ticks * [0x2F024C]                                  // 0x18E580, into [0x2E2A80]
//
// [0x2E2A80] is the one delta the rest of the engine multiplies by - 97 reads in 63 functions -
// so with a 1/60 s tick and a floor of one tick, every frame shorter than 25 ms still advanced the
// game by a whole 1/60 s. Measured: at a 144 fps cap the attract loop, menus and city ran 2.4x
// fast (144 * 1/60 = 2.4 s of game time per real second).
//
// The tick is only the engine's own unit, so the host picks a finer one and writes back a
// seconds-per-tick that makes `ticks * [0x2F024C]` exactly the time that really passed. dt is then
// real seconds at any frame rate - 60, 144, 240 or uncapped - and the engine's own 0.25 s ceiling
// (15 ticks of 1/60 s) is kept, so a long stall still cannot advance the world in one jump.
//
// Nothing else depends on the unit: [0x2E2A7C] (the tick count) has one other reader,
// sub_001A7F80 at 0x1A7FAE, which forwards it only on the smoothing path the game never enables
// (clock+5 is cleared at 0x18E0E2 and again at 0x1B02EE), and the render thread's type 4 handler
// sub_001D44D0 discards that field anyway; the running total [0x358898] is copied to [0x3588BC]
// by the two pad polls and never read again.
X86_FUNC(sub_0018E360) {
    constexpr double kTick = 0.001;  // seconds per tick, 1/60 before this
    constexpr int kMaxTicks = 250;   // 0.25 s, the engine's own ceiling of 15 ticks of 1/60 s
    const float elapsed = X86_MEMF32(ctx.esp + 0x4u);
    // not (elapsed > 0): a NaN or a negative reading takes the smallest step, as the clamp did
    const double dt = elapsed > 0.0f ? std::min(double(elapsed), kMaxTicks * kTick) : kTick;
    const int ticks = std::clamp(int(dt / kTick + 0.5), 1, kMaxTicks);
    X86_MEMF32(0x2F024Cu) = float(dt / ticks);  // ticks * this == the time that really passed
    X86_MEM32(0x2E2A7Cu) = uint32_t(ticks);
    X86_MEM32(0x358898u) = X86_MEM32(0x358898u) + uint32_t(ticks);
    ctx.eax = uint32_t(ticks);
    g_dt.store(dt, std::memory_order_relaxed);
    ctx.esp += 8;  // ret 4
}

// Two subsystems step per call instead of per dt, so they sped up with the frame rate even once
// the frame step above was real: they are the only ones in the engine (every other consumer
// multiplies by the delta at [0x2E2A80]).

// The camera zoom, sub_0001FC20(camera in eax), called once a frame from the camera update
// sub_0001F4A0. It applies `distance(cam+0x450) -= rate * RY` with rate = [[cam+0x08]+0x54] = 1.3
// (0x1FCB2-0x1FCE7) and no frame-time factor at all, and its out-of-bounds dolly steps by the
// constant at [0x2E5DE8] = 0.1 per call. Both are per-frame rates meant for 60 fps, so the host
// scales them to the frame that really passed for the duration of the call and puts them back:
// zoom now moves at the same speed per second at 60, 144 or 240 fps.
X86_FUNC(__imp__sub_0001FC20);

// a rate that is not a plausible positive zoom speed, or a scale from a frame that was not a
// frame time: the original's own per-frame value is the right answer for both
bool rate_invalid(float rate, float scale) {
    return !(rate > 0.0f) || rate > 100.0f || !(scale > 0.0f) || scale > 16.0f;
}
X86_FUNC(sub_0001FC20) {
    const float scale = float(g_dt.load(std::memory_order_relaxed) * 60.0);
    const uint32_t params = X86_MEM32(ctx.eax + 0x8u);
    // [cam+8] is only a valid params block on the paths where the original reads the rate out of
    // it, so a null one or an implausible rate has to mean "run the original untouched", not a
    // write into low guest memory or over whatever lives at that offset.
    if (params < 0x10000u || rate_invalid(X86_MEMF32(params + 0x54u), scale)) {
        __imp__sub_0001FC20(ctx, base);
        return;
    }
    const float rate = X86_MEMF32(params + 0x54u);
    // only the rate: the out-of-bounds dolly at 0x1FE42 multiplies [0x2E5DE8] by the zoom delta
    // this scaling already produced, so scaling it too would make that move scale squared
    X86_MEMF32(params + 0x54u) = rate * scale;
    __imp__sub_0001FC20(ctx, base);
    X86_MEMF32(params + 0x54u) = rate;
}

// Button auto-repeat, sub_0018D6D0(pad in eax). It counts each held button's timer down by one
// per call (0x18D6D0 onwards, reloading from the pad's delay byte +0x1D8 = 10 and rate byte
// +0x1D9 = 3). The pad manager sub_001929B0 polls every pad twice per frame (its two vtbl+0x54
// loops, both through sub_00192170, whose only call to here is at 0x1921A5), so on the console
// that is 120 calls a second per pad: repeat after 10/120 s, then every 3/120 s. Tied to calls,
// a 144 fps game repeats 2.4x too fast on the one widget that reads the repeat word
// (sub_0012E840's only consumer, sub_0012F2D0 mode 4). The counts are rescaled by this machine's
// call rate, which is 2 per frame - 2/(120 * frame dt) - so the original logic runs on every
// call, bits and all, at console timing. (An earlier version of this wrapper smoothed the gaps
// between all 8 calls a frame, which clustered back-to-back and made it 4-6x too slow instead.)
X86_FUNC(__imp__sub_0018D6D0);
X86_FUNC(sub_0018D6D0) {
    const double calls_per_tick = 1.0 / (60.0 * g_dt.load(std::memory_order_relaxed));
    const uint32_t pad = ctx.eax;
    X86_MEM8(pad + 0x1D8u) = uint8_t(std::clamp(int(10.0 * calls_per_tick + 0.5), 1, 255));
    X86_MEM8(pad + 0x1D9u) = uint8_t(std::clamp(int(3.0 * calls_per_tick + 0.5), 1, 255));
    __imp__sub_0018D6D0(ctx, base);
}

// The menu held-repeat throttle, sub_0001D530(pad, direction), gated from the UI router
// sub_0001D630's held branch (calls at 0x1D630's 6390/6421/6452/6483 recompiled lines; the router
// posts the navigation event only when this returns 1). This is the repeat the player feels in
// every menu and on the Create-an-Urb name keyboard, which moves one cell per router event and
// has no timing of its own.
//
// It looks time-based but is not. It advances a global [0x378B60] by the honest clock delta at
// [0x358494], then adds `int([0x378B60] * 30)` - the *cumulative* tick count, clamped to 4 - into
// the direction's accumulator at [0x3584D8 + (pad*4+dir)*4], firing at the threshold in
// [0x3584B8 + ...] (22, then -3 per fire, floor 12). Adding a cumulative count once per call makes
// the rate quadratic in the call rate: simulated from the guest algorithm, about 8.8 repeats/s
// at 60 fps, 15.8/s at 144, 27.8/s at 240.
//
// So the host runs it at console cadence: per (pad, direction) the real frame delta is
// accumulated, and the original only runs once per 1/60 s, with [0x358494] - whose only reader
// in the image is this function - pinned to exactly 1/60 for that call and restored after. The
// guest logic then sees precisely what it saw on the console, at any frame rate. Skipped calls
// return 0 ("do not post") and pop the function's two stack arguments (ret 8).
X86_FUNC(__imp__sub_0001D530);
X86_FUNC(sub_0001D530) {
    static double pending[16] = {};  // one accumulator per (pad, direction)
    const uint32_t key = (X86_MEM32(ctx.esp + 0x4u) & 3u) * 4u + (X86_MEM32(ctx.esp + 0x8u) & 3u);
    pending[key] += g_dt.load(std::memory_order_relaxed);
    if (pending[key] < 1.0 / 60.0) {
        ctx.eax = 0;
        ctx.esp += 12;  // ret 8
        return;
    }
    pending[key] -= 1.0 / 60.0;
    const float delta = X86_MEMF32(0x358494u);
    X86_MEMF32(0x358494u) = float(1.0 / 60.0);
    __imp__sub_0001D530(ctx, base);
    X86_MEMF32(0x358494u) = delta;
}

// The idle auto-return, sub_0001F0E0(camera in ESI, one stack dword at [esp+4], ret 4). After two
// seconds without camera input (the countdown at cam+0x3C4, which already counts down by the frame
// delta) the camera eases back to its default distance. The easing *speed* at cam+0x3CC grows by
// [0x2E4D9C] = 0.05 per call at 0x1F295, and only then is the step dt-scaled (speed * [0x2E2A80] at
// 0x1F2B4), so the acceleration - not the velocity - was tied to the frame rate: the camera snapped
// back 2.4x harder at 144 fps. The same scaling is applied to the dead auto-yaw twin at [0x2E4D98]
// so it cannot come back wrong if its gate byte (byte_2E4D90) is ever set.
X86_FUNC(__imp__sub_0001F0E0);
X86_FUNC(sub_0001F0E0) {
    const float scale = float(g_dt.load(std::memory_order_relaxed) * 60.0);
    const float zoom = X86_MEMF32(0x2E4D9Cu), yaw = X86_MEMF32(0x2E4D98u);
    X86_MEMF32(0x2E4D9Cu) = zoom * scale;
    X86_MEMF32(0x2E4D98u) = yaw * scale;
    __imp__sub_0001F0E0(ctx, base);  // pops its own argument
    X86_MEMF32(0x2E4D9Cu) = zoom;
    X86_MEMF32(0x2E4D98u) = yaw;
}

// The script-binding executor, sub_00173300(group entry on the stack, ret 4), pumped for every
// binding group by sub_00173D80 from the gameplay tick. For an analog binding it reads the pad's
// deadzoned stick float - `[pad+0x200]` is the history block, +0x28/+0x2C are LX/LY and
// +0x30/+0x34 RX/RY - and hands it to the bound handler as a displacement (`call [edx+0x30]` at
// 0x173488/0x1734A4) with no frame-time factor, so a handler that integrates it directly moves
// proportionally faster the higher the frame rate. The blue selection cursor is one such handler.
//
// Scaling the sticks for *every* group was tried and is wrong: walking goes through the same
// executor and its handler already applies its own frame time, so the scale corrected it twice -
// measured 5.0 units/s at 60 fps against 0.51 at 255 fps. The scale therefore applies only to
// handlers named in XBE_BIND_SCALE (comma-separated guest addresses of the `vtable+0x30` target),
// which is also how a handler is identified in the first place: XBE_BIND_LOG=1 reports each
// distinct handler the executor dispatches to, once, with its group and vtable.
X86_FUNC(__imp__sub_00173300);
X86_FUNC(sub_00173300) {
    static const std::vector<uint32_t> scaled = [] {
        std::vector<uint32_t> v;
        const char* s = std::getenv("XBE_BIND_SCALE");
        for (const char* p = s; p && *p;) {
            char* end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 0);
            if (end == p) break;
            if (a) v.push_back(uint32_t(a));
            p = *end ? end + 1 : end;
        }
        if (!v.empty()) LOGR("input: scaling %zu bound handler(s) by frame time", v.size());
        return v;
    }();
    static const bool log_handlers = [] {
        const char* s = std::getenv("XBE_BIND_LOG");
        return s && s[0] && std::strcmp(s, "0") != 0;
    }();

    const uint32_t group = X86_MEM32(ctx.esp + 0x4u);
    const uint32_t obj = group ? X86_MEM32(group + 0x18u) : 0;
    const uint32_t vt = obj ? X86_MEM32(obj) : 0;
    const uint32_t fn = vt ? X86_MEM32(vt + 0x30u) : 0;
    if (log_handlers && fn) {
        static std::mutex m;
        static std::set<uint32_t> seen;
        std::lock_guard lock(m);
        if (seen.insert(fn).second)
            LOGR("input: bound handler %08X (group %08X, object %08X, vtable %08X)", fn, group, obj, vt);
    }
    const bool scale_this = fn && std::find(scaled.begin(), scaled.end(), fn) != scaled.end();
    if (!scale_this) {
        __imp__sub_00173300(ctx, base);  // pops its own argument
        return;
    }
    const float scale = float(g_dt.load(std::memory_order_relaxed) * 60.0);
    uint32_t hist[4] = {};
    float saved[4][4];
    for (int i = 0; i < 4; i++) {
        const uint32_t pad = X86_MEM32(0x2F027Cu + uint32_t(i) * 4u);
        hist[i] = pad ? X86_MEM32(pad + 0x200u) : 0;
        if (!hist[i]) continue;
        for (int a = 0; a < 4; a++) {
            saved[i][a] = X86_MEMF32(hist[i] + 0x28u + uint32_t(a) * 4u);
            X86_MEMF32(hist[i] + 0x28u + uint32_t(a) * 4u) = saved[i][a] * scale;
        }
    }
    __imp__sub_00173300(ctx, base);
    for (int i = 0; i < 4; i++) {
        if (!hist[i]) continue;
        for (int a = 0; a < 4; a++) X86_MEMF32(hist[i] + 0x28u + uint32_t(a) * 4u) = saved[i][a];
    }
}

// Shot-mode camera glide, sub_0001F320. It walks the camera position (cam+0x444/0x448/0x44C)
// toward its target by [0x2E4DA4] = 0.6 per call with no frame-time factor (0x1F426-0x1F469), so
// a scripted shot settled faster the higher the frame rate. Same treatment as the others.
X86_FUNC(__imp__sub_0001F320);
X86_FUNC(sub_0001F320) {
    const float scale = float(g_dt.load(std::memory_order_relaxed) * 60.0);
    const float step = X86_MEMF32(0x2E4DA4u);
    X86_MEMF32(0x2E4DA4u) = step * scale;
    __imp__sub_0001F320(ctx, base);
    X86_MEMF32(0x2E4DA4u) = step;
}
