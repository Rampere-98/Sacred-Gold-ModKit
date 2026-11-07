#include "game/auto_attack.h"
#include "config/auto_attack.h"
#include "config/controller.h"
#include "game/aim_assist.h"
#include "game/controller.h"
#include "game/focus.h"
#include "input/inject.h"
#include "log.h"

#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <mutex>

namespace
{
    bool g_on = false;
    bool g_keyWasDown = false;
    bool g_down = false;            // the left button is held by us
    uint32_t g_target = 0;          // the enemy we attack, 0 for none
    DWORD g_quietUntil = 0;         // the player's own input: no auto attack before this tick
    POINT g_lastCursor{};
    bool g_haveCursor = false;

    // The physical state: SacredBild's GetAsyncKeyState is not the game's patched import, so injected input and
    // the controller never show here.
    bool physicallyDown(int vk)
    {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }

    // A mouse button held, or the system cursor moved a few pixels since the last frame.
    bool playerUsesMouse()
    {
        bool used = physicallyDown(VK_LBUTTON) || physicallyDown(VK_RBUTTON) || physicallyDown(VK_MBUTTON);
        POINT p;
        if (GetCursorPos(&p))
        {
            if (g_haveCursor && (std::abs(p.x - g_lastCursor.x) > 3 || std::abs(p.y - g_lastCursor.y) > 3))
            {
                used = true;
            }
            g_lastCursor = p;
            g_haveCursor = true;
        }
        return used;
    }

    // Lets go of the button and the target; the world pick is the game's own again.
    void release()
    {
        if (g_down)
        {
            // The player pressing the button themselves: only drop our held state, so our button-up message does
            // not cut their own click short.
            if (physicallyDown(VK_LBUTTON))
            {
                Inject::hold(VK_LBUTTON, false);
            }
            else
            {
                Inject::button(VK_LBUTTON, false);
            }
            g_down = false;
        }
        if (g_target)
        {
            AimAssist::pickGame();
            g_target = 0;
        }
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
}

void AutoAttack::install()
{
    // The controller installs the world pick hook; without it, we do.
    if (!Config::controller.enabled)
    {
        AimAssist::install();
    }
    g_on = Config::autoAttack.enabled;
    LOG("AutoAttack: {} (key 0x{:x}, range {} px, pause {} ms)", g_on ? "on" : "off", Config::autoAttack.key,
        Config::autoAttack.range, Config::autoAttack.pauseMs);
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

    if (playerUsesMouse())
    {
        g_quietUntil = now + static_cast<DWORD>(Config::autoAttack.pauseMs);
    }
    if (!g_on || !foreground || Controller::drives() || !Controller::inGameWorld() || !heroAlive() ||
        static_cast<int>(now - g_quietUntil) < 0)
    {
        release();
        return;
    }

    AimAssist::Target t;
    if (!chooseTarget(t))
    {
        release();
        return;
    }
    if (t.id != g_target)
    {
        // A new target: the game keeps attacking what the button went down on, so press again for it.
        release();
        g_target = t.id;
        AimAssist::pickTarget(g_target);
        Controller::pointAt(static_cast<int>(t.x), static_cast<int>(t.y), 250);
        return;
    }
    AimAssist::pickTarget(g_target);
    // The game's cursor on the enemy, so the click never lands on the HUD under the mouse.
    Controller::pointAt(static_cast<int>(t.x), static_cast<int>(t.y), 250);
    if (!g_down)
    {
        Inject::mouseMove();
        Inject::button(VK_LBUTTON, true);
        g_down = true;
    }
}
