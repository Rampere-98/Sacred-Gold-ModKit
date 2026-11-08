#include "game/combat_learn.h"
#include "game/combat_state.h"
#include "game/map_memory.h"
#include "game/modkit_menu.h"
#include "log.h"
#include "sacred/object.h"
#include "sig.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Sacred;

namespace
{
    // Creature fields (GAME_DATA.md): the type id at +0x10, the level at +0x3FE; the hero's experience in its stats
    // (+0x3A8 + 0x0C).
    constexpr size_t kTypeId = 0x10;
    constexpr size_t kLevel = 0x3FE;
    constexpr size_t kExperience = 0x3A8 + 0x0C;

    template <class T>
    T field(const void* object, size_t offset)
    {
        return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(object) + offset);
    }

    // typeName (ENG 0043CED0): cdecl (type id) -> "TYPE_NPC_..." from a table the game builds on first use (its guard
    // byte is the first operand); only read once the game has built it.
    using TypeName = const char*(__cdecl*)(uint32_t type);
    uintptr_t g_typeName = 0;
    const Sig::Entry kSigs[] = {
        {"typeName", "8A 0D ?? ?? ?? ?? 83 EC 10 B0 01 84 C8 56 75 ?? 8A D1 B9 ?? ?? ?? ?? 0A D0 88 15 ?? ?? ?? ?? E8", 0,
            Sig::Take::Match, &g_typeName},
    };

    std::string typeName(uint32_t type)
    {
        if (g_typeName)
        {
            const uint8_t* guard = *reinterpret_cast<uint8_t* const*>(g_typeName + 2);
            if (*guard & 1)
            {
                if (const char* name = reinterpret_cast<TypeName>(g_typeName)(type); name && *name)
                {
                    std::string s = name;
                    if (s.rfind("TYPE_NPC_", 0) == 0)
                    {
                        s.erase(0, 9);
                    }
                    return s;
                }
            }
        }
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "type %u", type);
        return buffer;
    }

    // The model: log(rate + kFloor) = w . x, x = (1, log2(its max health / the hero's), level difference / 10,
    // ranged). Learned by gradient steps, one per enemy that struck the hero long enough to tell.
    constexpr int kFeatures = 4;
    constexpr float kFloor = 0.002f;
    constexpr float kRate = 0.05f;
    constexpr float kMinStrikeSeconds = 2.0f;
    constexpr float kTypePrior = 3.0f;          // a kind's own measure counts as this many samples of the model's
    float g_w[kFeatures] = {std::log(0.01f + kFloor), 0.5f, 0.5f, 0.0f};
    int g_samples = 0;

    struct Kind
    {
        std::string name;
        int samples = 0;
        float logRate = 0.0f;   // mean of log(rate + kFloor)
        int minLevel = 0, maxLevel = 0;
        int killed = 0;
    };
    std::map<uint32_t, Kind> g_kinds;
    std::mutex g_lock;          // g_kinds and g_w: the menu draws on another thread

    struct Seen
    {
        uint32_t type = 0;
        int level = 0;
        int maxHealth = 0, health = 0;
        bool ranged = false;
        float strikeSeconds = 0.0f;     // seconds it aimed at the hero
        float damage = 0.0f;            // health of the hero's put down to it
        int toHirelings = 0;
        DWORD firstHurtAt = 0;          // when it first lost health
        DWORD lastSeen = 0;
        float predicted = 0.0f;
        bool learned = false;
    };
    std::unordered_map<uint32_t, Seen> g_seen;
    std::unordered_map<uint32_t, float> g_relative;

    // The fight under way.
    struct Fight
    {
        DWORD start = 0, lastAction = 0;
        int heroStart = 0, heroMin = 0, heroMax = 0;
        int mountMin = 0, mountMax = 0;
        bool mounted = false;
        int experience = 0;
        int potions = 0;
        DWORD lastPotion = 0;
        int hirelingDeaths = 0;
        bool died = false;
        int kills = 0;
        float unexplained = 0.0f;   // health lost with nobody striking (poison, an archer unseen)
        std::vector<std::pair<uint32_t, Seen>> foes;    // those of the fight that are gone
    };
    Fight g_fight;
    bool g_inFight = false;
    DWORD g_lastLook = 0;
    int g_lastHealth = 0, g_lastMount = 0;
    uint32_t g_heroId = 0;
    std::unordered_map<uint32_t, int> g_hirelingHealth;
    float g_incoming = 0.0f;    // share of max health per second, those striking now
    float g_heroFraction = 1.0f;
    DWORD g_savedAt = 0;
    bool g_dirty = false;
    constexpr DWORD kFightOverMs = 5000;

    std::wstring dirFile(const wchar_t* name)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path = exe;
        const size_t slash = path.find_last_of(L"\\/");
        return (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + name;
    }

    void features(const Seen& s, int heroMax, int heroLevel, float x[kFeatures])
    {
        x[0] = 1.0f;
        x[1] = std::clamp(std::log2(std::max(1, s.maxHealth) / static_cast<float>(std::max(1, heroMax))), -6.0f, 4.0f);
        x[2] = std::clamp((s.level - heroLevel) / 10.0f, -2.0f, 2.0f);
        x[3] = s.ranged ? 1.0f : 0.0f;
    }

    // Under g_lock.
    float predictLog(const Seen& s, int heroMax, int heroLevel)
    {
        float x[kFeatures];
        features(s, heroMax, heroLevel, x);
        float model = 0.0f;
        for (int i = 0; i < kFeatures; ++i)
        {
            model += g_w[i] * x[i];
        }
        const auto it = g_kinds.find(s.type);
        if (it != g_kinds.end() && it->second.samples > 0)
        {
            const float n = static_cast<float>(it->second.samples);
            return (n * it->second.logRate + kTypePrior * model) / (n + kTypePrior);
        }
        return model;
    }

    // One enemy done striking the hero long enough: a step of the model and its kind's measure.
    void learn(Seen& s, int heroMax, int heroLevel)
    {
        if (s.learned || s.strikeSeconds < kMinStrikeSeconds || heroMax <= 0)
        {
            return;
        }
        s.learned = true;
        const float rate = s.damage / static_cast<float>(heroMax) / s.strikeSeconds;
        const float y = std::log(rate + kFloor);
        const float weight = std::min(1.0f, s.strikeSeconds / 6.0f);
        float x[kFeatures];
        features(s, heroMax, heroLevel, x);
        const std::lock_guard lock(g_lock);
        float model = 0.0f;
        for (int i = 0; i < kFeatures; ++i)
        {
            model += g_w[i] * x[i];
        }
        const float error = std::clamp(y - model, -3.0f, 3.0f);
        for (int i = 0; i < kFeatures; ++i)
        {
            g_w[i] += kRate * weight * error * x[i];
        }
        ++g_samples;
        Kind& k = g_kinds[s.type];
        if (k.name.empty())
        {
            k.name = typeName(s.type);
        }
        k.samples = std::min(k.samples + 1, 50);
        k.logRate += (y - k.logRate) / static_cast<float>(k.samples);
        k.minLevel = k.minLevel ? std::min(k.minLevel, s.level) : s.level;
        k.maxLevel = std::max(k.maxLevel, s.level);
        g_dirty = true;
    }

    void load()
    {
        FILE* f = _wfopen(dirFile(L"SacredBild-learn.txt").c_str(), L"r");
        if (!f)
        {
            return;
        }
        char line[512];
        const std::lock_guard lock(g_lock);
        while (std::fgets(line, sizeof(line), f))
        {
            float w[kFeatures];
            unsigned type = 0;
            Kind k;
            char name[256] = {};
            if (std::sscanf(line, "model %f %f %f %f %d", &w[0], &w[1], &w[2], &w[3], &g_samples) == 5)
            {
                std::copy(w, w + kFeatures, g_w);
            }
            else if (std::sscanf(line, "kind %u %d %f %d %d %d %255s", &type, &k.samples, &k.logRate, &k.minLevel,
                                 &k.maxLevel, &k.killed, name) == 7)
            {
                k.name = name;
                g_kinds[type] = k;
            }
        }
        std::fclose(f);
        LOG("CombatLearn: {} enemy kinds learned, the model from {} samples", g_kinds.size(), g_samples);
    }

    void save()
    {
        const std::wstring path = dirFile(L"SacredBild-learn.txt");
        const std::wstring temp = path + L".tmp";
        FILE* f = _wfopen(temp.c_str(), L"w");
        if (!f)
        {
            return;
        }
        {
            const std::lock_guard lock(g_lock);
            std::fprintf(f, "# SacredBild combat learning: model w0 w1 w2 w3 samples; kind id samples logRate minLevel "
                            "maxLevel killed name\n");
            std::fprintf(f, "model %.5f %.5f %.5f %.5f %d\n", g_w[0], g_w[1], g_w[2], g_w[3], g_samples);
            for (const auto& [type, k] : g_kinds)
            {
                std::fprintf(f, "kind %u %d %.5f %d %d %d %s\n", type, k.samples, k.logRate, k.minLevel, k.maxLevel,
                             k.killed, k.name.empty() ? "?" : k.name.c_str());
            }
            g_dirty = false;
        }
        std::fclose(f);
        MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void writeFight(DWORD now, int heroHealth, int heroLevel)
    {
        std::string line;
        char head[512];
        std::snprintf(head, sizeof(head),
                      "{\"time\":%lu,\"seconds\":%.1f,\"heroLevel\":%d,\"heroMax\":%d,\"heroStart\":%d,\"heroMin\":%d,"
                      "\"heroEnd\":%d,\"mounted\":%s,\"mountMin\":%d,\"mountMax\":%d,\"potions\":%d,\"kills\":%d,"
                      "\"experience\":%d,\"hirelingDeaths\":%d,\"died\":%s,\"unexplained\":%.0f,\"foes\":[",
                      static_cast<unsigned long>(now), (g_fight.lastAction - g_fight.start) / 1000.0f, heroLevel,
                      g_fight.heroMax, g_fight.heroStart, g_fight.heroMin, heroHealth,
                      g_fight.mounted ? "true" : "false", g_fight.mountMin, g_fight.mountMax, g_fight.potions,
                      g_fight.kills, g_fight.experience, g_fight.hirelingDeaths, g_fight.died ? "true" : "false",
                      g_fight.unexplained);
        line = head;
        bool first = true;
        for (const auto& [id, s] : g_fight.foes)
        {
            char foe[384];
            const float rate = s.strikeSeconds > 0.0f && g_fight.heroMax > 0
                                   ? s.damage / g_fight.heroMax / s.strikeSeconds * 100.0f : 0.0f;
            std::snprintf(foe, sizeof(foe),
                          "%s{\"kind\":\"%s\",\"level\":%d,\"maxHealth\":%d,\"left\":%d,\"ranged\":%s,"
                          "\"strikeSeconds\":%.1f,\"damage\":%.0f,\"percentPerSecond\":%.2f,\"predicted\":%.2f,"
                          "\"toHirelings\":%d}",
                          first ? "" : ",", typeName(s.type).c_str(), s.level, s.maxHealth, std::max(0, s.health),
                          s.ranged ? "true" : "false", s.strikeSeconds, s.damage, rate, s.predicted * 100.0f,
                          s.toHirelings);
            line += foe;
            first = false;
        }
        line += "]}\n";
        if (FILE* f = _wfopen(dirFile(L"SacredBild-combat.log").c_str(), L"a"))
        {
            std::fputs(line.c_str(), f);
            std::fclose(f);
        }
        LOG("CombatLearn: fight over, {:.0f} s, {} enemies, {} killed, health {} -> {} (lowest {}), {} potions, "
            "{} experience", (g_fight.lastAction - g_fight.start) / 1000.0f, g_fight.foes.size(), g_fight.kills,
            g_fight.heroStart, heroHealth, g_fight.heroMin, g_fight.potions, g_fight.experience);
    }

    // An enemy gone from the looks: down (killed) or out of sight.
    void gone(uint32_t id, Seen& s, int heroMax, int heroLevel)
    {
        learn(s, heroMax, heroLevel);
        // Down: the creature dead or gone (a blow that kills is seldom seen in the looks); out of sight: still alive.
        bool killed = s.health <= 0;
        if (!killed && s.firstHurtAt)
        {
            cObjectManager* objects = cObjectManager::instance();
            cCreature* c = objects ? objects->creature(id) : nullptr;
            killed = !c || c->health <= 0 || CombatState::action(c) == CombatState::Dead;
        }
        if (killed && s.firstHurtAt)
        {
            ++g_fight.kills;
            const std::lock_guard lock(g_lock);
            const auto it = g_kinds.find(s.type);
            if (it != g_kinds.end())
            {
                ++it->second.killed;
            }
        }
        if (g_inFight && (s.strikeSeconds > 0.0f || s.firstHurtAt))
        {
            g_fight.foes.emplace_back(id, s);
        }
    }

    void drawMenu()
    {
        const std::lock_guard lock(g_lock);
        ImGui::TextWrapped("Lo que el ataque automático aprende en combate: cuánto quita cada enemigo (en %% de tu vida "
                           "máxima por segundo). Aprende de rasgos relativos (su vida frente a la tuya, su nivel frente al "
                           "tuyo, si dispara de lejos), así que lo aprendido sirve también para enemigos nuevos.");
        ImGui::Text("Modelo: %d muestras. Combates en SacredBild-combat.log.", g_samples);
        if (g_kinds.empty())
        {
            ImGui::TextDisabled("Aún sin enemigos aprendidos.");
            return;
        }
        if (ImGui::BeginTable("kinds", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0.0f, 220.0f)))
        {
            ImGui::TableSetupColumn("Enemigo");
            ImGui::TableSetupColumn("Niveles");
            ImGui::TableSetupColumn("%vida/s");
            ImGui::TableSetupColumn("Vistos");
            ImGui::TableSetupColumn("Muertos");
            ImGui::TableHeadersRow();
            for (const auto& [type, k] : g_kinds)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(k.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%d-%d", k.minLevel, k.maxLevel);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", std::max(0.0f, std::exp(k.logRate) - kFloor) * 100.0f);
                ImGui::TableNextColumn();
                ImGui::Text("%d", k.samples);
                ImGui::TableNextColumn();
                ImGui::Text("%d", k.killed);
            }
            ImGui::EndTable();
        }
    }

    void saveMenu(const std::wstring&)
    {
        save();
    }
}

