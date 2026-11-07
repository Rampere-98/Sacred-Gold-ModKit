#include "game/auto_attack.h"
#include "config/auto_attack.h"
#include "config/controller.h"
#include "game/aim_assist.h"
#include "game/controller.h"
#include "game/focus.h"
#include "game/frame_hooks.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <mutex>

using namespace Sacred;

namespace
{
    // The order a left click on a creature or object gives (the world mouse handler, ENG 006172C0, at 00617935):
    // cOrder's layout with type 7, the picked id at +0x18 and the clicked world point after it. The handler hands
    // it straight to the controlled creature's order handler (vtable +0x18), not through cEngine::sendOrder, which
    // would snap +0x18/+0x1C as a walk target. On an enemy the hero walks up to it and attacks it.
    struct UseOrder
    {
        uintptr_t vtable = Addr::cOrder_vtable;
        uint32_t type = 7;
        uint8_t _08[0x18 - 0x08] = {};
        uint32_t target = 0;
        int32_t x = 0, y = 0;
        uint8_t _24[0x44 - 0x24] = {};
    };
    static_assert(sizeof(UseOrder) == sizeof(cOrder));
    static_assert(offsetof(UseOrder, target) == 0x18 && offsetof(UseOrder, x) == 0x1C && offsetof(UseOrder, y) == 0x20);

    // cCreature's order handler, thiscall (order*), callee pops.
    using OrderHandler = void(__fastcall*)(cCreature* self, void* edx, UseOrder* order);

    bool g_on = false;
    bool g_keyWasDown = false;
    uint32_t g_target = 0;          // the enemy we attack, 0 for none
    DWORD g_sentAt = 0;             // when the last order went out
    DWORD g_quietUntil = 0;         // the player's own clicks: no auto attack before this tick

    // The physical state: SacredBild's GetAsyncKeyState is not the game's patched import, so injected input and
    // the controller never show here.
    bool physicallyDown(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    // A mouse button held: the player gives their own orders. Moving the mouse does not count, so the cursor stays
    // free for the inventory and other windows while the hero fights.
    bool playerClicks()
    {
        return physicallyDown(VK_LBUTTON) || physicallyDown(VK_RBUTTON) || physicallyDown(VK_MBUTTON);
    }

    bool heroAlive()
    {
        int health = 0, maximum = 0;
        return AimAssist::heroHealth(health, maximum) && health > 0;
    }

    // The current target if it is still there and within reach, else the nearest enemy in range.
    bool chooseTarget(AimAssist::Target& t)
    {
        float hx, hy;
        if (!AimAssist::hero(hx, hy))
        {
            return false;
        }
        const float range = static_cast<float>(Config::autoAttack.range);
        if (g_target && AimAssist::locate(g_target, AimAssist::Kind::Enemy, t))
        {
            // Keep chasing it up to half again the range, then look for a closer one.
            const float dx = t.x - hx, dy = t.y - hy;
            const float reach = range * 1.5f;
            if (dx * dx + dy * dy <= reach * reach)
            {
                return true;
            }
        }
        return AimAssist::find(AimAssist::Kind::Enemy, 0.0f, 0.0f, range, 360.0f, t);
    }

    // Orders the creature the mouse commands (the hero, or its mount) to attack `id`, as a click on it does.
    bool attack(uint32_t id)
    {
        cEngine* engine = FrameHooks::engine();
        cObjectManager* objects = cObjectManager::instance();
        if (!engine || !objects)
        {
            return false;
        }
        cCreature* self = objects->creature(engine->controlled);
        cObject* target = objects->object(id);
        if (!self || !target)
        {
            return false;
        }
        UseOrder order;
        order.target = id;
        order.x = target->x;
        order.y = target->y;
        const auto handler = reinterpret_cast<const OrderHandler*>(self->vtable)[6];
        handler(self, nullptr, &order);
        return true;
    }
}

void AutoAttack::install()
{
    // The controller installs the world pick hook; without it, we do.
    if (!Config::controller.enabled)
    {
        AimAssist::install();
    }
    g_on = Config::autoAttack.enabled;
    LOG("AutoAttack: {} (key 0x{:x}, range {} px, pause {} ms, repeat {} ms)", g_on ? "on" : "off",
        Config::autoAttack.key, Config::autoAttack.range, Config::autoAttack.pauseMs, Config::autoAttack.repeatMs);
}

void AutoAttack::onFrame()
{
    // Menus present from the UI thread, the game from the render thread; around loading both may.
    static std::mutex frameLock;
    const std::unique_lock lock(frameLock, std::try_to_lock);
    if (!lock)
    {
        return;
    }
    const DWORD now = GetTickCount();
    const bool foreground = Focus::foreground();

    // The toggle key, on its press and only while the game has the focus.
    const bool keyDown = foreground && physicallyDown(Config::autoAttack.key);
    if (keyDown && !g_keyWasDown)
    {
        g_on = !g_on;
        LOG("AutoAttack: {}", g_on ? "on" : "off");
        MessageBeep(g_on ? MB_ICONASTERISK : MB_ICONEXCLAMATION);
    }
    g_keyWasDown = keyDown;

    if (foreground && playerClicks())
    {
        g_quietUntil = now + static_cast<DWORD>(Config::autoAttack.pauseMs);
        g_target = 0;
    }
    if (!g_on || !foreground || Controller::drives() || !Controller::worldRunning() || !heroAlive() ||
        static_cast<int>(now - g_quietUntil) < 0)
    {
        g_target = 0;
        return;
    }

    AimAssist::Target t;
    if (!chooseTarget(t))
    {
        g_target = 0;
        return;
    }
    const int repeat = Config::autoAttack.repeatMs;
    if (t.id == g_target && (repeat <= 0 || static_cast<int>(now - g_sentAt) < repeat))
    {
        return;
    }
    if (attack(t.id))
    {
        if (t.id != g_target)
        {
            LOG("AutoAttack: target {:x}", t.id);
        }
        g_target = t.id;
        g_sentAt = now;
    }
}
