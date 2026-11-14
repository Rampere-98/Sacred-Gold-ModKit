#include "game/auto_potion.h"
#include "game/act.h"
#include "game/act.h"
#include "config/auto_potion.h"
#include "game/combat_state.h"
#include "game/focus.h"
#include "config/auto_attack.h"
#include "game/frame_hooks.h"
#include "game/modkit_menu.h"
#include "game/ui_nav.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"
#include "sacred/ui.h"
#include "sig.h"

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
    // Less than this gain over the measure is no potion: the smallest heals far more.
    constexpr int kMinPotionHeal = 8;
    constexpr int kMinPotionPercent = 6;

    int g_heal = 0;                 // health one potion heals, learned; 0 while unknown
    DWORD g_drankAt = 0;            // when the last potion was drunk, 0 for none
    int g_healthBefore = 0;         // the health just before it
    int g_bestAfter = 0;            // the highest health since
    int g_worstAfter = 0;           // the lowest health since (blows taken while it took effect)
    int g_maxBefore = 0;            // the maximum health when it was drunk
    uint32_t g_heroId = 0;          // the hero g_heal was learned for
    bool g_measuring = false;
    DWORD g_noneLeftUntil = 0;
    DWORD g_lastTry = 0;            // the last drink tried
    // With none left, an emergency looks again this often.
    constexpr DWORD kEmergencyRetryMs = 3000;
    uintptr_t g_usePotion = 0;

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

    // usePotion (ENG 00612FA0): thiscall on the engine (type, toHirelings) -> whether a potion was drunk; type 1 is
    // healing, what Space drinks (the key switch at ENG 006186EE calls it). Resolved on first use; without it, Space.
    using UsePotion = uint8_t(__fastcall*)(cEngine* self, void* edx, int type, int toHirelings);
    const Sig::Entry kSigs[] = {
        {"usePotion", "8B 0D ?? ?? ?? ?? 83 EC 08 53 55 56 57 E8 ?? ?? ?? ?? 8B F8 85 FF 0F 84 ?? ?? ?? ?? 8B 47 0C", 0,
            Sig::Take::Match, &g_usePotion},
    };

    // Drinks a healing potion; false if the game said none was drunk (none left). With Space there is no answer.
    bool drink()
    {
        cEngine* engine = FrameHooks::engine();
        if (g_usePotion && engine)
        {
            return reinterpret_cast<UsePotion>(g_usePotion)(engine, nullptr, 1, 0) != 0;
        }
        Inject::key(VK_SPACE, true);
        Inject::key(VK_SPACE, false);
        return true;
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
        if (CombatState::noPotionsAt.load())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "No quedan pociones de curación.");
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
    std::call_once(menu, [] {
        ModkitMenu::add({"Pociones automáticas", &drawMenu, &saveMenu});
        // The game's usePotion tells whether a potion was drunk; without it, Space (no answer).
        if (Sig::resolve(kSigs) != 0)
        {
            g_usePotion = 0;
        }
        LOG("AutoPotion: drinks through {}", g_usePotion ? "the game's usePotion" : "the Space key");
    });
    // Menus present from the UI thread, the game from the render thread; around loading both may.
    static std::mutex frameLock;
    const std::unique_lock lock(frameLock, std::try_to_lock);
    if (!lock || !Config::autoPotion.enabled || !Act::automatic(Act::Kind::LifePotion))
    {
        return;
    }
    cObjectManager* objects = cObjectManager::instance();
    cCreature* hero = objects ? objects->hero() : nullptr;
    if (!hero || hero->maxHealth <= 0 || !(Focus::foreground() || Config::autoAttack.background) || !worldRunning())
    {
        return;
    }
    const DWORD now = GetTickCount();
    const int health = hero->health;
    const int maximum = hero->maxHealth;
    if (hero->id != g_heroId)
    {
        // Another hero (a game loaded): what a potion heals is learned again.
        g_heroId = hero->id;
        g_heal = 0;
        g_measuring = false;
    }
    if (health <= 0)
    {
        g_measuring = false;
        return;
    }

    // Learn what the last potion healed.
    if (g_measuring)
    {
        g_bestAfter = std::max(g_bestAfter, health);
        g_worstAfter = std::min(g_worstAfter, health);
        if (static_cast<int>(now - g_drankAt) < static_cast<int>(kMeasureMs))
        {
            return;
        }
        g_measuring = false;
        const int gained = g_bestAfter - g_healthBefore;
        // Hurt while it took effect: blows ate part of the healing, so the gain only says the potion heals at least
        // that much.
        const bool hurt = g_worstAfter < g_healthBefore;
        if (maximum != g_maxBefore)
        {
            // The maximum changed meanwhile (a level up, a game loaded, an item put on): the gain is no potion's.
            LOG("AutoPotion: the maximum health changed ({} -> {}) while measuring, nothing learned", g_maxBefore,
                maximum);
        }
        else if (gained >= std::max(kMinPotionHeal, maximum * kMinPotionPercent / 100))
        {
            // At full health the potion heals no further: only a gain short of the maximum, taken without blows, is
            // its full amount; a larger gain always is a better estimate.
            if (gained > g_heal || (g_bestAfter < maximum && !hurt))
            {
                g_heal = gained;
            }
            LOG("AutoPotion: healed {} ({} -> {} of {}{}), potion heals {}", gained, g_healthBefore, g_bestAfter,
                maximum, hurt ? ", hurt meanwhile" : "", g_heal);
        }
        else if (g_usePotion)
        {
            // The game said a potion was drunk: the blows ate its healing.
            LOG("AutoPotion: the potion was drunk but the gain was {} ({})", gained,
                hurt ? "hurt meanwhile" : "nothing seen");
        }
        else
        {
            g_noneLeftUntil = now + kNoneLeftMs;
            CombatState::noPotionsAt = now;     // the auto attack's retreat needs to know
            LOG("AutoPotion: the potion healed nothing ({} is regeneration; none left?), next try in {} s", gained,
                kNoneLeftMs / 1000);
        }
    }

    if (g_drankAt && static_cast<int>(now - g_drankAt) < Config::autoPotion.intervalMs)
    {
        return;
    }
    const int missing = maximum - health;
    const DWORD wanted = CombatState::drinkWantedAt.load();
    const bool emergency = health * 100 <= maximum * Config::autoPotion.emergencyPercent ||
                           (wanted && now - wanted < 2000 && health * 100 < maximum * 70);
    // Never waiting for more than most of the maximum missing (a heal learned on a stronger hero, or larger than now).
    const bool due = g_heal > 0 ? missing >= std::min(g_heal, maximum * 60 / 100)
                                : missing * 100 >= maximum * Config::autoPotion.firstPercent;
    if (!emergency && !due)
    {
        return;
    }
    // None left lately: no new try for a while, but in an emergency a look every few seconds (the game's usePotion
    // tells at once whether one was drunk, and a potion picked up meanwhile counts).
    if (g_noneLeftUntil && static_cast<int>(now - g_noneLeftUntil) < 0 &&
        (!emergency || !g_usePotion || static_cast<int>(now - g_lastTry) < static_cast<int>(kEmergencyRetryMs)))
    {
        return;
    }
    // A combat art's effect is being measured: a potion now would pass for its healing. Not when it is urgent.
    const DWORD measuring = CombatState::measuringUntil.load();
    if (!emergency && measuring && static_cast<int>(now - measuring) < 0)
    {
        return;
    }
    g_lastTry = now;
    if (!drink())
    {
        g_noneLeftUntil = now + kNoneLeftMs;
        if (!CombatState::noPotionsAt.load())
        {
            LOG("AutoPotion: no healing potion left (at {} of {}), next look in {} s{}", health, maximum,
                kNoneLeftMs / 1000, emergency ? ", every few seconds while in danger" : "");
        }
        CombatState::noPotionsAt = now;
        return;
    }
    CombatState::noPotionsAt = 0;
    CombatState::potionAt = now;
    LOG("AutoPotion: drinks at {} of {} ({} missing, potion heals {}{})", health, maximum, missing, g_heal,
        emergency ? ", emergency" : "");
    g_drankAt = now;
    g_healthBefore = health;
    g_bestAfter = health;
    g_worstAfter = health;
    g_maxBefore = maximum;
    g_measuring = true;
}
