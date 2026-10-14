// Hooks for default.xbe. A strong definition replaces the recompiled function at that address.
#include "xdk/hle.h"

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
