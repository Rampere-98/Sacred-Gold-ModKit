#include "game/auto_start.h"
#include "game/controller.h"
#include "game/frame_hooks.h"
#include "game/resolution.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/ui.h"

#include <cmath>
#include <string>

using namespace Sacred;

namespace
{
    // The class rotation and where each class stands on the selection screen, in the menus' 1024x768 layout (measured
    // on the 5120x1440 screen, 2026-11-10). The campaign entry, Start, and the quit dialog's Cancel too.
    struct Spot
    {
        const char* name;
        int x, y;
    };
    constexpr Spot kClasses[] = {
        {"Seraphim", 216, 383}, {"Gladiator", 96, 470},  {"Battle Mage", 731, 401}, {"Wood Elf", 623, 331},
        {"Dark Elf", 810, 348}, {"Vampiress", 888, 488}, {"Dwarf", 522, 401},       {"Daemon", 373, 383},
    };
    constexpr int kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);
    constexpr Spot kCampaign = {"campaign", 510, 284};
    constexpr Spot kStart = {"start", 606, 714};

    bool g_enabled = true;
    bool g_rotate = true;       // [AutoStart] Rotate=0: always the same class (one agent per class, each its own copy)
    int g_nextClass = 0;
    DWORD g_menuSince = 0;      // in the start menu since then (0: not in it)
    int g_step = -1;            // the sequence's step (-1: none under way)
    DWORD g_stepAt = 0;
    int g_tries = 0;

    std::wstring iniPath()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        return path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild.ini";
    }

    // A point of the menus' 1024x768 layout in client pixels: the menus are drawn 4:3, centred, full height.
    void click(const Spot& s)
    {
        const float h = static_cast<float>(Resolution::height()), w = static_cast<float>(Resolution::width());
        const float k = h / 768.0f;
        const float offset = (w - h * 4.0f / 3.0f) * 0.5f;
        Controller::clickAt(static_cast<int>(std::lround(offset + s.x * k)), static_cast<int>(std::lround(s.y * k)));
    }

    void tapKey(int vk)
    {
        Inject::key(vk, true);
        Inject::key(vk, false);
    }

    void install()
    {
        const std::wstring ini = iniPath();
        g_enabled = GetPrivateProfileIntW(L"AutoStart", L"Enabled", 1, ini.c_str()) != 0;
        g_nextClass = static_cast<int>(GetPrivateProfileIntW(L"AutoStart", L"NextClass", 0, ini.c_str())) % kClassCount;
        g_rotate = GetPrivateProfileIntW(L"AutoStart", L"Rotate", 1, ini.c_str()) != 0;
        LOG("AutoStart: {} (next class {})", g_enabled ? "on" : "off", kClasses[g_nextClass].name);
    }
}

void AutoStart::onFrame()
{
    static bool installed = false;
    if (!installed)
    {
        installed = true;
        install();
    }
    if (!g_enabled)
    {
        return;
    }
    const DWORD now = GetTickCount();
    cUI_Manager* ui = cUI_Manager::instance();
    cEngine* engine = FrameHooks::engine();
    const bool loading = engine && (engine->flags & cEngine::loading);
    const bool inGame = ui && (ui->flags & cUI_Manager::inGame);
    const bool inMenus = ui && (ui->flags & cUI_Manager::inMenus) && !inGame && !loading;
    if (g_step >= 0 && (inGame || loading))
    {
        LOG("AutoStart: the game began ({})", kClasses[g_nextClass].name);
        if (!g_rotate)
        {
            g_step = -1;
            g_tries = 0;
            g_menuSince = 0;
            return;
        }
        g_nextClass = (g_nextClass + 1) % kClassCount;
        WritePrivateProfileStringW(L"AutoStart", L"NextClass", std::to_wstring(g_nextClass).c_str(), iniPath().c_str());
        g_step = -1;
        g_tries = 0;
        g_menuSince = 0;
        return;
    }
    // After Start, films (the class's, the act's) play outside the menus: the sequence goes on (Esc) there too.
    if (!inMenus && g_step < 3)
    {
        g_menuSince = 0;
        if (g_step >= 0 && !inGame)
        {
            g_step = -1;    // left the menus some other way (the player): nothing more
        }
        return;
    }
    if (!g_menuSince)
    {
        g_menuSince = now;
    }
    // 20 s in the start menu with nothing going on: a new game, as the player would begin it.
    if (g_step < 0)
    {
        if (now - g_menuSince < 20000)
        {
            return;
        }
        if (++g_tries > 5)
        {
            static bool told = false;
            if (!told)
            {
                told = true;
                LOG("AutoStart: 5 tries and no game began: left to the player");
            }
            return;
        }
        LOG("AutoStart: {} s in the start menu: a new game with the {} (try {})", (now - g_menuSince) / 1000,
            kClasses[g_nextClass].name, g_tries);
        g_step = 0;
        g_stepAt = now;
    }
    if (static_cast<int>(now - g_stepAt) < 0)
    {
        return;
    }
    switch (g_step)
    {
    case 0:
        // A message box over the menu (the quit question): Esc closes it.
        if (ui->dialog && ui->dialog->visible())
        {
            tapKey(VK_ESCAPE);
            g_stepAt = now + 1500;
            return;
        }
        click(kCampaign);
        g_step = 1;
        g_stepAt = now + 3000;
        break;
    case 1:
        click(kClasses[g_nextClass]);
        g_step = 2;
        g_stepAt = now + 2000;
        break;
    case 2:
        click(kStart);
        g_step = 3;
        g_stepAt = now + 5000;
        break;
    default:
        // Still in the menus long after Start: films are skipped with Esc; after 40 s the sequence begins again.
        tapKey(VK_ESCAPE);
        g_stepAt = now + 3000;
        if (++g_step > 16)
        {
            LOG("AutoStart: no game after Start, again");
            g_step = -1;
            g_menuSince = now - 15000;
        }
        break;
    }
}
