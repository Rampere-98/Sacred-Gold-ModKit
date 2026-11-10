#pragma once
#include <windows.h>

// The agent's games start on their own ([AutoStart], the player's wish, 2026-11-10: the tests run all the time, the
// game never left standing in its menus): with the game in its start menu for a while, a new campaign game is begun as
// the player would (Campaign, a class, Start, the films skipped), with the next class of the rotation each time.
// Clicks and keys go to the game's window (Controller::clickAt, Inject), so it works without the focus.
namespace AutoStart
{
    // Every presented frame (menus too).
    void onFrame();
}
