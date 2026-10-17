// Hooks for default.xbe. A strong definition replaces the recompiled function at that address.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

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
X86_FUNC(sub_0001FC20) {
    const float scale = float(g_dt.load(std::memory_order_relaxed) * 60.0);
    const uint32_t params = X86_MEM32(ctx.eax + 0x8u);
    const float rate = X86_MEMF32(params + 0x54u), dolly = X86_MEMF32(0x2E5DE8u);
    X86_MEMF32(params + 0x54u) = rate * scale;
    X86_MEMF32(0x2E5DE8u) = dolly * scale;
    __imp__sub_0001FC20(ctx, base);
    X86_MEMF32(params + 0x54u) = rate;
    X86_MEMF32(0x2E5DE8u) = dolly;
}

// Button auto-repeat, sub_0018D6D0(pad in eax). It counts each held button's timer down by one
// per call (0x18D6D0 onwards, reloading from the pad's delay byte +0x1D8 = 10 and rate byte
// +0x1D9 = 3), and the pad manager calls it twice a frame, so on the console a menu repeated
// after 10/120 s and then every 3/120 s. Tied to calls, a 144 fps game scrolls lists 2.4x too
// fast. The counts are rescaled to this machine's call rate - measured here rather than assumed,
// so a change in how often the manager polls cannot silently halve it - which keeps the original
// logic running on every call, bits and all, at console timing.
X86_FUNC(__imp__sub_0018D6D0);
X86_FUNC(sub_0018D6D0) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    static double interval = 1.0 / 120.0;  // seconds between calls, smoothed
    const clock::time_point now = clock::now();
    const double gap = std::chrono::duration<double>(now - last).count();
    last = now;
    if (gap > 0.0 && gap < 0.25) interval = interval * 0.9 + gap * 0.1;
    const double calls_per_tick = 1.0 / (120.0 * interval);  // 1.0 at the console's 120 calls a second
    const uint32_t pad = ctx.eax;
    X86_MEM8(pad + 0x1D8u) = uint8_t(std::clamp(int(10.0 * calls_per_tick + 0.5), 1, 255));
    X86_MEM8(pad + 0x1D9u) = uint8_t(std::clamp(int(3.0 * calls_per_tick + 0.5), 1, 255));
    __imp__sub_0018D6D0(ctx, base);
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
