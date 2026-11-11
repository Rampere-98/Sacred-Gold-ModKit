#include "game/click_walk.h"
#include "game/agent_cursor.h"
#include "game/agent_save.h"
#include "config/auto_attack.h"
#include "config/debug.h"
#include "game/frame_hooks.h"
#include "game/map_memory.h"
#include "game/wide_sectors.h"
#include "game/world_tiles.h"
#include "input/inject.h"
#include "log.h"
#include "patch.h"
#include "sacred/engine.h"
#include "sacred/object.h"
#include "sacred/world.h"
#include "sig.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <queue>
#include <vector>

namespace
{
    using namespace Sacred;

    decltype(Addr::cEngine_sendOrder)::Ptr g_origSendOrder = nullptr;

    constexpr double kTileSize = 53.66563;      // world units per tile (the world -> tile conversion, ENG 006224E0)
    constexpr double kMaxTiles = 30.0;          // inside the path finders' +-32 tile grid, a margin for rounding
    constexpr double kPlanTiles = 12.0;         // clicks further out than this get a route over the loaded sectors
    constexpr double kArrivedTiles = 1.5;       // close enough to the clicked spot
    constexpr double kMovedTiles = 2.0;         // a leg that moved the hero less than this found no way
    constexpr int kMaxStaleLegs = 10;           // legs in a row that got no closer than the best so far: give up
    constexpr DWORD kCheckMs = 100;             // how often a far walk looks at the hero
    constexpr DWORD kSettleMs = 1000;           // a new leg's grace (path finding, starting to walk)
    constexpr DWORD kStillMs = 500;             // the hero's position steps a tile at a time: still this long = stopped
    constexpr DWORD kMinLegMs = 400;            // a route leg runs at least this long before the next one goes out

    // Without a route (no loaded sectors to plan over, or none found): when the leg straight at the goal finds no
    // way (its end in water, behind a wall, or no path inside the path finder's grid), legs turned aside and shorter
    // ones: around a river bend or a wall end. Degrees, tiles.
    struct Try
    {
        double degrees, tiles;
    };
    constexpr Try kTries[] = {{0, kMaxTiles}, {45, 22}, {-45, 22}, {90, 16}, {-90, 16}, {0, 14}, {135, 10},
        {-135, 10}, {0, 7}};
    constexpr int kTryCount = static_cast<int>(sizeof(kTries) / sizeof(kTries[0]));

    // ---- Routes over the loaded sectors ----
    // The game's path finders only search 64x64 tiles around the walker. A route is planned here instead (A* over
    // the 5x5 loaded sectors, 320x320 tiles, with the game's own tile test) and walked in legs the game's path
    // finder can do: each leg ends on the route, at most kLegTiles from the hero.

    // A map position as the game's tile functions take it (ENG 006224B0 fills one): the sector id is a cache the
    // lookup fills in (0: look it up from the tile), x and y are world tiles, the layer 0 as for walk targets.
    struct TilePos
    {
        uint16_t sector;
        uint16_t _02;
        int32_t x, y;
        uint8_t layer;
        uint8_t _0d[3];
    };
    static_assert(sizeof(TilePos) == 0x10);

    // cMapData thiscall (position, 0) -> whether a walker may stand on the tile: its blocking flags (+0x1E bit 8,
    // +0x1F low nibble 1 or 2), overridden by objects on it such as doors (ENG 00636C10, the flood fill path
    // finder's cell test).
    using WalkableFn = uint8_t(__fastcall*)(cMapData* map, void* edx, TilePos* pos, uint32_t zero);
    uintptr_t s_walkable = 0;
    WalkableFn g_walkable = nullptr;
    const Sig::Entry kSigs[] = {
        {"click walk: tile walkable", "8B 44 24 08 53 55 56 8B 74 24 10 57", 0, Sig::Take::Match, &s_walkable},
    };

    constexpr int kSide = WideSectors::kTiles;
    constexpr int kCells = kSide * kSide;
    constexpr int kLegTiles = 24;               // route legs, well inside the path finders' grid
    constexpr int kMinLegTiles = 6;             // a stuck leg is tried again half as long, down to this
    constexpr int kAheadTiles = 6;              // the next route leg goes out this close to the current one's end
    constexpr int kOffRouteTiles = 6;           // further from the route than this: plan again
    constexpr int kMaxReplans = 8;

    struct Cell
    {
        int x, y;
    };

    int chebyshev(const Cell& a, int x, int y)
    {
        return std::max(std::abs(a.x - x), std::abs(a.y - y));
    }

    Cell cellOf(int32_t worldX, int32_t worldY)
    {
        return {static_cast<int>(worldX / kTileSize), static_cast<int>(worldY / kTileSize)};
    }

