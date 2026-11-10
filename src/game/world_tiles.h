#pragma once

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
}
