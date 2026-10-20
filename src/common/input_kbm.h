#pragma once

#include <cstdint>

#include "core/input.h"

// keyboard and mouse as the console's pad on port 0 (input_kbm.cpp)
namespace kbm {

#if defined(_WIN32)
// app::GameInfo::message: the window thread, after ImGui. Reads key and mouse messages and never
// takes one, so ImGui and the host's own F3 still see them; always returns false. The wheel
// arrives here as WM_MOUSEWHEEL.
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
#else
// mouse-wheel notches (positive zooms in, negative out), from whoever owns the host's event pump
// this frame (overlay::Draw forwards ImGui's wheel while the settings panel is closed). On Windows
// the wheel reaches Message as a window message instead, so this exists only on the SDL host.
void NoteWheel(int notches);
// the pointer (SDL window coordinates) and the right and middle buttons, sampled by the SDL host's
// event loop each iteration (sdk/runtime/app/main_sdl.cpp CacheMouseState): SDL_GetMouseState
// may only be called on that thread, so the sample is cached in atomics that Apply reads on the
// guest's polling threads.
void NoteMouse(float x, float y, bool right, bool middle);
#endif

// app::GameInfo::pad: any guest thread, each poll. ORs the keyboard and mouse into port 0 and
// reports that port connected, pad or no pad. Never clears what a real controller pressed.
void Apply(int port, input::Pad& pad);

}  // namespace kbm
