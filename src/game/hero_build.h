#pragma once

#include <windows.h>

// The hero's build, as a player grows it ([Build], ModKit menu "Progreso del héroe"): attribute points spent by the
// class's plan, skill points on its skills (the ones it leans on first, none above the hero's level), new skill slots
// filled from what the class offers. All through the game's own functions (the character window's "+" buttons), so
// the stats and their anti-cheat copies stay as the game keeps them. Addresses: docs/AGENT_ADDRESSES.md, "Build".
namespace HeroBuild
{
    // Every frame of the auto attack while the hero lives; `calm`: no fight on.
    void onFrame(DWORD now, bool calm);
}
