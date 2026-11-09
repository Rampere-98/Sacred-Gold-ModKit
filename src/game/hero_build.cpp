#include "game/hero_build.h"
#include "game/modkit_menu.h"
#include "input/inject.h"
#include "log.h"
#include "sacred/object.h"
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
    };
    Settings g_settings;

    // stats = creature + 0x3A8 (seen live, 2026-11-09): +8 class; +0x1C u8[6] points put in each attribute, +0x23
    // attribute points to spend; +0x24 u8[8] skill id per slot, +0x2C u8[8] its base level; +0x42 u16 skill points to
    // spend; +0x44 u16 new skill slots to fill; +0x56 u16 the hero's level.
    constexpr size_t kStats = 0x3A8;

    uintptr_t g_spendAttr = 0, g_addSkillLevel = 0, g_learnable = 0, g_learnSkill = 0;
    const Sig::Entry kSigs[] = {
        {"spendAttr", "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 83 EC 7C 53 56 57 8B ?? ?? ?? ?? 00 00 83 FF 07",
            0, Sig::Take::Match, &g_spendAttr},
        {"addSkillLevel", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 8B ?? ?? ?? ?? 00 00 53 56 57 8B F9",
            0, Sig::Take::Match, &g_addSkillLevel},
        {"learnableSkills", "83 EC 10 56 8B F1 8B 0D ?? ?? ?? ??", 0, Sig::Take::Match, &g_learnable},
        {"learnSkill", "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 ?? ?? ?? ?? 00 00 53 56 57 8B F9 33 DB",
            0, Sig::Take::Match, &g_learnSkill},
    };
    bool g_ready = false;

    using SpendAttr = void(__fastcall*)(void* stats, void*, uint32_t attr);
    using AddSkillLevel = void(__fastcall*)(void* stats, void*, uint32_t slot, int16_t delta, bool pay);
    using Learnable = int(__fastcall*)(void* stats, void*, uint8_t* out);
    using LearnSkill = void(__fastcall*)(void* stats, void*, uint8_t id);

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

    // How much a skill is worth to the agent: trade and riding last, the rest alike.
    float skillWeight(uint8_t id)
    {
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

    // Attributes by the class (order of the character window: strength, endurance, dexterity, physical regeneration,
    // intelligence, mental regeneration): how much of each, relative. 1 seraphim, 2 gladiator, 3 battle mage, 4 dark
    // elf, 5 wood elf, 6/7 vampiress, 8 dwarf.
    const int kAttrPlan[9][6] = {
        {2, 3, 2, 1, 1, 1},     // unknown
        {3, 3, 2, 1, 1, 1},     // seraphim
        {4, 3, 2, 1, 0, 0},     // gladiator
        {0, 3, 1, 0, 4, 2},     // battle mage
        {1, 3, 4, 1, 1, 0},     // dark elf
        {0, 3, 4, 1, 2, 1},     // wood elf
        {4, 3, 2, 1, 0, 0},     // vampiress
        {4, 3, 2, 1, 0, 0},     // vampiress (other form)
        {3, 3, 2, 1, 0, 1},     // dwarf
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
        const int level = *reinterpret_cast<const uint16_t*>(st + 0x56);
        // One point a look, as a player clicking "+".
        if (g_settings.attributes && st[0x23] > 0)
        {
            const int* plan = kAttrPlan[cls < 9 ? cls : 0];
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
    }

    void saveMenu(const std::wstring& ini)
    {
        ModkitMenu::write(ini, L"Build", L"Attributes", g_settings.attributes);
        ModkitMenu::write(ini, L"Build", L"Skills", g_settings.skills);
        ModkitMenu::write(ini, L"Build", L"Learn", g_settings.learn);
    }

    void install()
    {
        const std::wstring ini = iniPath();
        g_settings.attributes = GetPrivateProfileIntW(L"Build", L"Attributes", 1, ini.c_str()) != 0;
        g_settings.skills = GetPrivateProfileIntW(L"Build", L"Skills", 1, ini.c_str()) != 0;
        g_settings.learn = GetPrivateProfileIntW(L"Build", L"Learn", 1, ini.c_str()) != 0;
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
        !(g_settings.attributes || g_settings.skills || g_settings.learn))
    {
        return;
    }
    nextAt = now + 2000;
    g_working = true;
    Inject::call(&work, nullptr);
}
