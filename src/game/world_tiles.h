#pragma once

#include <cstdint>

// Whether a walker may stand on any surface tile of the world, read from the game's own files on disk
// (WORLD\sectors.keyx + sectors.wldx, see docs/AGENT_ADDRESSES.md "Casillas pisables (bloque P)"), not only the 5x5
// sectors loaded around the hero. Sectors are inflated on demand and kept in a small LRU cache.
//
// The files do not know doors, portcullises and the script's barriers (tile +0x1E bit 4, settled at run time by the
// object tree): those tiles count as walkable here, to be checked on arrival.
namespace WorldTiles
{
    // 1 walkable, 0 not (blocked, deep water, cliff, or outside the world's sector grid), -1 not known (a layer
    // other than the surface, or the files missing or unreadable). Thread-safe.
    int walkable(int layer, int tileX, int tileY);
    // The tile's ground (Manolito, block R2: the high nibble of tile +0x1F): 0xB stone road, 0xC gravel road, 7 a
    // bridge, 0xD town paving, 0xE wooden floor, 0xF gravel, 1 desert, 2 arctic, 3 forest, 4 grass, 5 swamp, 6 lava,
    // 8 dungeon, 9/0xA water. -1 not known or outside the world. Thread-safe.
    int ground(int layer, int tileX, int tileY);
    // A surface box at once (one lock): per tile -1 not known, 0 not walkable or outside, else bit 0 walkable and
    // bits 1-4 the ground.
    void fill(int tileX, int tileY, int w, int h, int8_t* out);
    // A road one walks on to go far: stone or gravel road, a bridge, a town's paving.
    inline bool road(int ground)
    {
        return ground == 0xB || ground == 0xC || ground == 7 || ground == 0xD;
    }
}