    // An object's map position starts at +0x18 (the walk target snap, ENG 00616B9B, passes it): the sector id
    // cache, x, y, then the layer byte at +0x24.
    uint8_t layerOf(const cCreature* creature)
    {
        return reinterpret_cast<const uint8_t*>(creature)[0x24];
    }

    // The loaded sectors' tiles; each tile is tested once per plan.
    struct Grid
    {
        int baseX = 0, baseY = 0;               // world tile of the 5x5's corner
        Sector* sectors[WideSectors::kCount] = {};
        cMapData* map = nullptr;
        uint8_t layer = 0;                      // the walker's (its position's +0xC), as the path finders test with
        std::vector<uint8_t> state;             // 0 not tested yet, 1 walkable, 2 blocked
        std::vector<Cell> avoid;                // tiles the game's path finder found no way over during this walk

        bool load()
        {
            map = WideSectors::map();
            const Sector* center = WideSectors::at(WideSectors::kCount / 2);
            if (!g_walkable || !WideSectors::active() || !map || !center)
            {
                return false;
            }
            baseX = center->originX - 2 * 64;
            baseY = center->originY - 2 * 64;
            for (int slot = 0; slot < WideSectors::kCount; ++slot)
            {
                // The table changes on the render thread: a copy, each sector checked against its place.
                Sector* s = WideSectors::at(slot);
                const int wantX = baseX + slot % WideSectors::kSide * 64;
                const int wantY = baseY + slot / WideSectors::kSide * 64;
                sectors[slot] = s && s->originX == wantX && s->originY == wantY ? s : nullptr;
            }
            state.assign(kCells, 0);
            // A shut door that opens is a way through, not the wall the tile test makes of it (the player,
            // 2026-11-11: «el agente continúa con problemas con las puertas para conseguir llegar a sus objetivos»: a
            // goal behind a gate, a graveyard's, had the route end at the nearest tile outside). Its tile and those
            // around it; the hero opens it when it is on the way (AutoAttack's doorOnTheWay).
            for (const auto& [dx, dy] : MapMemory::doorsThatOpen(layer, baseX, baseY, baseX + kSide, baseY + kSide))
            {
                for (int y = dy - 1; y <= dy + 1; ++y)
                {
                    for (int x = dx - 1; x <= dx + 1; ++x)
                    {
                        if (inside(x, y))
                        {
                            state[(y - baseY) * kSide + (x - baseX)] = 1;
                        }
                    }
                }
            }
            for (const Cell& c : avoid)
            {
                if (inside(c.x, c.y))
                {
                    state[(c.y - baseY) * kSide + (c.x - baseX)] = 2;
                }
            }
            return true;
        }

        bool inside(int x, int y) const
        {
            return x >= baseX && y >= baseY && x < baseX + kSide && y < baseY + kSide;
        }

        bool walkable(int x, int y)
        {
            if (!inside(x, y))
            {
                return false;
            }
            uint8_t& s = state[(y - baseY) * kSide + (x - baseX)];
            if (!s)
            {
                const Sector* sector = sectors[(y - baseY) / 64 * WideSectors::kSide + (x - baseX) / 64];
                bool ok = false;
                if (sector && (sector->flags & Sector::loaded) && sector->tiles)
                {
                    TilePos p{};
                    p.x = x;
                    p.y = y;
                    p.layer = layer;
                    ok = g_walkable(map, nullptr, &p, 0) != 0;
                }
                else
                {
                    // Not loaded (yet): the tile as the world files have it.
                    ok = WorldTiles::walkable(layer, x, y) == 1;
                }
                s = ok ? 1 : 2;
            }
            return s == 1;
        }
    };

    struct Route
    {
        std::vector<Cell> cells;                // hero's tile .. goal's tile
        size_t from = 0;                        // the route tile nearest the hero when the current leg went out
        size_t legEnd = 0;                      // the current leg's end
        int legTiles = kLegTiles;               // how far out legs end (shorter after a stuck one)
        int replans = 0;
        bool partial = false;                   // ends short of the goal, at the loaded sectors' edge: planned on
        int hops = 0;                           // partial routes walked toward one goal
    };

    Grid g_grid;
    Route g_route;
    bool g_routed = false;                      // the far walk follows g_route
    // A click within the path finders' grid goes to the game unchanged: its path finder knows doors and stairs into
    // houses and cellars (another layer, which a route planned here over the hero's layer misses). Only when the hero
    // does not move (no way inside the grid: around a river) does the walk get a route.
    bool g_native = false;

