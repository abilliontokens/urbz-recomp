#pragma once

// the in-game overlay: F1 settings, F2 performance (overlay.cpp)
namespace overlay {

// app::GameInfo::ui: the render thread, each frame inside the ImGui frame, app::UiLock held
void Draw(double fps);

}  // namespace overlay
