#include "game/agent_save.h"
#include "game/combat_learn.h"
#include "game/frame_hooks.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/engine.h"
#include "sacred/object.h"
#include "sacred/ui.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace Sacred;

namespace
{
    // Manolito, block S (AGENT_ADDRESSES «Partidas guardadas (bloque S)»): a cEvent (vtable 0x890B38) with +4 0x0F,
    // +8 2 save / 1 load, +0xC the slot, +0x20 the name, sent through 0x808E50()->0x8092F0(&ev, 0, 0) (ret 0xC); a save
    // only when 0x756F40()->0x75ADB0(1) allows it; nothing of it with a network game ([0x182EBEC]).
    constexpr uintptr_t kEventVtable = 0x890B38;
    constexpr uintptr_t kDispatcher = 0x808E50;
    constexpr uintptr_t kSend = 0x8092F0;
    constexpr uintptr_t kUiGetter = 0x756F40;
    constexpr uintptr_t kCanSave = 0x75ADB0;
    constexpr uintptr_t kNetwork = 0x182EBEC;
    // The save function and the free slot finder, to tell this is the exe those addresses are of.
    constexpr uintptr_t kSaveFn = 0x619BF0;
    constexpr uintptr_t kFreeSlotFn = 0x619B80;

    int g_slot = -1;            // -1 not looked at yet, 0 none (not an agent's copy)
    std::wstring g_path;        // its file
    char g_name[32] = {};
    bool g_loadedOnce = false;
    DWORD g_inGameSince = 0;
    bool g_saveWanted = false;
    bool g_diedSince = false;   // died since the last save or load: no save
    int32_t g_lastHealth = 1;
    DWORD g_actionAt = 0;       // the last save or load asked for
    int g_sub = 0;              // the event's kind for the window's thread
    alignas(8) uint8_t g_event[0x40] = {};

    bool exeMatches()
    {
        const auto* save = reinterpret_cast<const uint8_t*>(kSaveFn);
        const auto* free = reinterpret_cast<const uint8_t*>(kFreeSlotFn);
        static const uint8_t kSaveHead[] = {0x6A, 0xFF, 0x68};
        static const uint8_t kSaveTail[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50};
        static const uint8_t kFree[] = {0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x56, 0xBE, 0x01, 0x00, 0x00, 0x00};
        return std::memcmp(save, kSaveHead, sizeof(kSaveHead)) == 0 &&
               std::memcmp(save + 7, kSaveTail, sizeof(kSaveTail)) == 0 &&
               std::memcmp(free, kFree, sizeof(kFree)) == 0;
    }

    // The copy's slot by its folder: ...\SacredA<n>\Sacred.exe -> 9 + n; anything else none.
    void findSlot()
    {
        g_slot = 0;
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        const size_t cut = path.find_last_of(L"\\/");
        if (cut == std::wstring::npos)
        {
            return;
        }
        const std::wstring dir = path.substr(0, cut);
        const size_t nameAt = dir.find_last_of(L"\\/");
        const std::wstring name = nameAt == std::wstring::npos ? dir : dir.substr(nameAt + 1);
        if (name.size() != 8 || name.compare(0, 7, L"SacredA") != 0 || name[7] < L'1' || name[7] > L'8')
        {
            LOG("AgentSave: not an agent's copy ({}): no saves", std::string(name.begin(), name.end()));
            return;
        }
        if (!exeMatches())
        {
            LOG("AgentSave: the save functions are not where block S has them: no saves");
            return;
        }
        // Only where the copy's SacredBild.ini asks for it ([AgentSave] Enabled=1), while it is tried on one copy.
        const std::wstring ini = dir + L"\\SacredBild.ini";
        if (!GetPrivateProfileIntW(L"AgentSave", L"Enabled", 0, ini.c_str()))
        {
            LOG("AgentSave: off for this copy ([AgentSave] Enabled=0)");
            return;
        }
        const int n = name[7] - L'0';
        g_slot = 9 + n;
        wchar_t file[32];
        swprintf(file, 32, L"\\save\\GAME%02d.PAK", g_slot);
        g_path = dir + file;
        std::snprintf(g_name, sizeof(g_name), "Agente A%d", n);
        LOG("AgentSave: the copy's slot is GAME{:02d}", g_slot);
    }