    // A* with 8 neighbours (no corner cutting) from the hero's tile to the goal's, or to the nearest walkable tile
    // around it. Fills g_route.cells.
    bool planUnguarded(Cell start, Cell goal)
    {
        // No grid of the live sectors while a game loads (a crash in here 14 s after a load, the analyst 2026-11-11).
        if (AgentSave::worldChanging())
        {
            return false;
        }
        const DWORD t0 = GetTickCount();
        if (!g_grid.load() || !g_grid.inside(start.x, start.y))
        {
            return false;
        }
        // A goal beyond the loaded sectors (a click on the agent's map): first as far as they reach, along a route over
        // what the map memory knows (walked, or walkable by the tile test), else straight at it; planned on from there.
        constexpr int kEdge = 12;
        const auto nearEdge = [](int x, int y) {
            return x < g_grid.baseX + kEdge || y < g_grid.baseY + kEdge || x >= g_grid.baseX + kSide - kEdge ||
                   y >= g_grid.baseY + kSide - kEdge;
        };
        bool partial = false;
        if (nearEdge(goal.x, goal.y))
        {
            partial = true;
            std::vector<std::pair<int, int>> wide;
            Cell stop = start;
            if (MapMemory::route(g_grid.layer, start.x, start.y, goal.x, goal.y, wide))
            {
                for (const auto& [x, y] : wide)
                {
                    if (nearEdge(x, y))
                    {
                        break;
                    }
                    stop = {x, y};
                }
            }
            else
            {
                const double dx = goal.x - start.x, dy = goal.y - start.y;
                const double reach = kSide / 2.0 - kEdge - 4;
                const double scale = std::min(1.0, reach / std::max(std::abs(dx), std::abs(dy)));
                stop = {start.x + static_cast<int>(dx * scale), start.y + static_cast<int>(dy * scale)};
            }
            if (chebyshev(stop, start.x, start.y) < 4)
            {
                return false;
            }
            goal = stop;
        }
        if (!g_grid.walkable(goal.x, goal.y))
        {
            // A click on a wall, a tree or water: the nearest tile around it one can stand on.
            bool found = false;
            for (int r = 1; r <= (partial ? 12 : 4) && !found; ++r)
            {
                int bestD = INT_MAX;
                Cell best{};
                for (int dy = -r; dy <= r; ++dy)
                {
                    for (int dx = -r; dx <= r; ++dx)
                    {
                        if (std::max(std::abs(dx), std::abs(dy)) == r && g_grid.walkable(goal.x + dx, goal.y + dy))
                        {
                            const int d = dx * dx + dy * dy;
                            if (d < bestD)
                            {
                                bestD = d;
                                best = {goal.x + dx, goal.y + dy};
                                found = true;
                            }
                        }
                    }
                }
                if (found)
                {
                    goal = best;
                }
            }
            if (!found)
            {
                return false;
            }
        }

        static std::vector<int32_t> cost;
        static std::vector<int8_t> came;        // direction index into kDx/kDy the tile was reached by
        cost.assign(kCells, INT32_MAX);
        came.assign(kCells, -1);
        static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
        static constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
        auto index = [](int x, int y) { return (y - g_grid.baseY) * kSide + (x - g_grid.baseX); };
        auto estimate = [&](int x, int y) {
            const int dx = std::abs(x - goal.x), dy = std::abs(y - goal.y);
            return 10 * (dx + dy) - 6 * std::min(dx, dy);
        };
        using Entry = std::pair<int32_t, int32_t>;  // (estimated total, cell)
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
        const int startCell = index(start.x, start.y), goalCell = index(goal.x, goal.y);
        cost[startCell] = 0;
        open.push({estimate(start.x, start.y), startCell});
        int expanded = 0;
        bool reached = false;
        while (!open.empty())
        {
            const auto [f, cell] = open.top();
            open.pop();
            const int x = g_grid.baseX + cell % kSide, y = g_grid.baseY + cell / kSide;
            if (f - estimate(x, y) > cost[cell])
            {
                continue;   // a stale entry
            }
            if (cell == goalCell)
            {
                reached = true;
                break;
            }
            ++expanded;
            for (int d = 0; d < 8; ++d)
            {
                const int nx = x + kDx[d], ny = y + kDy[d];
                if (!g_grid.walkable(nx, ny))
                {
                    continue;
                }
                if (d >= 4 && (!g_grid.walkable(x + kDx[d], y) || !g_grid.walkable(x, y + kDy[d])))
                {
                    continue;
                }
                const int n = index(nx, ny);
                int32_t c = cost[cell] + (d < 4 ? 10 : 14);
                if (Config::autoAttack.ownPaths)
                {
                    // Kept off walls, trees and water's edge: each blocked tile around costs as 0.4 of a step, so
                    // the route runs mid-path where the hero does not snag on corners.
                    for (int e = 0; e < 8; ++e)
                    {
                        if (!g_grid.walkable(nx + kDx[e], ny + kDy[e]))
                        {
                            c += 4;
                        }
                    }
                }
                if (c < cost[n])
                {
                    cost[n] = c;
                    came[n] = static_cast<int8_t>(d);
                    open.push({c + estimate(nx, ny), n});
                }
            }
        }
        if (Config::debug.uiTrace)
        {
            LOG("Click walk: route from {},{} to {},{} on layer {}: {} ({} tiles searched, {} ms)", start.x, start.y,
                goal.x, goal.y, g_grid.layer, reached ? "found" : "none", expanded, GetTickCount() - t0);
        }
        if (!reached)
        {
            return false;
        }
        auto& cells = g_route.cells;
        cells.clear();
        for (int cell = goalCell; cell != startCell;)
        {
            const int x = g_grid.baseX + cell % kSide, y = g_grid.baseY + cell / kSide;
            cells.push_back({x, y});
            const int d = came[cell];
            cell = index(x - kDx[d], y - kDy[d]);
        }
        cells.push_back(start);
        std::reverse(cells.begin(), cells.end());
        g_route.from = 0;
        g_route.legEnd = 0;
        g_route.legTiles = kLegTiles;
        g_route.partial = partial;
        return true;
    }

