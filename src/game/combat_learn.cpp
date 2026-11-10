#include "game/combat_learn.h"
#include "game/combat_state.h"
#include "game/gear.h"
#include "game/hero_profile.h"
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

    // The forecast's learned parts: the hero's effective blows per second (attack speed times hits; reset with the
    // hero, since it is the build's), and the log error of the expected loss (bias, spread) over past fights.
    float g_blowsPerSecond = 1.0f;
    int g_blowSamples = 0;
    float g_lossBias = 0.0f, g_lossSpread = 0.7f;
    int g_lossSamples = 0;

    // Each class its own agent (the player, 2026-11-10: «1 agente para cada personaje»): the forecast's bias and spread
    // and the blows per second, kept per class and saved («class» lines); the enemies' kinds stay shared (what an
    // enemy deals does not depend on who it fights). A class not seen yet starts from the shared «forecast2».
    struct ClassLearn
    {
        bool known = false;
        float bias = 0.0f, spread = 0.7f, blows = 1.0f;
        int samples = 0, blowSamples = 0;
    };
    ClassLearn g_byClass[10];
    int g_class = -1;

    // Under g_lock.
    void useClass(int cls, bool newHero)
    {
        if (cls < 0 || cls >= 10 || (cls == g_class && !newHero))
        {
            return;
        }
        if (g_class >= 0 && cls != g_class)
        {
            g_byClass[g_class] = {true, g_lossBias, g_lossSpread, g_blowsPerSecond, g_lossSamples, g_blowSamples};
        }
        g_class = cls;
        const ClassLearn& c = g_byClass[cls];
        if (c.known)
        {
            g_lossBias = c.bias;
            g_lossSpread = c.spread;
            g_lossSamples = c.samples;
            g_blowsPerSecond = c.blows;
            g_blowSamples = std::min(c.blowSamples, 10);     // its own, still open to this hero's gear
        }
        LOG("CombatLearn: class {} ({}): forecast bias {:.2f} spread {:.2f} from {} fights, {:.2f} blows/s", cls,
            c.known ? "its own learning" : "new, from the shared one", g_lossBias, g_lossSpread, g_lossSamples,
            g_blowsPerSecond);
    }
    CombatLearn::Forecast g_forecast;
    std::unordered_map<uint32_t, int> g_killRank;

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
        float hit = -1.0f;              // the hero's expected blow against it
        bool summoner = false;          // brings others in while alive (raises, conjures, heals)
    };

    // Kinds that bring others into a fight while alive, by their type names (the web researcher, WEB_RESEARCH §18:
    // goblin shamans breed goblins, Sakkara priests and mages conjure, liches raise servants, druids call wisps; the
    // priests seen live heal their side). Matched on the names the game gives (GOBLIN_MAGIC, PRIEST...).
    bool summonerName(const std::string& name)
    {
        static const char* const kParts[] = {"GOBLIN_MAGIC", "SHAMAN", "PRIEST", "BLACK_MAGICIAN", "LICH", "NECRO",
                                             "DRUID", "SAKKARA", "WITCH"};
        for (const char* part : kParts)
        {
            if (name.find(part) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
    // How much more a summoner weighs in the forecast: what it brings over the fight (a guess until measured).
    constexpr float kSummonerWeight = 2.5f;
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
        float lostTotal = 0.0f;     // all the health lost (potions hide it from start - end)
        float dealt = 0.0f;         // health the enemies lost
        CombatLearn::Forecast forecast; // at its start
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
            int cls = 0;
            ClassLearn c;
            if (std::sscanf(line, "class %d %f %f %d %f %d", &cls, &c.bias, &c.spread, &c.samples, &c.blows,
                            &c.blowSamples) == 6 && cls >= 0 && cls < 10)
            {
                c.known = true;
                c.spread = std::clamp(c.spread, 0.2f, 2.0f);
                g_byClass[cls] = c;
            }
            else if (std::sscanf(line, "forecast2 %f %f %d", &g_lossBias, &g_lossSpread, &g_lossSamples) == 3)
            {
                g_lossSpread = std::clamp(g_lossSpread, 0.2f, 2.0f);
            }
            else if (std::sscanf(line, "model %f %f %f %f %d", &w[0], &w[1], &w[2], &w[3], &g_samples) == 5)
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
            std::fprintf(f, "forecast2 %.5f %.5f %d\n", g_lossBias, g_lossSpread, g_lossSamples);
            if (g_class >= 0)
            {
                g_byClass[g_class] = {true, g_lossBias, g_lossSpread, g_blowsPerSecond, g_lossSamples, g_blowSamples};
            }
            for (int cls = 0; cls < 10; ++cls)
            {
                if (const ClassLearn& c = g_byClass[cls]; c.known)
                {
                    std::fprintf(f, "class %d %.5f %.5f %d %.4f %d\n", cls, c.bias, c.spread, c.samples, c.blows,
                                 c.blowSamples);
                }
            }
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
        char head[1024];
        std::snprintf(head, sizeof(head),
                      "{\"time\":%lu,\"seconds\":%.1f,\"heroLevel\":%d,\"heroMax\":%d,\"heroStart\":%d,\"heroMin\":%d,"
                      "\"heroEnd\":%d,\"mounted\":%s,\"mountMin\":%d,\"mountMax\":%d,\"potions\":%d,\"kills\":%d,"
                      "\"experience\":%d,\"hirelingDeaths\":%d,\"died\":%s,\"unexplained\":%.0f,"
                      "\"lostTotal\":%.0f,\"dealt\":%.0f,\"forecast\":{\"foes\":%d,\"loss\":%.3f,\"seconds\":%.1f,"
                      "\"win\":%.3f,\"blowsPerSecond\":%.2f,\"bias\":%.3f,\"spread\":%.3f},\"foes\":[",
                      static_cast<unsigned long>(now), (g_fight.lastAction - g_fight.start) / 1000.0f, heroLevel,
                      g_fight.heroMax, g_fight.heroStart, g_fight.heroMin, heroHealth,
                      g_fight.mounted ? "true" : "false", g_fight.mountMin, g_fight.mountMax, g_fight.potions,
                      g_fight.kills, g_fight.experience, g_fight.hirelingDeaths, g_fight.died ? "true" : "false",
                      g_fight.unexplained, g_fight.lostTotal, g_fight.dealt, g_fight.forecast.foes,
                      g_fight.forecast.loss, g_fight.forecast.seconds, g_fight.forecast.win, g_blowsPerSecond,
                      g_lossBias, g_lossSpread);
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
                          "\"toHirelings\":%d,\"hit\":%.1f}",
                          first ? "" : ",", typeName(s.type).c_str(), s.level, s.maxHealth, std::max(0, s.health),
                          s.ranged ? "true" : "false", s.strikeSeconds, s.damage, rate, s.predicted * 100.0f,
                          s.toHirelings, s.hit);
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
        LOG("Forecast: foreseen {:.0f}% lost in {:.0f} s, {:.0f}% to win; it was {:.0f}% lost{}",
            g_fight.forecast.loss * 100.0f, g_fight.forecast.seconds, g_fight.forecast.win * 100.0f,
            g_fight.heroMax > 0 ? g_fight.lostTotal * 100.0f / g_fight.heroMax : 0.0f, g_fight.died ? ", died" : "");
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

    float normalCdf(float z)
    {
        return 0.5f * std::erfc(-z / std::sqrt(2.0f));
    }

    // The forecast from the enemies seen at this look (those in a fight or aiming at the hero or its hirelings, else
    // all around): Smith's rule on (rate, seconds to kill).
    void computeForecast(const CombatLearn::Look& look, float heroFraction)
    {
        struct Item
        {
            uint32_t id;
            float rate, seconds;
        };
        std::vector<Item> items;
        bool anyEngaged = false;
        for (int i = 0; i < look.count; ++i)
        {
            anyEngaged = anyEngaged || look.foes[i].atHero || look.foes[i].atHireling;
        }
        for (int i = 0; i < look.count; ++i)
        {
            const CombatLearn::Foe& f = look.foes[i];
            const Seen& s = g_seen[f.id];
            if (f.health <= 0 || (anyEngaged && !f.atHero && !f.atHireling && !s.firstHurtAt))
            {
                continue;
            }
            const float dps = s.hit > 0.0f ? s.hit * g_blowsPerSecond : 0.0f;
            const float seconds = dps > 0.0f ? std::min(600.0f, f.health / dps) : 600.0f;
            // A summoner: its own blows and those of what it brings while it lives, so killed first by Smith's rule.
            items.push_back({f.id, std::max(s.predicted, kFloor) * (s.summoner ? kSummonerWeight : 1.0f), seconds});
        }
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b) { return a.rate * b.seconds > b.rate * a.seconds; });
        CombatLearn::Forecast fc;
        fc.foes = static_cast<int>(items.size());
        g_killRank.clear();
        float clock = 0.0f;
        for (size_t i = 0; i < items.size(); ++i)
        {
            clock += items[i].seconds;
            fc.loss += items[i].rate * clock;
            g_killRank[items[i].id] = static_cast<int>(i);
        }
        fc.seconds = clock;
        fc.first = items.empty() ? 0 : items[0].id;
        if (fc.loss > 1e-4f && heroFraction > 0.0f)
        {
            const float z = (std::log(heroFraction) - std::log(fc.loss) - g_lossBias) / g_lossSpread;
            fc.win = normalCdf(z);
        }
        else
        {
            fc.win = heroFraction > 0.0f ? 1.0f : 0.0f;
        }
        g_forecast = fc;
    }

    // A fight over: its loss against the forecast made at its start (the bias and spread of the log error), and the
    // hero's blows per second from the health the enemies lost.
    void learnForecast(float seconds)
    {
        if (g_fight.heroMax <= 0)
        {
            return;
        }
        if (g_fight.forecast.loss > 0.01f && seconds >= 3.0f)
        {
            const float actual = std::max(g_fight.lostTotal / g_fight.heroMax, 0.005f);
            const float error = std::clamp(std::log(actual) - std::log(g_fight.forecast.loss), -4.0f, 4.0f);
            g_lossSamples = std::min(g_lossSamples + 1, 100);
            // A prior of 8 fights (bias 0, spread 0.7): one wild fight does not swing the chances (seen live: one
            // 251 % fight made a 4-enemy fight 0 % to win, won with 11 % lost).
            const float a = 1.0f / static_cast<float>(g_lossSamples + 8);
            const float before = g_lossBias;
            g_lossBias += a * (error - g_lossBias);
            const float var = g_lossSpread * g_lossSpread;
            g_lossSpread = std::clamp(std::sqrt(var + a * ((error - before) * (error - g_lossBias) - var)), 0.2f, 2.0f);
            g_dirty = true;
        }
        float hitSum = 0.0f;
        int hitCount = 0;
        for (const auto& [id, foe] : g_fight.foes)
        {
            if (foe.firstHurtAt && foe.hit > 0.0f)
            {
                hitSum += foe.hit;
                ++hitCount;
            }
        }
        if (hitCount > 0 && g_fight.dealt > 0.0f && seconds >= 3.0f)
        {
            const float meanHit = hitSum / hitCount;
            const float sample = std::clamp(g_fight.dealt / seconds / meanHit, 0.05f, 10.0f);
            g_blowSamples = std::min(g_blowSamples + 1, 20);
            g_blowsPerSecond += (sample - g_blowsPerSecond) / static_cast<float>(g_blowSamples);
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
        g_blowsPerSecond = 1.0f;
        g_blowSamples = 0;
        if (const HeroProfile::Data d = HeroProfile::current(); d.valid)
        {
            const int cls = d.cls == HeroProfile::Class::VampiressNight ? 6 : static_cast<int>(d.cls);
            const std::lock_guard lock(g_lock);
            useClass(cls, true);
        }
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
    for (int i = 0; i < look.count; ++i)
    {
        g_seen[look.foes[i].id].hit = Gear::expectedHit(look.foes[i].id);
    }
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
                    s.summoner = summonerName(typeName(s.type));
                    sightings.push_back({s.type, o->x, o->y});
                }
            }
            if (f.health < s.health && !s.firstHurtAt)
            {
                s.firstHurtAt = now;
            }
            if (s.lastSeen && f.health < s.health && g_inFight)
            {
                g_fight.dealt += static_cast<float>(s.health - f.health);
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
    computeForecast(look, g_heroFraction);

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
            g_fight.forecast = g_forecast;
            if (g_forecast.foes > 0)
            LOG("Forecast: a fight with {} enemies, {:.0f}% of health to be lost in {:.0f} s, {:.0f}% to win "
                "(first {:x})", g_forecast.foes, g_forecast.loss * 100.0f, g_forecast.seconds, g_forecast.win * 100.0f,
                g_forecast.first);
        }
        // More come in: the forecast taken again if it is grimmer.
        if (g_forecast.foes > g_fight.forecast.foes && g_forecast.loss > g_fight.forecast.loss)
        {
            g_fight.forecast.foes = g_forecast.foes;
            g_fight.forecast.loss = g_forecast.loss;
            g_fight.forecast.seconds = std::max(g_fight.forecast.seconds, g_forecast.seconds);
            g_fight.forecast.win = std::min(g_fight.forecast.win, g_forecast.win);
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
        if (dt > 0.0f && !potion)
        {
            g_fight.lostTotal += static_cast<float>(std::max(0, g_lastHealth - heroHealth));
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
        learnForecast((g_fight.lastAction - g_fight.start) / 1000.0f);
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

const CombatLearn::Forecast& CombatLearn::forecast()
{
    return g_forecast;
}

int CombatLearn::killRank(uint32_t id)
{
    const auto it = g_killRank.find(id);
    return it != g_killRank.end() ? it->second : -1;
}
