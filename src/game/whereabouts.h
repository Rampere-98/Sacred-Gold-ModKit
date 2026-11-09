#pragma once

#include <cstdint>

// Where the hero is (the player's wish, 2026-11-09: the agent knows where it stands to know where to go and how).
// A flood fill from the hero's tile over the game's own tile test (ClickWalk::walkable) within kRadius tiles: the
// tiles the hero can walk to from here, and whether they are shut in (a closed yard, the Gladiator's arena behind
// its portcullis, a cellar) or reach out into open ground. A target outside a shut-in area is not travelled to; the
// agent goes to the reachable tile nearest it (by the gate, where a script may open the way) and waits there.
namespace Whereabouts
{
    constexpr int kRadius = 40;     // tiles around the hero the fill looks at

    struct Area
    {
        bool valid = false;
        int layer = 0;
        int heroX = 0, heroY = 0;   // tile the fill began on
        bool shutIn = false;        // no reachable tile touches the fill's edge or an unknown tile
        int tiles = 0;              // reachable tiles
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
    };

    // Fills again when the hero's tile or layer changed and at least a second has passed (or every 5 s anyway).
    void update(int layer, int tileX, int tileY);
    const Area& area();
    // Whether tile (x, y) was reached by the last fill: 1 yes, 0 no (inside the window, not reachable), -1 unknown.
    int reachable(int x, int y);
    // The reached tile nearest to (x, y); false when nothing was reached.
    bool nearestReachable(int x, int y, int& outX, int& outY);

    void drawMenu();
}
