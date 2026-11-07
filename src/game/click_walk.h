#pragma once

// Clicks on far ground. A click walks by path finding, and both path finders (cPathAstar, cPathFloodfill) search a
// 64x64 tile grid around the walker (start - 32 .. start + 31 per tile axis): a target further out on either axis,
// or one whose way around a river or a wall leaves that grid, finds no path and the hero stays put. At 1024x768 the
// screen never reached that far; a large, zoomed-out view does (its corners lie ~50 tiles out along one tile axis).
// The hero's walk orders to targets more than 12 tiles out get a route planned over the loaded sectors (A* over the
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
}
