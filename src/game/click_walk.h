#pragma once

#include <cstdint>

// Clicks on far ground. A click walks by path finding, and both path finders (cPathAstar, cPathFloodfill) search a
// 64x64 tile grid around the walker (start - 32 .. start + 31 per tile axis): a target further out on either axis,
// or one whose way around a river or a wall leaves that grid, finds no path and the hero stays put. At 1024x768 the
// screen never reached that far; a large, zoomed-out view does (its corners lie ~50 tiles out along one tile axis).
// Walk orders within 30 tiles go to the game unchanged (its path finder knows the doors and stairs into houses);
// only when the hero does not move do they get a route like the ones further out. The hero's walk orders to targets
// more than 30 tiles out get a route planned over the loaded sectors (A* over the
// 5x5 of WideSectors with the game's own tile test, ENG 00636C10) and walk it in legs of up to 24 tiles, the next
// leg going out as the hero nears the end of the last; off the route or stuck, the route is planned again (three
// times at most). Without a route, the order is pulled back to 30 tiles straight at the target and, when a leg finds
// no way, legs turned aside follow. Another order from the player ends the walk.
namespace ClickWalk
{
    // Queues the hook in the caller's Patch transaction.
    void install();

    // Once per presented frame: while a far walk lasts, asks the window's thread now and then for the next leg.
    void onFrame();

    // The hero walks to world (x, y), however far (a click on the agent's map): as a click there, routed over the
    // loaded sectors and, beyond them, over the agent's map memory, stretch after stretch.
    // `byPlayer` false: the agent's own travel (to a quest), which does not hold the agent back as the player's does.
    void walkTo(int32_t x, int32_t y, bool byPlayer = true);
    // A walk asked for on the agent's map in progress (the auto attack leaves it be unless the hero is attacked).
    bool walking();
    // Any routed walk under way (the player's or the agent's), or one asked for and not yet sent.
    bool routing();

    // Whether a walker may stand on world tile (tx, ty) of `layer`, by the game's own tile test: 1 yes, 0 no, -1 not
    // known (the tile outside the loaded sectors, or no tile test in this build).
    int walkable(int layer, int tx, int ty);
}