    // A straight walk from a to b over walkable tiles only (sampled every quarter tile, no corner cut): a leg the
    // game's path finder has nothing to decide on.
    bool lineClear(Cell a, Cell b)
    {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const int steps = std::max(1, static_cast<int>(std::max(std::abs(dx), std::abs(dy)) * 4));
        int px = a.x, py = a.y;
        for (int i = 1; i <= steps; ++i)
        {
            const int x = static_cast<int>(std::floor(a.x + 0.5 + dx * i / steps));
            const int y = static_cast<int>(std::floor(a.y + 0.5 + dy * i / steps));
            if (x == px && y == py)
            {
                continue;
            }
            if (!g_grid.walkable(x, y) || (x != px && y != py && (!g_grid.walkable(x, py) || !g_grid.walkable(px, y))))
            {
                return false;
            }
            px = x;
            py = y;
        }
        return true;
    }

    // The next leg's end: the furthest route tile after the one nearest the hero that stays within kLegTiles of
    // him. False if the hero is off the route.
    // The sectors are read on this (the window's) thread while the render thread loads and unloads them: one gone
    // under the plan (A3 2026-11-11 23:49, c0000005 in Grid::load reading the middle sector's origin) is no plan
    // this time, not a crash. The SEH frame in a function of its own, with no C++ objects to unwind.
    int planFaults = 0;

    bool planGuarded(Cell start, Cell goal)
    {
        __try
        {
            return planUnguarded(start, goal);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ++planFaults;
            return false;
        }
    }

    bool plan(Cell start, Cell goal)
    {
        const int before = planFaults;
        if (planGuarded(start, goal))
        {
            return true;
        }
        if (planFaults != before)
        {
            g_route.cells.clear();
            if (planFaults <= 10)
            {
                LOG("Click walk: a sector went away under the plan (unloaded meanwhile): no route this time");
            }
        }
        return false;
    }

    bool nextLeg(Cell hero, Cell& end)
    {
        const auto& cells = g_route.cells;
        size_t nearest = g_route.from;
        int nearestD = INT_MAX;
        const size_t limit = std::min(cells.size(), g_route.from + 4 * kLegTiles);
        for (size_t i = g_route.from; i < limit; ++i)
        {
            const int d = chebyshev(cells[i], hero.x, hero.y);
            if (d < nearestD)
            {
                nearestD = d;
                nearest = i;
            }
        }
        if (nearestD > kOffRouteTiles)
        {
            return false;
        }
        size_t j = nearest;
        while (j + 1 < cells.size() && chebyshev(cells[j + 1], hero.x, hero.y) <= g_route.legTiles &&
               (!Config::autoAttack.ownPaths || lineClear(hero, cells[j + 1])))
        {
            ++j;
        }
        if (j == nearest && j + 1 < cells.size())
        {
            ++j;    // the next route tile at least (beside the hero, the line test is too strict at a corner)
        }
        g_route.from = nearest;
        g_route.legEnd = j;
        end = cells[j];
        return true;
    }

    // ---- The far walk ----

    // A far walk in progress (the window thread's, but for g_active, which the render thread reads).
    std::atomic<bool> g_active{false};
    bool g_ours = false;                        // the order going out is our next leg
    bool g_asking = false;                      // the order going out is one asked for on the agent's map
    std::atomic<bool> g_fromMap{false};         // the far walk is one asked for on the agent's map
    std::atomic<bool> g_askByPlayer{true};      // a click of the player's on the map (not the agent's own travel)
    int32_t g_goalX = 0, g_goalY = 0;           // where the player clicked
    int32_t g_lastX = 0, g_lastY = 0;           // the hero at the last check
    int32_t g_legX = 0, g_legY = 0;             // the hero when the current leg went out
    int g_try = 0;                              // kTries index of the next leg
    double g_best = 0.0;                        // the closest the hero got to the goal, in tiles
    int g_staleLegs = 0;                        // legs since the hero last got closer than g_best
    DWORD g_legStart = 0;
    DWORD g_stillSince = 0;                     // the hero's position last changed
    DWORD g_nextCheck = 0;                      // the render thread's

