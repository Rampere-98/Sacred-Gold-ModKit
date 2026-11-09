#include "game/hero_build.h"
#include "game/modkit_menu.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/object.h"
#include "sacred/ui.h"
#include "sig.h"

#include <imgui.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

using namespace Sacred;

namespace
{
    struct Settings
    {
        bool attributes = true;
        bool skills = true;
        bool learn = true;
        bool bar = true;
    };
    Settings g_settings;

    // stats = creature + 0x3A8 (seen live, 2026-11-09): +8 class; +0x1C u8[6] points put in each attribute, +0x23
    // attribute points to spend; +0x24 u8[8] skill id per slot, +0x2C u8[8] its base level; +0x42 u16 skill points to
    // spend; +0x44 u16 new skill slots to fill; +0x56 u16 the hero's level.
    constexpr size_t kStats = 0x3A8;

    uintptr_t g_spendAttr = 0, g_addSkillLevel = 0, g_learnable = 0, g_learnSkill = 0, g_setSlot = 0;
    const Sig::Entry kSigs[] = {
        {"spendAttr", "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 83 EC 7C 53 56 57 8B ?? ?? ?? ?? 00 00 83 FF 07",
            0, Sig::Take::Match, &g_spendAttr},
        {"addSkillLevel", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 8B ?? ?? ?? ?? 00 00 53 56 57 8B F9",
            0, Sig::Take::Match, &g_addSkillLevel},
        {"learnableSkills", "83 EC 10 56 8B F1 8B 0D ?? ?? ?? ??", 0, Sig::Take::Match, &g_learnable},
        {"learnSkill", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 53 56 57 8B F9 33 DB",
            0, Sig::Take::Match, &g_learnSkill},
        // The taskbar's art slot setter, as a drop onto a slot (ENG 005D94C0): ecx = the player's data, (slot, kind,
        // w2, const u32 data[4]); it also tells the server.
        {"setTaskbarSlot", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 6C 8B 54 24 7C",
            0, Sig::Take::Match, &g_setSlot},
    };
    bool g_ready = false;

    using SpendAttr = void(__fastcall*)(void* stats, void*, uint32_t attr);
    using AddSkillLevel = void(__fastcall*)(void* stats, void*, uint32_t slot, int16_t delta, bool pay);
    using Learnable = int(__fastcall*)(void* stats, void*, uint8_t* out);
    using LearnSkill = void(__fastcall*)(void* stats, void*, uint8_t id);
    using SetSlot = void(__fastcall*)(void* player, void*, uint32_t slot, uint32_t kind, uint32_t w2, const uint32_t* data);

    // The players' data (32 of 0x1180 bytes at [ENG 00AD4448], the hero's by his id's low word): the art slots (keys
    // 6-0) as 20-byte entries at +0xC90, two per slot (the second for the alternate bar, +0x1162 bit 1): u16 kind (2
    // spell, 3 weapon art, 4 combo), u16, u32 data[4] (the art, a combo's 0x200 + index).
    constexpr uintptr_t kPlayers = 0xAD4448;
    constexpr size_t kPlayerSize = 0x1180, kArtEntries = 0xC90, kArtEntrySize = 20, kAltBar = 0x1162;
    // The taskbar's slot count shown (more come with the levels, at most 5).
    constexpr size_t kTaskbarSlots = 0x15C;

    // The learned arts (stats +0x102 begin, +0x106 end; 22 bytes each: u32 kind 1 spell / 2 weapon art, u16 id, u16
    // level) and the combos (stats +0x10E: 4 records of 38 bytes, u16 id 0x200 + i, u32 arts[4]).
    struct Candidate
    {
        uint16_t kind = 0;
        uint32_t data = 0;
        int worth = 0;
    };

