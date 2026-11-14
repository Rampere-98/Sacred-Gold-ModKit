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
    // The agent's own travel stopped (it picks up or fights now; its next look sends it on again). A walk the player
    // asked for is left be.
    void cancel();
    // Any routed walk under way (the player's or the agent's), or one asked for and not yet sent.
    bool routing();

    // Whether a walker may stand on world tile (tx, ty) of `layer`, by the game's own tile test where its sector is
    // loaded, else as the world files have it (WorldTiles: surface only, doors counted walkable): 1 yes, 0 no, -1 not
    // known (another layer outside the loaded sectors, or no world files).
    int walkable(int layer, int tx, int ty);

    // What a tile is, read from the game's own data (docs/AGENT_ADDRESSES.md «Casilla» and «Casillas dependen de
    // objeto», Manolito): the tile's +0x1F type (low 4 bits 1 or 2: blocked; high 4 bits the ground class, 9 water,
    // 0xA lava), +0x1E flags (8 blocked, 4 decided by the tile's record), +0x1C/+0x1D the offset to a large
    // obstacle's anchor (a house, a wall); the record of a flag-4 tile ([0xAB44C8] tree -> [map+0x268] table, 16
    // bytes: +6 object id, +0xA bit 0 walkable) says a door or gate (an object) or a scripted barrier (none), open or
    // shut. So the agent knows a shut door from a wall, a house from a rock, water from a fence.
    enum class TileKind : uint8_t
    {
        Unknown = 0,
        Floor = 1,
        Water = 2,
        Lava = 3,
        Building = 4,
        Obstacle = 5,
        GateOpen = 6,
        GateShut = 7,
        BarrierOpen = 8,
        BarrierShut = 9,
        // A low obstacle (the tile's type, low nibble 2, with no water under it: a bush, a small rock, a lath fence;
        // Manolito, block AU, 2026-11-13, live): not walked on, but a projectile goes over and it is seen across.
        Low = 10,
    };
    // -1 when not known (the sector not loaded and no world file for it). Bit 0x10 over the kind: the tile's +0x1E
    // bit 2, the safe zone of a settlement (Manolito, static, 2026-11-13: 0x4D4DA0 shields players and friends in
    // it, FillRegion spawns no creatures there). Not «under a roof»: what hides a roof is not found yet. The name
    // kIndoors is from the first guess.
    constexpr int kIndoors = 0x10;
    // `liveOnly`: -1 where the tile's sector is not loaded, the world files not read (the map's scan on the
    // presenting thread: the files' lock is the route search's too, and the frame waited on it for seconds).
    int tileKind(int layer, int tx, int ty, bool liveOnly = false);
    // The tile's ground class (its +0x1F high 4 bits), -1 when its sector is not loaded: for the scan's log.
    int tileGround(int layer, int tx, int ty);
    inline bool kindWalkable(int kind)
    {
        kind &= 0xF;
        return kind == 1 || kind == 6 || kind == 8;
    }
}
