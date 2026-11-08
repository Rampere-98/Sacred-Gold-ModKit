#pragma once

#include <cstdint>

// What the hero has seen of the world, for the auto attack's roaming: the map in cells of a few tiles, per layer (the
// surface, each dungeon and interior has its own), each with how often the hero was there and how often a walk into it
// got nowhere (water, walls), and the color of its ground. Roaming heads for the cells least seen that are not known to be blocked, so it covers
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
    // The ground's color is wanted for the cell at world (x, y) on `layer`, the hero standing there at screen (sx, sy)
    // (back buffer pixels): sampled after the next world render, around its feet. Only for cells still without one.
    void wantGround(int layer, int32_t x, int32_t y, float sx, float sy);
    // Right after the world is drawn (before the UI), on the render thread.
    void afterWorld();
    // Its ModKit menu section: the explored map around the hero, a square per cell in the ground's color, blocked
    // ones left clear, unexplored ones black (dark ground is lifted above black so it still reads as explored).
    void install();
    void setHero(int layer, int32_t x, int32_t y);

    // Writes the file if something changed (at most every `minMs`).
    void save(uint32_t now, uint32_t minMs);
}
