#pragma once

#include <atomic>
#include <cstdint>

// The modern HUD: the host draws what the game's own corner HUD shows, as vectors at the output's
// own resolution instead of a 640x480 bitmap stretched over it (hud_draw.cpp draws, hud_state.cpp
// reads the guest and keeps the game's own status clips off screen).
namespace hud {

// what the HUD shows, all of it already normalised; the game's own units stay in the reader
struct State {
    // 0 (empty, red) to 1 (full, green), in the order the game's HUD draws them
    float bladder = 0.0f, hygiene = 0.0f, hunger = 0.0f, fun = 0.0f, energy = 0.0f;
    float mood = 0.5f;      // 0 (miserable) to 1 (delighted)
    int hour = 0;           // 0 to 23
    int minute = 0;         // 0 to 59
    // the number the game's own box shows is HUD_rep, the reputation itself ([H+0x970+2p], read
    // by sub_142180) - not a level index, which is what an earlier reading of it assumed
    int rep = 0;
    float progress = 0.0f;  // 0 to 1 along the bar beside it: (rep - base) / (next unlock - base)
    // the action queue the game shows in the bottom right: how many actions are lined up and
    // which one the sim is doing now (-1 when nothing is running)
    int queued = 0;
    int queue_active = -1;
};

// the NPC dialogue currently on screen, if any; text is UTF-8, already stripped of the game's
// own colour markup, with the coloured words carried beside it as spans into `body`
struct Span {
    uint16_t start = 0, length = 0;
    uint32_t colour = 0;  // 0xRRGGBB as the game asked for it
};
struct Chat {
    char speaker[64] = {};
    char body[512] = {};
    Span spans[16];
    int span_count = 0;
};

// the title thread's values for this frame; false until the city is up, and false whenever the game
// is not showing its own HUD - cutscenes, the pause menu, the XAM, Create-an-Urb - so the modern HUD
// comes and goes with the original (hud_state.cpp, HudUp)
bool Read(State& out);
// false when no dialogue box is up this frame
bool ReadChat(Chat& out);

// the setting, kept in settings.toml as modern_hud and offered in the F1 panel; on by default
extern bool g_enabled;

// inside the ImGui frame, under app::UiLock: draws the two corner groups at the output's scale
void Draw(const State& s);

// the modern dialogue panel, drawn where the game's own box sits
void DrawChat(const Chat& c);

}  // namespace hud
