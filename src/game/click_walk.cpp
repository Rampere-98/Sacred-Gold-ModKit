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
    constexpr double kProgressTiles = 1.0;      // a leg that got the hero less than this much closer was blocked
    constexpr DWORD kCheckMs = 150;             // how often a far walk looks whether the hero stopped
    constexpr DWORD kSettleMs = 300;            // a new leg's grace before a standing hero counts as stopped

    // A far walk in progress (the window thread's, but for g_active, which the render thread reads).
    std::atomic<bool> g_active{false};
    bool g_ours = false;                        // the order going out is our next leg
    int32_t g_goalX = 0, g_goalY = 0;           // where the player clicked
    int32_t g_lastX = 0, g_lastY = 0;           // the hero at the last check
    double g_legStartTiles = 0.0;               // distance to the goal when the current leg went out
    DWORD g_legStart = 0;
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
                if (tiles > kMaxTiles)
                {
                    if (!ours)
                    {
                        // Remember the clicked spot; the hero walks there leg by leg.
                        g_goalX = order->x;
                        g_goalY = order->y;
                        g_active = true;
                    }
                    const double scale = kMaxTiles / tiles;
                    const int32_t x = creature->x + static_cast<int32_t>(std::lround(dx * scale));
                    const int32_t y = creature->y + static_cast<int32_t>(std::lround(dy * scale));
                    if (Config::debug.uiTrace)
                    {
                        LOG("Click walk: target {},{} is {:.0f} tiles out, walking to {},{} first", order->x,
                            order->y, tiles, x, y);
                    }
                    order->x = x;
                    order->y = y;
                }
                if (g_active)
                {
                    g_lastX = creature->x;
                    g_lastY = creature->y;
                    g_legStartTiles = tilesTo(creature);
                    g_legStart = GetTickCount();
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
        const bool moved = hero->x != g_lastX || hero->y != g_lastY;
        g_lastX = hero->x;
        g_lastY = hero->y;
        if (moved || GetTickCount() - g_legStart < kSettleMs)
        {
            return;
        }
        const double left = tilesTo(hero);
        if (left <= kArrivedTiles)
        {
            finish("arrived");
            return;
        }
        if (g_legStartTiles - left < kProgressTiles)
        {
            finish("blocked");
            return;
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
