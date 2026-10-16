#pragma once

#include <cstdint>

#include "core/input.h"

// keyboard and mouse as the console's pad on port 0 (input_kbm.cpp)
namespace kbm {

// app::GameInfo::message: the window thread, after ImGui. Reads key and mouse messages and never
// takes one, so ImGui and the host's own F3 still see them; always returns false.
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

// app::GameInfo::pad: any guest thread, each poll. ORs the keyboard and mouse into port 0 and
// reports that port connected, pad or no pad. Never clears what a real controller pressed.
void Apply(int port, input::Pad& pad);

}  // namespace kbm
