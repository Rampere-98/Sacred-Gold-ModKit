#pragma once

#include <cstdint>

// What the hero has seen of the world, for the auto attack's roaming: the map in cells of a few tiles, per layer (the
// surface, each dungeon and interior has its own), each with how often the hero was there and how often a walk into it
// got nowhere (water, walls). Roaming heads for the cells least seen that are not known to be blocked, so it covers
// new ground instead of wandering at random. Kept in SacredBild-map.txt beside the game.
namespace MapMemory
{
    // The hero at world position (x, y) on `layer` (cObject +0x1C/+0x20, the creature's +0x24).
    void visit(int layer, int32_t x, int32_t y);
    // A walk toward (x, y) got the hero no further.
    void blocked(int layer, int32_t x, int32_t y);
    // How little worth going to: the more seen and blocked, the higher.
    float cost(int layer, int32_t x, int32_t y);
    // Cells known, and of them on `layer`.
    int cells();
    int cellsOn(int layer);
    // Writes the file if something changed (at most every `minMs`).
    void save(uint32_t now, uint32_t minMs);
}