    double tilesTo(const cCreature* creature)
    {
        const double dx = static_cast<double>(g_goalX) - creature->x;
        const double dy = static_cast<double>(g_goalY) - creature->y;
        return std::max(std::abs(dx), std::abs(dy)) / kTileSize;
    }

    void finish(const char* why)
    {
        g_fromMap = false;
        g_routed = false;
        g_native = false;
        if (g_active.exchange(false) && Config::debug.uiTrace)
        {
            LOG("Click walk: {} ({},{})", why, g_goalX, g_goalY);
        }
    }

    // A route leg's end as a walk target: the tile's middle (on the clicked tile: the clicked spot).
    void target(const Cell& c, int32_t& x, int32_t& y)
    {
        const Cell goal = cellOf(g_goalX, g_goalY);
        if (c.x == goal.x && c.y == goal.y)
        {
            x = g_goalX;
            y = g_goalY;
            return;
        }
        x = static_cast<int32_t>(std::lround((c.x + 0.5) * kTileSize));
        y = static_cast<int32_t>(std::lround((c.y + 0.5) * kTileSize));
    }

    // Riding (the hero's mount field hero+0x1EC set), the controlled creature is the horse (Manolito AD/AG); the
    // orders still go to the hero, whose handler (0x52E590) passes a move on to the horse, which swallows direct ones
    // (Manolito, phase 2, 2026-11-12). A route went on dying the moment the agent got up (the reviewer, 2026-11-12).
    bool controls(cEngine* engine, cObjectManager* objects, cCreature* hero)
    {
        if (engine->controlled == hero->id)
        {
            return true;
        }
        return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x1EC) &&
               objects && objects->creature(engine->controlled);
    }

    void __fastcall hookSendOrder(cEngine* engine, void* edx, cCreature* creature, cOrder* order)
    {
        cObjectManager* objects = cObjectManager::instance();
        if (order && creature && objects && objects->hero() == creature)
        {
            bool ours = g_ours;
            g_ours = false;
            bool kept = false;
            // The agent asking again for the goal its own route already walks to: the route goes on (no plan again,
            // no new leg's grace), the order sent as the current leg.
            if (!ours && g_asking && !g_askByPlayer && g_active && g_routed && order->type == cOrder::move &&
                std::max(std::abs(static_cast<double>(order->x) - g_goalX),
                         std::abs(static_cast<double>(order->y) - g_goalY)) < 2.0 * kTileSize &&
                g_route.legEnd < g_route.cells.size())
            {
                target(g_route.cells[g_route.legEnd], order->x, order->y);
                ours = true;
                kept = true;
            }
            if (!ours)
            {
                // Any other order for the hero (a click, hold-to-walk, an attack, stop) ends a far walk.
                finish("another order");
                g_fromMap = g_asking && g_askByPlayer;
            }
            if (order->type == cOrder::move && order->mode == cOrder::walk && !order->follow)
            {
                const double dx = static_cast<double>(order->x) - creature->x;
                const double dy = static_cast<double>(order->y) - creature->y;
                const double tiles = std::max(std::abs(dx), std::abs(dy)) / kTileSize;
                bool own = false;
                if (!ours && g_asking && !g_askByPlayer && Config::autoAttack.ownPaths && tiles > 3.0 &&
                    tiles <= kMaxTiles)
                {
                    // The agent's walk on its own route, not the game's path finder (which keeps the doors and stairs
                    // into houses: when no route is found on the hero's layer, the order goes to the game as it is).
                    g_grid.avoid.clear();
                    g_grid.layer = layerOf(creature);
                    const Cell from = cellOf(creature->x, creature->y);
                    Cell end{};
                    const int32_t goalX = order->x, goalY = order->y;
                    g_goalX = goalX;
                    g_goalY = goalY;
                    if (plan(from, cellOf(goalX, goalY)) && nextLeg(from, end))
                    {
                        own = true;
                        g_native = false;
                        g_routed = true;
                        g_route.replans = 0;
                        g_route.hops = 0;
                        g_active = true;
                        target(end, order->x, order->y);
                        static int s_told = 0;
                        if (Config::debug.uiTrace || s_told++ < 20)
                        {
                            LOG("Click walk: own route to {},{} ({:.0f} tiles, {} route tiles), first leg to {},{}",
                                goalX, goalY, tiles, g_route.cells.size(), order->x, order->y);
                        }
                    }
                }
                if (own)
                {
                    // Planned above: neither the game's walk nor the far route below.
                }
                else if (!ours && tiles <= kMaxTiles)
                {
                    g_native = true;
                    g_routed = false;
                    g_goalX = order->x;
                    g_goalY = order->y;
                    g_active = true;
                }
                else if (!ours && tiles > kPlanTiles)
                {
                    g_goalX = order->x;
                    g_goalY = order->y;
                    g_grid.avoid.clear();
                    g_grid.layer = layerOf(creature);
                    const Cell from = cellOf(creature->x, creature->y);
                    Cell end{};
                    if (plan(from, cellOf(order->x, order->y)) && nextLeg(from, end))
                    {
                        g_routed = true;
                        g_route.replans = 0;
                        g_route.hops = 0;
                        g_active = true;
                        target(end, order->x, order->y);
                        if (Config::debug.uiTrace)
                        {
                            LOG("Click walk: target {},{} is {:.0f} tiles out, {} route tiles, first leg to {},{}",
                                g_goalX, g_goalY, tiles, g_route.cells.size(), order->x, order->y);
                        }
                    }
                }
                if (!g_routed && !g_native)
                {
                    const Try t = ours ? kTries[g_try] : kTries[0];
                    if (tiles > t.tiles || (ours && t.degrees != 0.0))
                    {
                        if (!ours)
                        {
                            // Remember the clicked spot; the hero walks there leg by leg.
                            g_goalX = order->x;
                            g_goalY = order->y;
                            g_try = 0;
                            g_best = tiles;
                            g_staleLegs = 0;
                            g_active = true;
                        }
                        const double scale = std::min(1.0, t.tiles / tiles);
                        const double a = t.degrees * 3.14159265358979 / 180.0;
                        const double rx = (dx * std::cos(a) - dy * std::sin(a)) * scale;
                        const double ry = (dx * std::sin(a) + dy * std::cos(a)) * scale;
                        const int32_t x = creature->x + static_cast<int32_t>(std::lround(rx));
                        const int32_t y = creature->y + static_cast<int32_t>(std::lround(ry));
                        if (Config::debug.uiTrace)
                        {
                            LOG("Click walk: target {},{} is {:.0f} tiles out, walking to {},{} first (turned {})",
                                order->x, order->y, tiles, x, y, t.degrees);
                        }
                        order->x = x;
                        order->y = y;
                    }
                }
                if (g_active && !kept)
                {
                    g_lastX = g_legX = creature->x;
                    g_lastY = g_legY = creature->y;
                    g_legStart = g_stillSince = GetTickCount();
                }
            }
        }
        g_origSendOrder(engine, edx, creature, order);
    }

    void sendLeg(cEngine* engine, cCreature* hero, int32_t x, int32_t y)
    {
        cOrder order;
        order.type = cOrder::move;
        order.mode = cOrder::walk;
        order.x = x;
        order.y = y;
        order.layer = layerOf(hero);    // left 0, a hero on layer 1-2 had its order snapped to layer 0 and never set off
        g_ours = true;
        engine->sendOrder(hero, order);
        g_ours = false;
    }

    // The next route leg once the hero nears the current one's end or stands, planning again if he left the route.
    void continueRoute(cEngine* engine, cCreature* hero, bool stopped)
    {
        const Cell at = cellOf(hero->x, hero->y);
        const Cell end = g_route.cells[g_route.legEnd];
        const int toEnd = chebyshev(end, at.x, at.y);
        if (g_route.legEnd + 1 == g_route.cells.size())
        {
            if (g_route.partial)
            {
                // Nearing the end of a stretch short of the goal: the next one from here, the sectors around moved on.
                if (toEnd > kAheadTiles && !stopped)
                {
                    return;
                }
                if (stopped && toEnd > 2 && ++g_route.replans > kMaxReplans)
                {
                    finish("blocked");
                    return;
                }
                Cell next{};
                g_grid.layer = layerOf(hero);
                if (++g_route.hops > 400 || !plan(at, cellOf(g_goalX, g_goalY)) || !nextLeg(at, next))
                {
                    finish("blocked");
                    return;
                }
                int32_t x, y;
                target(next, x, y);
                if (Config::debug.uiTrace)
                {
                    LOG("Click walk: next stretch toward {},{} ({} route tiles{})", g_goalX, g_goalY,
                        g_route.cells.size(), g_route.partial ? ", short of the goal" : "");
                }
                sendLeg(engine, hero, x, y);
                return;
            }
            if (stopped)
            {
                finish(toEnd <= 2 ? "arrived" : "stopped short");
            }
            return;
        }
        if (!stopped && !(toEnd <= kAheadTiles && GetTickCount() - g_legStart >= kMinLegMs))
        {
            return;
        }
        // Progress along the route, not movement: at a bridge the game's path finder walked the hero a few tiles
        // off and back for each long leg.
        size_t nearest = g_route.from;
        int nearestD = INT_MAX;
        const size_t limit = std::min(g_route.cells.size(), g_route.legEnd + 1);
        for (size_t i = g_route.from; i < limit; ++i)
        {
            const int d = chebyshev(g_route.cells[i], at.x, at.y);
            if (d < nearestD)
            {
                nearestD = d;
                nearest = i;
            }
        }
        const bool advanced = nearestD <= kOffRouteTiles && nearest >= g_route.from + kMovedTiles;
        Cell next{};
        const bool stuck = stopped && !advanced && toEnd > 2;
        if (advanced)
        {
            g_route.legTiles = kLegTiles;
        }
        bool shorter = false;
        if (stuck && g_route.legTiles > kMinLegTiles)
        {
            // The game's path finder may not find a long leg where it finds short ones (over a bridge): the same
            // route in shorter legs first.
            g_route.legTiles = std::max(kMinLegTiles, g_route.legTiles / 2);
            shorter = nextLeg(at, next);
        }
        if (!shorter && (stuck || !nextLeg(at, next)))
        {
            // Stuck on the way (the tile test lets through what the game's path finder does not: under a bridge,
            // a closed gate) or off the route (pushed aside): the next stretch of the route counts as blocked
            // for this walk, and the route is planned again from here.
            g_grid.layer = layerOf(hero);
            if (stuck)
            {
                // From past the hero to the leg's end (not the goal): somewhere on it the game found no way.
                const size_t first = std::min(g_route.from + 2, g_route.cells.size() - 1);
                const size_t last = std::min(g_route.legEnd + 1, g_route.cells.size() - 1);
                for (size_t i = first; i < last; ++i)
                {
                    g_grid.avoid.push_back(g_route.cells[i]);
                }
            }
            if (++g_route.replans > kMaxReplans || !plan(at, cellOf(g_goalX, g_goalY)) || !nextLeg(at, next))
            {
                finish("blocked");
                return;
            }
        }
        int32_t x, y;
        target(next, x, y);
        if (Config::debug.uiTrace)
        {
            LOG("Click walk: leg from {},{} to {},{} (route tile {} of {}, legs up to {} tiles{})", at.x, at.y, next.x,
                next.y, g_route.legEnd, g_route.cells.size(), g_route.legTiles, stuck ? ", after a stuck leg" : "");
        }
        sendLeg(engine, hero, x, y);
    }

    // A walk asked for from the agent's map: sent as the player's click would be, so the hook above routes it.
    // Asked with the ModKit menu open, when the game takes no orders: kept until it does (the menu closed).
    std::atomic<int32_t> g_askX{0}, g_askY{0};
    std::atomic<bool> g_askPending{false};

    void askedWalk(void*)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!g_askPending || !engine || !hero || (engine->flags & (cEngine::loading | cEngine::noWorldInput)) ||
            !controls(engine, objects, hero))
        {
            return;
        }
        g_askPending = false;
        cOrder order;
        order.type = cOrder::move;
        order.mode = cOrder::walk;
        order.x = g_askX;
        order.y = g_askY;
        order.layer = layerOf(hero);
        LOG("Click walk: to {},{} from the agent's map ({:.0f} tiles)", order.x, order.y,
            std::max(std::abs(static_cast<double>(order.x) - hero->x), std::abs(static_cast<double>(order.y) - hero->y)) /
                kTileSize);
        g_asking = true;
        engine->sendOrder(hero, order);
        g_asking = false;
    }

    // On the window's thread, where the game sends its own orders: the next leg.
    void continueWalk(void*)
    {
        if (!g_active)
        {
            return;
        }
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!engine || !hero || (engine->flags & (cEngine::loading | cEngine::noWorldInput)) ||
            !controls(engine, objects, hero))
        {
            finish("no hero to walk");
            return;
        }
        const DWORD now = GetTickCount();
        if (hero->x != g_lastX || hero->y != g_lastY)
        {
            g_lastX = hero->x;
            g_lastY = hero->y;
            g_stillSince = now;
        }
        const bool stopped = now - g_legStart >= kSettleMs && now - g_stillSince >= kStillMs;
        const double left = tilesTo(hero);
        if (left <= kArrivedTiles)
        {
            finish("arrived");
            return;
        }
        if (g_native)
        {
            if (!stopped)
            {
                return;
            }
            const double moved = std::max(std::abs(static_cast<double>(hero->x) - g_legX),
                                     std::abs(static_cast<double>(hero->y) - g_legY)) / kTileSize;
            if (moved >= kMovedTiles)
            {
                finish("walked by the game");
                return;
            }
            // The game found no way: a route over the loaded sectors, else legs turned aside.
            g_native = false;
            g_grid.avoid.clear();
            g_grid.layer = layerOf(hero);
            const Cell from = cellOf(hero->x, hero->y);
            Cell end{};
            if (plan(from, cellOf(g_goalX, g_goalY)) && nextLeg(from, end))
            {
                g_routed = true;
                g_route.replans = 0;
                g_route.hops = 0;
                int32_t x, y;
                target(end, x, y);
                if (Config::debug.uiTrace)
                {
                    LOG("Click walk: the game found no way to {},{}, routing", g_goalX, g_goalY);
                }
                sendLeg(engine, hero, x, y);
                return;
            }
            g_try = 1;
            g_best = left;
            g_staleLegs = 0;
            sendLeg(engine, hero, g_goalX, g_goalY);
            return;
        }
        if (g_routed)
        {
            continueRoute(engine, hero, stopped);
            return;
        }
        if (!stopped)
        {
            return;
        }
        if (left < g_best - 0.5)
        {
            g_best = left;
            g_staleLegs = 0;
        }
        else if (++g_staleLegs >= kMaxStaleLegs)
        {
            finish("blocked");
            return;
        }
        const double movedTiles = std::max(std::abs(static_cast<double>(hero->x) - g_legX),
                                      std::abs(static_cast<double>(hero->y) - g_legY)) / kTileSize;
        if (movedTiles < kMovedTiles)
        {
            // No way to the leg's end: the next try (turned aside, shorter), until none is left.
            if (++g_try >= kTryCount)
            {
                finish("blocked");
                return;
            }
        }
        else
        {
            // Moved: straight at the goal again from here.
            g_try = 0;
        }
        sendLeg(engine, hero, g_goalX, g_goalY);
    }
}

