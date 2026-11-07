#include "game/auto_potion.h"
#include "config/auto_potion.h"
#include "game/focus.h"
#include "game/frame_hooks.h"
#include "game/modkit_menu.h"
#include "game/ui_nav.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"
#include "sacred/ui.h"

#include <imgui.h>
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <string>

using namespace Sacred;

namespace
{
    // How long after a potion its healing is measured (the highest health reached over the drink).
    constexpr DWORD kMeasureMs = 1500;
    // A potion that healed nothing: none left (or the key did nothing); no new try for this long.
    constexpr DWORD kNoneLeftMs = 15000;

    int g_heal = 0;                 // health one potion heals, learned; 0 while unknown
    DWORD g_drankAt = 0;            // when the last potion was drunk, 0 for none
    int g_healthBefore = 0;         // the health just before it
    int g_bestAfter = 0;            // the highest health since
    bool g_measuring = false;
    DWORD g_noneLeftUntil = 0;

    // In the game world with nothing in front of it that Space would go to.
    bool worldRunning()
    {
        cUI_Manager* manager = cUI_Manager::instance();
        if (!manager)
        {
            return false;
        }
        const uint32_t flags = manager->flags;
        if (!(flags & cUI_Manager::inGame) || (flags & (cUI_Manager::inMenus | cUI_Manager::cinematic)))
        {
            return false;
        }
        if (cEngine* engine = FrameHooks::engine(); !engine || (engine->flags & cEngine::loading))
        {
            return false;
        }
        const cUI_Window2* console = manager->window(cUI_Manager::Window::console);
        if (console && console->visible())
        {
            return false;   // the chat line: Space would type
        }
        return !UiNav::modal() && !UiNav::npcDialogOpen();
    }

    void drink()
    {
        Inject::key(VK_SPACE, true);
        Inject::key(VK_SPACE, false);
    }

    void drawMenu()
    {
        Config::AutoPotion& c = Config::autoPotion;
        ImGui::Checkbox("Activo", &c.enabled);
        ImGui::SliderInt("Primera poción: falta (%)", &c.firstPercent, 5, 95);
        ImGui::SetItemTooltip("Hasta saber cuánto cura una poción, bebe cuando falta este porcentaje de vida.");
        ImGui::SliderInt("Emergencia: vida (%)", &c.emergencyPercent, 0, 90);
        ImGui::SetItemTooltip("Con esta vida o menos bebe siempre.");
        ImGui::SliderInt("Entre pociones (ms)", &c.intervalMs, 250, 10000);
        if (g_heal > 0)
        {
            ImGui::TextDisabled("Una poción cura %d de vida (medido).", g_heal);
        }
    }

    void saveMenu(const std::wstring& ini)
    {
        const Config::AutoPotion& c = Config::autoPotion;
        ModkitMenu::write(ini, L"AutoPotion", L"Enabled", c.enabled);
        ModkitMenu::write(ini, L"AutoPotion", L"FirstPercent", c.firstPercent);
        ModkitMenu::write(ini, L"AutoPotion", L"EmergencyPercent", c.emergencyPercent);
        ModkitMenu::write(ini, L"AutoPotion", L"Interval", c.intervalMs);
    }
}

void AutoPotion::onFrame()
{
    // Its menu section, once (this mod has no install step).
    static std::once_flag menu;
    std::call_once(menu, [] { ModkitMenu::add({"Pociones automáticas", &drawMenu, &saveMenu}); });
    // Menus present from the UI thread, the game from the render thread; around loading both may.
    static std::mutex frameLock;
    const std::unique_lock lock(frameLock, std::try_to_lock);
    if (!lock || !Config::autoPotion.enabled)
    {
        return;
    }
    cObjectManager* objects = cObjectManager::instance();
    cCreature* hero = objects ? objects->hero() : nullptr;
    if (!hero || hero->maxHealth <= 0 || !Focus::foreground() || !worldRunning())
    {
        return;
    }
    const DWORD now = GetTickCount();
    const int health = hero->health;
    const int maximum = hero->maxHealth;
    if (health <= 0)
    {
        g_measuring = false;
        return;
    }

    // Learn what the last potion healed.
    if (g_measuring)
    {
        g_bestAfter = std::max(g_bestAfter, health);
        if (static_cast<int>(now - g_drankAt) < static_cast<int>(kMeasureMs))
        {
            return;
        }
        g_measuring = false;
        const int gained = g_bestAfter - g_healthBefore;
        if (gained > 0)
        {
            // At full health the potion heals no further: only a gain short of the maximum is its full amount.
            if (g_bestAfter < maximum || gained > g_heal)
            {
                g_heal = gained;
            }
            LOG("AutoPotion: healed {} ({} -> {} of {}), potion heals {}", gained, g_healthBefore, g_bestAfter,
                maximum, g_heal);
        }
        else
        {
            g_noneLeftUntil = now + kNoneLeftMs;
            LOG("AutoPotion: the potion healed nothing (none left?), next try in {} s", kNoneLeftMs / 1000);
        }
    }

    if (static_cast<int>(now - g_noneLeftUntil) < 0 ||
        (g_drankAt && static_cast<int>(now - g_drankAt) < Config::autoPotion.intervalMs))
    {
        return;
    }
    const int missing = maximum - health;
    const bool emergency = health * 100 <= maximum * Config::autoPotion.emergencyPercent;
    const bool due = g_heal > 0 ? missing >= g_heal : missing * 100 >= maximum * Config::autoPotion.firstPercent;
    if (!emergency && !due)
    {
        return;
    }
    LOG("AutoPotion: drinks at {} of {} ({} missing, potion heals {})", health, maximum, missing, g_heal);
    drink();
    g_drankAt = now;
    g_healthBefore = health;
    g_bestAfter = health;
    g_measuring = true;
}