    bool slotFileExists()
    {
        const DWORD attributes = GetFileAttributesW(g_path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    // On the window's thread (the game's own UI sends these there).
    void sendEvent(void*)
    {
        std::memset(g_event, 0, sizeof(g_event));
        *reinterpret_cast<uintptr_t*>(g_event) = kEventVtable;
        *reinterpret_cast<uint32_t*>(g_event + 4) = 0x0F;
        *reinterpret_cast<uint32_t*>(g_event + 8) = static_cast<uint32_t>(g_sub);
        *reinterpret_cast<uint32_t*>(g_event + 0xC) = static_cast<uint32_t>(g_slot);
        *reinterpret_cast<const char**>(g_event + 0x20) = g_name;
        void* dispatcher = reinterpret_cast<void*(__cdecl*)()>(kDispatcher)();
        if (!dispatcher)
        {
            LOG("AgentSave: no event dispatcher: nothing sent");
            return;
        }
        reinterpret_cast<void(__fastcall*)(void*, void*, void*, int, int)>(kSend)(dispatcher, nullptr, g_event, 0, 0);
    }

    bool canSave()
    {
        void* ui = reinterpret_cast<void*(__cdecl*)()>(kUiGetter)();
        return ui && (reinterpret_cast<uint32_t(__fastcall*)(void*, void*, int)>(kCanSave)(ui, nullptr, 1) & 0xFF) != 0;
    }

    void load(DWORD now, const char* why)
    {
        LOG("AgentSave: {}: loading GAME{:02d}", why, g_slot);
        g_sub = 1;
        g_actionAt = now;
        g_saveWanted = false;
        g_diedSince = false;
        Inject::call(&sendEvent, nullptr);
    }
}

void AgentSave::questDone()
{
    g_saveWanted = true;
}

void AgentSave::onFrame()
{
    if (g_slot < 0)
    {
        findSlot();
    }
    if (g_slot <= 0)
    {
        return;
    }
    const DWORD now = GetTickCount();
    cUI_Manager* ui = cUI_Manager::instance();
    cEngine* engine = FrameHooks::engine();
    const bool loading = engine && (engine->flags & cEngine::loading);
    const bool inGame = ui && (ui->flags & cUI_Manager::inGame) && !loading;
    cObjectManager* objects = cObjectManager::instance();
    cCreature* hero = objects ? objects->hero() : nullptr;
    if (!inGame || !hero || *reinterpret_cast<const uint32_t*>(kNetwork))
    {
        g_inGameSince = 0;
        return;
    }
    if (!g_inGameSince)
    {
        g_inGameSince = now;
        g_lastHealth = hero->health;
    }
    // The copy's own game, once in a game at start (a new one begun by AutoStart, then the saved one loaded over it).
    if (!g_loadedOnce)
    {
        if (now - g_inGameSince < 15000)
        {
            return;
        }
        g_loadedOnce = true;
        if (slotFileExists())
        {
            load(now, "the copy's own game");
            return;
        }
        LOG("AgentSave: no GAME{:02d} yet: this new game is the copy's", g_slot);
    }
    if (g_actionAt && now - g_actionAt < 15000)
    {
        g_lastHealth = hero->health;
        return;
    }
    // Died: its last save loaded (the player: «los que mueren tienen que cargar la partida anterior»).
    const int32_t health = hero->health;
    const bool died = health <= 0 && g_lastHealth > 0;
    g_lastHealth = health;
    if (died)
    {
        if (slotFileExists())
        {
            load(now, "the hero died");
        }
        else
        {
            g_diedSince = true;
            LOG("AgentSave: the hero died with no save yet: no save until a quest done after it");
        }
        return;
    }
    // A quest done: saved once the game allows it, with no fight about and the hero alive, unless it died since.
    if (!g_saveWanted)
    {
        return;
    }
    if (g_diedSince)
    {
        g_saveWanted = false;
        g_diedSince = false;    // the quests from here on count again
        LOG("AgentSave: a quest done, but the hero died since the last save: not saved");
        return;
    }
    if (health <= 0 || CombatLearn::forecast().foes > 0 || !canSave())
    {
        return;
    }
    // The last save kept beside it first (a bad game is not carried on with nothing to go back to).
    const std::wstring backup = g_path.substr(0, g_path.size() - 4) + L".BAK";
    if (slotFileExists())
    {
        CopyFileW(g_path.c_str(), backup.c_str(), FALSE);
    }
    LOG("AgentSave: a quest done: saving GAME{:02d} ({} of {} health)", g_slot, health, hero->maxHealth);
    g_saveWanted = false;
    g_sub = 2;
    g_actionAt = now;
    Inject::call(&sendEvent, nullptr);
}