void ClickWalk::walkTo(int32_t x, int32_t y, bool byPlayer)
{
    g_askX = x;
    g_askY = y;
    g_askByPlayer = byPlayer;
    g_askPending = true;
}

void ClickWalk::cancel()
{
    g_askPending = false;
    if (g_active && !g_fromMap)
    {
        Inject::call([](void*) {
            if (!g_fromMap)
            {
                finish("left for the agent's own order");
            }
        }, nullptr);
    }
}

bool ClickWalk::routing()
{
    return g_active || g_askPending;
}

bool ClickWalk::walking()
{
    return g_active && g_fromMap;
}

int ClickWalk::walkable(int layer, int tx, int ty)
{
    // Not the live sectors while a game loads: the world files only.
    if (AgentSave::worldChanging())
    {
        return WorldTiles::walkable(layer, tx, ty);
    }
    cMapData* map = WideSectors::map();
    const Sector* center = WideSectors::at(WideSectors::kCount / 2);
    if (!g_walkable || !WideSectors::active() || !map || !center)
    {
        return WorldTiles::walkable(layer, tx, ty);
    }
    const int baseX = center->originX - 2 * 64, baseY = center->originY - 2 * 64;
    if (tx < baseX || ty < baseY || tx >= baseX + kSide || ty >= baseY + kSide)
    {
        return WorldTiles::walkable(layer, tx, ty);
    }
    const int slot = (ty - baseY) / 64 * WideSectors::kSide + (tx - baseX) / 64;
    const Sector* sector = WideSectors::at(slot);
    if (!sector || sector->originX != baseX + slot % WideSectors::kSide * 64 ||
        sector->originY != baseY + slot / WideSectors::kSide * 64 || !(sector->flags & Sector::loaded) || !sector->tiles)
    {
        return WorldTiles::walkable(layer, tx, ty);
    }
    TilePos p{};
    p.x = tx;
    p.y = ty;
    p.layer = static_cast<uint8_t>(layer);
    return g_walkable(map, nullptr, &p, 0) ? 1 : 0;
}

void ClickWalk::install()
{
    if (!Addr::cObjectManager_hero || !Addr::g_pObjectManager)
    {
        return;
    }
    if (Sig::resolve(kSigs) == 0)
    {
        g_walkable = reinterpret_cast<WalkableFn>(s_walkable);
    }
    else
    {
        LOG("Click walk: no tile test, far clicks walk in straight legs only");
    }
    Patch::hook(g_origSendOrder, Addr::cEngine_sendOrder, &hookSendOrder, "cEngine::sendOrder");
}

void ClickWalk::onFrame()
{
    // The agent's own walk waits for the game's cursor on the spot (AgentCursor), the player's does not.
    if (g_askPending && (g_askByPlayer || AgentCursor::reach({0, g_askX, g_askY})))
    {
        Inject::call(&askedWalk, nullptr);
    }
    if (!g_active)
    {
        return;
    }
    const DWORD now = GetTickCount();
    if (static_cast<int>(now - g_nextCheck) < 0)
    {
        return;
    }
    g_nextCheck = now + kCheckMs;
    Inject::call(&continueWalk, nullptr);
}