    // One art not yet on the bar into the first free slot the hero has: the combos first (several arts at once), then
    // the arts by their level. A slot the player filled stays as it is. True when something was placed.
    bool fillBar(cCreature* hero, uint8_t* st)
    {
        cUI_Manager* manager = cUI_Manager::instance();
        cUI_Taskbar2* taskbar = manager ? manager->taskbar() : nullptr;
        const uint8_t* players = *reinterpret_cast<uint8_t* const*>(kPlayers);
        const uint32_t index = hero->id & 0xFFFF;
        if (!taskbar || !players || index >= 0x20)
        {
            return false;
        }
        const int shown = std::clamp<int>(*reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(taskbar) +
            kTaskbarSlots), 0, cUI_Taskbar2::slotCount);
        uint8_t* player = const_cast<uint8_t*>(players) + index * kPlayerSize;
        const int alt = (player[kAltBar] & 2) ? 1 : 0;
        auto entry = [&](int slot) { return player + kArtEntries + static_cast<size_t>(slot * 2 + alt) * kArtEntrySize; };
        int freeSlot = -1;
        for (int slot = 0; slot < shown && freeSlot < 0; ++slot)
        {
            if (!*reinterpret_cast<const uint16_t*>(entry(slot)))
            {
                freeSlot = slot;
            }
        }
        if (freeSlot < 0)
        {
            return false;
        }
        auto onBar = [&](uint32_t data) {
            for (int slot = 0; slot < cUI_Taskbar2::slotCount; ++slot)
            {
                const uint8_t* e = entry(slot);
                if (*reinterpret_cast<const uint16_t*>(e) && *reinterpret_cast<const uint32_t*>(e + 4) == data)
                {
                    return true;
                }
            }
            return false;
        };
        Candidate best;
        const uint8_t* combos = *reinterpret_cast<uint8_t* const*>(st + 0x10E);
        for (int i = 0; combos && i < 4; ++i)
        {
            const uint8_t* r = combos + i * 38;
            const uint16_t id = *reinterpret_cast<const uint16_t*>(r);
            if (id >= 0x200 && id < 0x204 && *reinterpret_cast<const uint32_t*>(r + 2) && !onBar(id))
            {
                best = {4, id, 1000 - i};
                break;
            }
        }
        const uint8_t* begin = *reinterpret_cast<uint8_t* const*>(st + 0x102);
        const uint8_t* end = *reinterpret_cast<uint8_t* const*>(st + 0x106);
        const bool combo = best.kind == 4;
        for (const uint8_t* a = begin; !combo && begin && end > begin && a + 22 <= end; a += 22)
        {
            const uint32_t kind = *reinterpret_cast<const uint32_t*>(a);
            const uint16_t id = *reinterpret_cast<const uint16_t*>(a + 4);
            const int level = *reinterpret_cast<const uint16_t*>(a + 6);
            if ((kind == 1 || kind == 2) && id && !onBar(id) && level > best.worth)
            {
                best = {static_cast<uint16_t>(kind + 1), id, level};
            }
        }
        if (!best.kind)
        {
            return false;
        }
        const uint32_t data[4] = {best.data, 0, 0, 0};
        LOG("HeroBuild: {} {:x} onto taskbar slot {} ({} shown)", best.kind == 4 ? "combo" : best.kind == 2 ? "spell" : "weapon art",
            best.data, freeSlot + 1, shown);
        reinterpret_cast<SetSlot>(g_setSlot)(player, nullptr, static_cast<uint32_t>(freeSlot), best.kind, 0, data);
        return true;
    }

    // The skill names (ENG 008EBA20: records of 0x44 bytes, u32 id and char[64]), for the plan below.
    const char* skillName(uint8_t id)
    {
        const auto* table = reinterpret_cast<const uint8_t*>(0x8EBA20);
        for (int i = 0; i < 64; ++i)
        {
            const uint8_t* r = table + i * 0x44;
            if (*reinterpret_cast<const uint32_t*>(r) == id)
            {
                return reinterpret_cast<const char*>(r + 4);
            }
        }
        return "";
    }

    bool named(uint8_t id, const char* part)
    {
        return std::strstr(skillName(id), part) != nullptr;
    }

    // The skills each class leans on, first the most (Manolito's table, docs/AGENT_ADDRESSES.md «Clases del héroe»;
    // ids are the game's SKILL_* numbers). 0 ends a list.
    const uint8_t kSkillPlan[10][7] = {
        {0},
        {1, 14, 12, 2, 25, 10, 0},      // seraphim: heavenly magic, magic lore, meditation
        {2, 25, 10, 11, 9, 0},          // gladiator: weapon lore, concentration, constitution, armour, parry
        {14, 12, 15, 17, 16, 18, 10},   // battle mage: magic lore, meditation, the four schools
        {2, 25, 13, 27, 26, 10, 0},     // dark elf: blade combat, traps, ballistics
        {7, 2, 25, 19, 8, 10, 0},       // wood elf: long range, moon magic, agility
        {2, 20, 28, 25, 10, 0},         // vampiress: vampirism, blood lust
        {2, 20, 28, 25, 10, 0},
        {2, 29, 31, 25, 10, 0},         // dwarf: weapon technique, dwarven lore
        {32, 14, 12, 2, 25, 10, 0},     // daemon: hell power
    };
    uint32_t g_class = 0;   // the hero's, set before the picks below

    // How much a skill is worth to the agent: its class's plan first, trade and riding last, the rest alike.
    float skillWeight(uint8_t id)
    {
        if (g_class < 10)
        {
            for (int i = 0; i < 7 && kSkillPlan[g_class][i]; ++i)
            {
                if (kSkillPlan[g_class][i] == id)
                {
                    return 3.0f - 0.25f * static_cast<float>(i);
                }
            }
        }
        if (named(id, "Handel") || named(id, "Reiten"))
        {
            return 0.25f;
        }
        if (named(id, "Konstitution") || named(id, "Konzentration") || named(id, "kunde") || named(id, "Kunde"))
        {
            return 1.5f;
        }
        return 1.0f;
    }

    // Attributes by the class (order of the character window, global.res 71321354..59: strength, endurance,
    // dexterity, physical regeneration, mental regeneration, charisma; no intelligence in Sacred): how much of each,
    // relative. Charisma only lowers prices and adds poison damage (GAME_GUIDE): the dark elf's poison only. Mental
    // regeneration recharges the arts. 1 seraphim, 2 gladiator, 3 battle mage, 4 dark elf, 5 wood elf, 6/7 vampiress,
    // 8 dwarf, 9 daemon.
    const int kAttrPlan[10][6] = {
        {2, 3, 2, 1, 1, 0},     // unknown
        {3, 3, 2, 1, 2, 0},     // seraphim
        {4, 3, 2, 1, 0, 0},     // gladiator
        {0, 3, 1, 0, 4, 0},     // battle mage
        {1, 3, 4, 1, 1, 1},     // dark elf
        {0, 3, 4, 1, 2, 0},     // wood elf
        {4, 3, 2, 1, 0, 0},     // vampiress
        {4, 3, 2, 1, 0, 0},     // vampiress (other form)
        {3, 3, 2, 1, 1, 0},     // dwarf
        {2, 3, 1, 1, 3, 0},     // daemon
    };

    std::atomic<bool> g_working{false};

    void work(void*)
    {
        cObjectManager* objects = cObjectManager::instance();
        cCreature* hero = objects ? objects->hero() : nullptr;
        if (!hero || hero->health <= 0)
        {
            g_working = false;
            return;
        }
        uint8_t* st = reinterpret_cast<uint8_t*>(hero) + kStats;
        const uint32_t cls = *reinterpret_cast<const uint32_t*>(st + 8);
        g_class = cls;
        const int level = *reinterpret_cast<const uint16_t*>(st + 0x56);
        // One point a look, as a player clicking "+".
        if (g_settings.attributes && st[0x23] > 0)
        {
            const int* plan = kAttrPlan[cls < 10 ? cls : 0];
            int best = -1;
            float bestShare = 0.0f;
            for (int i = 0; i < 6; ++i)
            {
                if (plan[i] <= 0)
                {
                    continue;
                }
                const float share = static_cast<float>(st[0x1C + i]) / static_cast<float>(plan[i]);
                if (best < 0 || share < bestShare)
                {
                    best = i;
                    bestShare = share;
                }
            }
            if (best >= 0)
            {
                LOG("HeroBuild: attribute point to attribute {} (had {}, {} left)", best, st[0x1C + best], st[0x23] - 1);
                reinterpret_cast<SpendAttr>(g_spendAttr)(st, nullptr, static_cast<uint32_t>(best));
            }
        }
        else if (g_settings.learn && *reinterpret_cast<const uint16_t*>(st + 0x44) > 0)
        {
            uint8_t offered[8] = {};
            const int count = std::clamp(reinterpret_cast<Learnable>(g_learnable)(st, nullptr, offered), 0, 8);
            int pick = -1;
            for (int i = 0; i < count; ++i)
            {
                if (offered[i] && (pick < 0 || skillWeight(offered[i]) > skillWeight(offered[pick])))
                {
                    pick = i;
                }
            }
            if (pick >= 0)
            {
                LOG("HeroBuild: new skill {} ({})", offered[pick], skillName(offered[pick]));
                reinterpret_cast<LearnSkill>(g_learnSkill)(st, nullptr, offered[pick]);
            }
        }
        else if (g_settings.bar && g_setSlot && fillBar(hero, st))
        {
        }
        else if (g_settings.skills && *reinterpret_cast<const uint16_t*>(st + 0x42) > 0)
        {
            // The skill furthest behind for its worth, never above the hero's level (the window's own limit).
            int best = -1;
            float bestShare = 0.0f;
            for (int slot = 0; slot < 8; ++slot)
            {
                const uint8_t id = st[0x24 + slot];
                const int base = st[0x2C + slot];
                if (!id || base >= level || base >= 255)
                {
                    continue;
                }
                const float share = static_cast<float>(base) / skillWeight(id);
                if (best < 0 || share < bestShare)
                {
                    best = slot;
                    bestShare = share;
                }
            }
            if (best >= 0)
            {
                LOG("HeroBuild: skill point to {} ({}) level {}", st[0x24 + best], skillName(st[0x24 + best]),
                    st[0x2C + best] + 1);
                reinterpret_cast<AddSkillLevel>(g_addSkillLevel)(st, nullptr, static_cast<uint32_t>(best), 1, true);
            }
        }
        g_working = false;
    }

    std::wstring iniPath()
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        return path.substr(0, path.find_last_of(L"\\/") + 1) + L"SacredBild.ini";
    }

    void drawMenu()
    {
        if (!g_ready)
        {
            ImGui::TextDisabled("No disponible en esta versión del juego.");
            return;
        }
        ImGui::Checkbox("Repartir puntos de atributo (según la clase)", &g_settings.attributes);
        ImGui::SetItemTooltip("Con el botón «+» del juego, uno a uno: resistencia siempre, y la fuerza, destreza o "
                              "inteligencia que pide la clase.");
        ImGui::Checkbox("Subir habilidades", &g_settings.skills);
        ImGui::SetItemTooltip("Los puntos de habilidad a la más atrasada (comercio y montar las últimas), nunca por "
                              "encima del nivel del héroe.");
        ImGui::Checkbox("Aprender habilidades nuevas", &g_settings.learn);
        ImGui::SetItemTooltip("Al abrirse un hueco (niveles 3, 6, 12, 20, 30 y 50), elige entre las que ofrece la clase.");
        ImGui::Checkbox("Poner artes y combos en la barra", &g_settings.bar);
        ImGui::SetItemTooltip("En los huecos libres de la barra (teclas 6-0) que ya tiene el héroe: primero los combos, "
                              "luego las artes de más nivel. Lo que pusiste tú no se toca.");
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Build", L"Attributes", g_settings.attributes);
        ModkitMenu::write(ini, L"Build", L"Skills", g_settings.skills);
        ModkitMenu::write(ini, L"Build", L"Learn", g_settings.learn);
        ModkitMenu::write(ini, L"Build", L"Bar", g_settings.bar);
    }

    void install()
    {
        const std::wstring ini = iniPath();
        g_settings.attributes = GetPrivateProfileIntW(L"Build", L"Attributes", 1, ini.c_str()) != 0;
        g_settings.skills = GetPrivateProfileIntW(L"Build", L"Skills", 1, ini.c_str()) != 0;
        g_settings.learn = GetPrivateProfileIntW(L"Build", L"Learn", 1, ini.c_str()) != 0;
        g_settings.bar = GetPrivateProfileIntW(L"Build", L"Bar", 1, ini.c_str()) != 0;
        g_ready = Sig::resolve(kSigs) == 0;
        LOG("HeroBuild: {} (attributes {}, skills {}, learn {})", g_ready ? "ready" : "not available (signatures)",
            g_settings.attributes, g_settings.skills, g_settings.learn);
        ModkitMenu::add({"Progreso del héroe", &drawMenu, &saveMenu});
    }
}

void HeroBuild::onFrame(DWORD now, bool calm)
{
    static std::once_flag once;
    std::call_once(once, install);
    static DWORD nextAt = 0;
    if (!g_ready || !calm || (nextAt && static_cast<int>(now - nextAt) < 0) || g_working ||
        !(g_settings.attributes || g_settings.skills || g_settings.learn || g_settings.bar))
    {
        return;
    }
    nextAt = now + 2000;
    g_working = true;
    Inject::call(&work, nullptr);
}
