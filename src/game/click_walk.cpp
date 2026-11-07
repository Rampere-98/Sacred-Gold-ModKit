#include "game/click_walk.h"
#include "config/debug.h"
#include "game/frame_hooks.h"
#include "input/inject.h"
#include "log.h"
#include "patch.h"
#include "sacred/engine.h"
#include "sacred/object.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace
{
    using namespace Sacred;

    decltype(Addr::cEngine_sendOrder)::Ptr g_origSendOrder = nullptr;

    constexpr double kTileSize = 53.66563;      // world units per tile (the world -> tile conversion, ENG 006224E0)
    constexpr double kMaxTiles = 30.0;          // inside the path finders' +-32 tile grid, a margin for rounding
    constexpr double kArrivedTiles = 1.5;       // close enough to the clicked spot
    constexpr double kMovedTiles = 2.0;         // a leg that moved the hero less than this found no way
    constexpr int kMaxStaleLegs = 10;           // legs in a row that got no closer than the best so far: give up
    constexpr DWORD kCheckMs = 100;             // how often a far walk looks whether the hero stopped
    constexpr DWORD kSettleMs = 1000;           // a new leg's grace (path finding, starting to walk)
    constexpr DWORD kStillMs = 500;             // the hero's position steps a tile at a time: still this long = stopped

    // When the leg straight at the goal finds no way (its end in water, behind a wall, or no path inside the path
    // finder's grid), legs turned aside and shorter ones: around a river bend or a wall end. Degrees, tiles.
    struct Try
    {
        double degrees, tiles;
    };
    constexpr Try kTries[] = {{0, kMaxTiles}, {45, 22}, {-45, 22}, {90, 16}, {-90, 16}, {0, 14}, {135, 10},
        {-135, 10}, {0, 7}};
    constexpr int kTryCount = static_cast<int>(sizeof(kTries) / sizeof(kTries[0]));

    // A far walk in progress (the window thread's, but for g_active, which the render thread reads).
    std::atomic<bool> g_active{false};
    bool g_ours = false;                        // the order going out is our next leg
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
        if (g_active.exchange(false) && Config::debug.uiTrace)
        {
            LOG("Click walk: {} ({},{})", why, g_goalX, g_goalY);
        }
    }

    void __fastcall hookSendOrder(cEngine* engine, void* edx, cCreature* creature, cOrder* order)
    {
        cObjectManager* objects = cObjectManager::instance();
        if (order && creature && objects && objects->hero() == creature)
        {
            const bool ours = g_ours;
            g_ours = false;
            if (!ours)
            {
                // Any other order for the hero (a click, hold-to-walk, an attack, stop) ends a far walk.
                finish("another order");
            }
            if (order->type == cOrder::move && order->mode == cOrder::walk && !order->follow)
            {
                const double dx = static_cast<double>(order->x) - creature->x;
                const double dy = static_cast<double>(order->y) - creature->y;
                const double tiles = std::max(std::abs(dx), std::abs(dy)) / kTileSize;
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
                if (g_active)
                {
                    g_lastX = g_legX = creature->x;
                    g_lastY = g_legY = creature->y;
                    g_legStart = g_stillSince = GetTickCount();
                }
            }
        }
        g_origSendOrder(engine, edx, creature, order);
    }

    // On the window's thread, where the game sends its own orders: the next leg once the hero stands.
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
            engine->controlled != hero->id)
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
        if (now - g_legStart < kSettleMs || now - g_stillSince < kStillMs)
        {
            return;
        }
        const double left = tilesTo(hero);
        if (left <= kArrivedTiles)
        {
            finish("arrived");
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
        cOrder order;
        order.type = cOrder::move;
        order.mode = cOrder::walk;
        order.x = g_goalX;
        order.y = g_goalY;
        g_ours = true;
        engine->sendOrder(hero, order);
        g_ours = false;
    }
}

void ClickWalk::install()
{
    if (!Addr::cObjectManager_hero || !Addr::g_pObjectManager)
    {
        return;
    }
    Patch::hook(g_origSendOrder, Addr::cEngine_sendOrder, &hookSendOrder, "cEngine::sendOrder");
}

void ClickWalk::onFrame()
{
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