void CombatLearn::install()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (Sig::resolve(kSigs) != 0)
        {
            g_typeName = 0;
        }
        load();
        ModkitMenu::add({"Enciclopedia de combate", &drawMenu, &saveMenu});
    });
}

void CombatLearn::observe(const Look& look)
{
    install();
    cCreature* hero = look.hero;
    if (!hero || hero->maxHealth <= 0)
    {
        return;
    }
    const DWORD now = look.now;
    const int heroMax = hero->maxHealth;
    const int heroHealth = hero->health;
    const int heroLevel = field<uint16_t>(hero, kLevel);
    const int experience = field<int32_t>(hero, kExperience);
    const bool mounted = look.self && look.self != hero;
    // A new hero (another game loaded): nothing carried over.
    if (hero->id != g_heroId)
    {
        g_heroId = hero->id;
        g_seen.clear();
        g_inFight = false;
        g_lastLook = 0;
        g_hirelingHealth.clear();
    }
    const float dt = g_lastLook && now - g_lastLook < 1500 ? (now - g_lastLook) / 1000.0f : 0.0f;
    const bool potion = CombatState::within(CombatState::potionAt.load(), now, 600);

    // The health the hero lost since the last look, shared among those striking it by how hard each is thought to
    // hit (at least a little each).
    const float lost = dt > 0.0f && !potion ? static_cast<float>(std::max(0, g_lastHealth - heroHealth)) : 0.0f;
    float weights = 0.0f;
    bool engaged = false;
    struct Sighting
    {
        uint32_t type;
        int32_t x, y;
    };
    std::vector<Sighting> sightings;    // enemies met for the first time: on the agent's map (outside g_lock)
    {
        const std::lock_guard lock(g_lock);
        for (int i = 0; i < look.count; ++i)
        {
            const Foe& f = look.foes[i];
            Seen& s = g_seen[f.id];
            if (!s.lastSeen)
            {
                if (cObject* o = cObjectManager::instance() ? cObjectManager::instance()->object(f.id) : nullptr)
                {
                    s.type = field<uint32_t>(o, kTypeId);
                    s.level = field<uint16_t>(o, kLevel);
                    sightings.push_back({s.type, o->x, o->y});
                }
            }
            if (f.health < s.health && !s.firstHurtAt)
            {
                s.firstHurtAt = now;
            }
            s.maxHealth = f.maxHealth;
            s.health = f.health;
            s.ranged = s.ranged || f.ranged;
            s.lastSeen = now;
            s.predicted = std::max(0.0f, std::exp(predictLog(s, heroMax, heroLevel)) - kFloor);
            if (f.atHero)
            {
                weights += std::max(s.predicted, kFloor);
            }
            engaged = engaged || f.atHero || f.atHireling || s.firstHurtAt;
        }
    }
    if (!sightings.empty() && look.hero)
    {
        const int layer = *(reinterpret_cast<const uint8_t*>(look.hero) + 0x24);
        for (const Sighting& s : sightings)
        {
            MapMemory::sighting(s.type, typeName(s.type), layer, s.x, s.y);
        }
    }
    g_incoming = 0.0f;
    std::vector<float> rates;
    for (int i = 0; i < look.count; ++i)
    {
        const Foe& f = look.foes[i];
        Seen& s = g_seen[f.id];
        rates.push_back(s.predicted);
        if (f.atHero)
        {
            s.strikeSeconds += dt;
            s.damage += weights > 0.0f ? lost * std::max(s.predicted, kFloor) / weights : 0.0f;
            g_incoming += s.predicted;
        }
    }
    float middle = 0.0f;
    if (!rates.empty())
    {
        std::nth_element(rates.begin(), rates.begin() + rates.size() / 2, rates.end());
        middle = rates[rates.size() / 2];
    }
    g_relative.clear();
    for (int i = 0; i < look.count; ++i)
    {
        const float p = g_seen[look.foes[i].id].predicted;
        g_relative[look.foes[i].id] = middle > 0.0f ? p / middle : 1.0f;
    }
    g_heroFraction = static_cast<float>(heroHealth) / static_cast<float>(heroMax);

    // The hirelings: health they lost to those aiming at them, and deaths.
    int hirelingLost = 0;
    for (int i = 0; i < look.hirelingCount; ++i)
    {
        cCreature* h = cObjectManager::instance() ? cObjectManager::instance()->creature(look.hirelings[i]) : nullptr;
        if (!h)
        {
            continue;
        }
        auto it = g_hirelingHealth.find(look.hirelings[i]);
        if (it != g_hirelingHealth.end())
        {
            hirelingLost += std::max(0, it->second - h->health);
            if (g_inFight && it->second > 0 && h->health <= 0)
            {
                ++g_fight.hirelingDeaths;
            }
        }
        g_hirelingHealth[look.hirelings[i]] = h->health;
    }
    int atHirelings = 0;
    for (int i = 0; i < look.count; ++i)
    {
        atHirelings += look.foes[i].atHireling ? 1 : 0;
    }
    for (int i = 0; i < look.count && atHirelings; ++i)
    {
        if (look.foes[i].atHireling)
        {
            g_seen[look.foes[i].id].toHirelings += hirelingLost / atHirelings;
        }
    }

    // The fight: begins with an enemy in it (or health lost), ends some seconds after the last one.
    if (engaged || lost > 0.0f)
    {
        if (!g_inFight)
        {
            g_inFight = true;
            g_fight = Fight{};
            g_fight.start = now;
            g_fight.heroStart = g_lastHealth ? g_lastHealth : heroHealth;
            g_fight.heroMin = heroHealth;
            g_fight.experience = experience;
            g_fight.mountMin = mounted ? look.self->health : 0;
        }
        g_fight.lastAction = now;
    }
    if (g_inFight)
    {
        g_fight.heroMin = std::min(g_fight.heroMin, heroHealth);
        g_fight.heroMax = heroMax;
        g_fight.died = g_fight.died || heroHealth <= 0 || CombatState::action(hero) == CombatState::Dead;
        if (mounted)
        {
            g_fight.mounted = true;
            g_fight.mountMin = std::min(g_fight.mountMin ? g_fight.mountMin : look.self->health, look.self->health);
            g_fight.mountMax = look.self->maxHealth;
        }
        const DWORD drank = CombatState::potionAt.load();
        if (drank && drank != g_fight.lastPotion && drank - g_fight.start < 0x80000000u)
        {
            g_fight.lastPotion = drank;
            ++g_fight.potions;
        }
        if (lost > 0.0f && weights <= 0.0f)
        {
            g_fight.unexplained += lost;
        }
    }

    // Those gone from the looks (down, or out of sight for a while).
    for (auto it = g_seen.begin(); it != g_seen.end();)
    {
        const bool down = it->second.health <= 0;
        if (down || now - it->second.lastSeen > 3000)
        {
            gone(it->first, it->second, heroMax, heroLevel);
            it = g_seen.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (g_inFight && now - g_fight.lastAction > kFightOverMs)
    {
        for (auto& [id, s] : g_seen)
        {
            if (s.strikeSeconds > 0.0f || s.firstHurtAt)
            {
                learn(s, heroMax, heroLevel);
                g_fight.foes.emplace_back(id, s);
            }
        }
        g_fight.experience = experience - g_fight.experience;
        writeFight(now, heroHealth, heroLevel);
        g_inFight = false;
        for (auto& [id, s] : g_seen)
        {
            s.strikeSeconds = 0.0f;
            s.damage = 0.0f;
            s.learned = false;
            s.firstHurtAt = 0;
        }
        if (g_dirty && (!g_savedAt || now - g_savedAt > 30000))
        {
            save();
            g_savedAt = now;
        }
    }

    g_lastHealth = heroHealth;
    g_lastMount = mounted ? look.self->health : 0;
    g_lastLook = now;
}

float CombatLearn::danger(uint32_t id)
{
    const auto it = g_seen.find(id);
    return it != g_seen.end() ? it->second.predicted : 0.0f;
}

bool CombatLearn::kind(uint32_t type, float& rate, int& killed)
{
    const std::lock_guard lock(g_lock);
    const auto it = g_kinds.find(type);
    if (it == g_kinds.end())
    {
        rate = -1.0f;
        killed = 0;
        return false;
    }
    rate = it->second.samples ? std::max(0.0f, std::exp(it->second.logRate) - kFloor) : -1.0f;
    killed = it->second.killed;
    return true;
}

float CombatLearn::relativeDanger(uint32_t id)
{
    const auto it = g_relative.find(id);
    return it != g_relative.end() ? it->second : 1.0f;
}

float CombatLearn::secondsToDie()
{
    return g_incoming > 1e-4f ? g_heroFraction / g_incoming : 1e6f;
}
