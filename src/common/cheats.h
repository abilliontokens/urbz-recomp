#pragma once

#include "core/input.h"

// The game's own console cheats, offered in the F1 panel (overlay.cpp). Each one is entered the way
// its pad code enters it (src/game/cheats.cpp), so what it does is the game's own: nothing here is a
// setting, nothing is saved, and nothing happens until the player clicks.
namespace cheats {

// Rows of the game's cheat-code table (sub_0004A300 writes it at 0x35A86E). Entering row n toggles
// bit n of the game's cheat mask, which the cheat gnome reads as SimAntics global 46. The Xbox
// button sequence of each row is beside it.
enum Code : int {
    kGnome = 0,          // "Get Shorty": Left Y Down A X - shows the cheat gnome
    kFillMotives = 2,    // R L Down Black Left B
    kSkillObjects = 3,   // L Black Right X Left
    kSocials = 5,        // Black Up Y Down R
    kMaxPhysical = 7,    // L R A Down Black
    kMaxMental = 8,      // L B A Black Down Y
    kMaxArtistic = 9,    // Y Down Black A B
    kPowerSocials = 10,  // Down Black Right X Left
};

// any thread: the player is in the city with the HUD up, where the game takes its codes
bool InCity();
// any thread: the credits screen is up
bool InCredits();

// any thread: the code is on - the game's own bit, with a click the title thread has not taken yet
// already counted, so a checkbox shows what was asked for
bool On(Code c);
// render thread: enter the code again (on <-> off) on the title thread's next frame, exactly as the
// pad sequence would; nothing is queued outside the city
void Toggle(Code c);

// render thread: on the credits screen, press Up Down A Up Down A on port 0, the sequence the credits
// movie answers with the development team's photo; false when the credits are not up
bool TeamPhoto();
// any thread: those presses are still going out
bool TeamPhotoRunning();

// app::GameInfo::pad (game_info.cpp): adds the team photo presses to port 0 while they run
void Pad(int port, input::Pad& pad);

}  // namespace cheats
