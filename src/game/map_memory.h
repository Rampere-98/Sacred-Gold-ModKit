#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
    // A route over what the map knows, tile by tile from (fromX, fromY) to (toX, toY) on `layer`: walked and
    // walkable tiles cheap, never seen ones dearer (they may be anything), unwalkable ones never. Empty and false
    // when there is none or the ends lie too far apart for one search (~1000 tiles).
    bool route(int layer, int fromX, int fromY, int toX, int toY, std::vector<std::pair<int, int>>& out);
    // A door seen (the player, 2026-11-11: «ten cuidado con las puertas, es importante que existan»; a script-held
    // door clicked again and again once kept the Daemon shut in 18 min): one that opens (or is open) is a way through
    // for routes, its tile and those around it; one held shut by a script or a key is a wall there, whatever the world
    // files say of its doorway.
    void noteDoor(int layer, int tileX, int tileY, bool opens);
    // The walk's length in tiles along a route (the player, 2026-11-11: «la distancia de la ruta entera», not the
    // straight line), worked out on a thread of its own and kept two minutes for ends within 4 tiles: -1 while it is
    // being worked out, -2 with no route (too far apart for one search, or none over what is known).
    float routeTiles(int layer, int fromX, int fromY, int toX, int toY);
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
    // Starts reading the map file on a thread of its own (once; install() calls it too).
    void preload();
    void install();

    // The menus' switches: the radar on the screen's side, the enemies seen and the quests on the map.
    bool& hudShown();
    bool& foesShown();
    bool& questsShown();
    void setHero(int layer, int32_t x, int32_t y);

    // The radar: what the agent sees now (world positions), and where it is heading (none: `route` false).
    // Enemies, the one aimed at, hirelings; loot on the ground, chests and the like, the map's usable fixed things
    // (magic hiding places, wells, gold).
    enum class Blip : uint8_t { Enemy, Target, Hireling, Item, Chest, Hiding };
    void beginRadar();
    void addBlip(Blip kind, int32_t x, int32_t y);
    void endRadar();
    void setRoute(bool route, int32_t x = 0, int32_t y = 0);

    // Quests on the map, as the game shows them over the heads: "?!" a quest to take, "!" one to report, "?" the others
    // with a quest icon; yellow when the game has it for the hero now, white when not yet. A ring: a quest's target
    // from the journal (yellow the one the agent goes for), at the rim when off the radar.
    enum class Glyph : uint8_t { Offer, Report, Other, Target };
    struct QuestMark
    {
        Glyph glyph;
        bool yellow;
        uint8_t layer;
        int32_t x, y;   // world
    };
    void setQuestMarks(std::vector<QuestMark> marks);

    // What the agent found, kept on its map with a symbol: a merchant (a shop opened there), a way into another layer
    // (door, stairs, cave: both ends), a portal (a jump on the same layer: both ends).
    enum class Mark : uint8_t { Merchant, Entrance, Portal, Count };
    void mark(Mark kind, int layer, int32_t x, int32_t y);
    // The ways in or out and portal ends found on `layer` within `radius` tiles of tile (tx, ty), as tiles: the ways
    // out of a shut-in area (Whereabouts) the agent can try.
    std::vector<std::pair<int, int>> waysOut(int layer, int tx, int ty, int radius);
    // An enemy of kind `type` (its name) met at world (x, y) on `layer`: the encyclopedia drawn on the map, a symbol per
    // kind and place (places of a kind a few squares apart count once), colored by how hard the kind hits, with
    // filters in the menu.
    void sighting(uint32_t type, const std::string& name, int layer, int32_t x, int32_t y);
    // A game loaded: the hero's next position is no jump.
    void forgetHero();

    // Writes the file if something changed (at most every `minMs`).
    void save(uint32_t now, uint32_t minMs);
}
