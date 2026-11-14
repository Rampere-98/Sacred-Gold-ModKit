#include "game/combat_learn.h"
#include "config/auto_attack.h"
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

    // A creature's combat sheet (the hero's too), from its stats st = creature + 0x3A8 (docs/AGENT_ADDRESSES.md
    // «Enciclopedia», «Resistencias y debilidades de enemigos»; docs/Medicion-de-enemigos.md §2.1). Kinds 0 physical,
    // 1 fire, 2 magic, 3 poison. Unconfirmed on creatures live: the damage (st+0x76/+0x86, which of the two is the
    // minimum disputed: the mean is used) and the attack (st+0x5A); the units of the two speeds.
    struct Sheet
    {
        bool ok = false;
        float dmgA[4] = {}, dmgB[4] = {};   // st+0x76+4i, st+0x86+4i: the kind's two damage bounds
        float res[4] = {};                  // effective resistance: st+0x66+4i times st+0xD6+4i
        float attack = 0.0f, defence = 0.0f;    // st+0x5A, st+0x5E
        int attackSpeed = 0, moveSpeed = 0;     // u16 st+0x3C, st+0x40
        int side = 0;                           // u8 o+0x1F0 (CL_*: 14 hostile, 6 fauna, 5 undead...)
        float meanDamage(int k) const { return (dmgA[k] + dmgB[k]) * 0.5f; }
    };

    bool saneStat(float v)
    {
        return v == v && v >= 0.0f && v < 100000.0f;
    }

    // Raw reads under SEH (a creature can be freed between the look and the read; click_walk's liveWalkable does the
    // same); false: not read, or not a sane sheet.
    bool readSheet(const void* object, Sheet& out)
    {
        __try
        {
            const auto* o = static_cast<const uint8_t*>(object);
            const uint8_t* st = o + 0x3A8;
            Sheet s;
            bool sane = true;
            float any = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                s.dmgA[k] = *reinterpret_cast<const float*>(st + 0x76 + 4 * k);
                s.dmgB[k] = *reinterpret_cast<const float*>(st + 0x86 + 4 * k);
                s.res[k] = *reinterpret_cast<const float*>(st + 0x66 + 4 * k) *
                           *reinterpret_cast<const float*>(st + 0xD6 + 4 * k);
                sane = sane && saneStat(s.dmgA[k]) && saneStat(s.dmgB[k]) && saneStat(s.res[k]);
                any += s.dmgA[k] + s.dmgB[k];
            }
            s.attack = *reinterpret_cast<const float*>(st + 0x5A);
            s.defence = *reinterpret_cast<const float*>(st + 0x5E);
            s.attackSpeed = *reinterpret_cast<const uint16_t*>(st + 0x3C);
            s.moveSpeed = *reinterpret_cast<const uint16_t*>(st + 0x40);
            s.side = o[0x1F0];
            sane = sane && saneStat(s.attack) && saneStat(s.defence) && s.attackSpeed < 2000 && s.moveSpeed < 2000;
            s.ok = sane && (any > 0.0f || s.attack > 0.0f);
            out = s;
            return s.ok;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out.ok = false;
            return false;
        }
    }

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
    // No enemy of the game takes half the hero's health each second for long: a rate above that is a misread (a
    // load, a transformation or a death landing in one look; A8's Daemon, 2026-11-12: ORC_WARRIOR learned at 720 %/s,
    // GOBLIN_CHAMP at 35 %/s, both measured under 1 %; she fled a lone goblin she kills in one blow).
    constexpr float kMaxRate = 0.25f;   // the findings verifier: p99 measured 6.9 %/s over 19k fights
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

    // How often fights went bad (near death or death) per tenth of the forecast's win, per class (the player,
    // 2026-11-14: «la probabilidad de victoria no puede depender del porcentaje en batalla, el agente no conoce el
    // potencial del personaje»: the raw win runs grim below one half; each class learns what its own fights did).
    // Under g_lock. The prior: near deaths + deaths per tenth over the 22 034 fights of docs/Medidores/Prevision.md §2.
    struct BadBin
    {
        float fights = 0.0f, bad = 0.0f;
    };
    BadBin g_bad[10][10];
    constexpr float kBadPrior[10] = {0.297f, 0.261f, 0.203f, 0.128f, 0.127f, 0.073f, 0.051f, 0.049f, 0.021f, 0.003f};
    constexpr float kBadPriorFights = 20.0f;

    float binBad(int cls, int bin)
    {
        const BadBin& b = g_bad[std::clamp(cls, 0, 9)][std::clamp(bin, 0, 9)];
        return (b.bad + kBadPrior[std::clamp(bin, 0, 9)] * kBadPriorFights) / (b.fights + kBadPriorFights);
    }

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
        Sheet sheet;                    // read until it reads sane
        float prior = -1.0f;            // its blows on this hero by its sheet, share of max health a second (-1: none)
        float rate3 = 0.0f, var3 = 0.0f;    // forecast3's rate (the prior and its kind's measure mixed), its log variance
        float distance = 0.0f;          // world units from the hero at the last look
    };

    // The hero's sheet at the last look (its resistances and defence against each enemy's sheet).
    Sheet g_heroSheet;
    constexpr float kPriorSamples = 5.0f;           // the sheet's prior counts as this many measures of the kind
    constexpr float kAttackSpeedPerBlow = 100.0f;   // st+0x3C for one blow a second: a guess until Manolito confirms

    // The enemy's blows on this hero from its sheet: per kind its mean damage through the hero's resistance
    // (dealt^2 / (dealt + resistance), as Gear's estimate the other way), times its chance to hit (its attack against
    // the hero's defence, the formula of Gear::expectedHit), times its blows a second; mixed in the log with what its
    // kind was measured to deal. Without a sheet, today's model. Under g_lock.
    void priorAndRate(Seen& s, int heroMax)
    {
        s.prior = -1.0f;
        if (s.sheet.ok && g_heroSheet.ok && heroMax > 0)
        {
            float blow = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                const float d = s.sheet.meanDamage(k);
                blow += d > 0.0f ? d * d / (d + g_heroSheet.res[k]) : 0.0f;
            }
            const float chance = s.sheet.attack > 0.0f ? s.sheet.attack / (s.sheet.attack + g_heroSheet.defence) : 1.0f;
            const float blows = s.sheet.attackSpeed > 0 ? s.sheet.attackSpeed / kAttackSpeedPerBlow : 1.0f;
            s.prior = std::clamp(blow * chance * blows / static_cast<float>(heroMax), 0.0f, kMaxRate);
        }
        const auto it = g_kinds.find(s.type);
        const float n = it != g_kinds.end() ? static_cast<float>(it->second.samples) : 0.0f;
        if (s.prior >= 0.0f)
        {
            const float logPrior = std::log(s.prior + kFloor);
            const float logRate =
                n > 0.0f ? (kPriorSamples * logPrior + n * it->second.logRate) / (kPriorSamples + n) : logPrior;
            s.rate3 = std::clamp(std::exp(logRate) - kFloor, 0.0f, kMaxRate);
            s.var3 = 0.36f * kPriorSamples / (kPriorSamples + n) + 0.1f;
        }
        else
        {
            s.rate3 = s.predicted;
            s.var3 = 0.49f * kTypePrior / (kTypePrior + n) + 0.1f;
        }
    }

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
        CombatLearn::Forecast3 forecast3;   // the same from the sheets (the grimmer one once more came)
        Sheet heroSheet;                    // the hero's at its start
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
        const float rate = std::min(kMaxRate, s.damage / static_cast<float>(heroMax) / s.strikeSeconds);
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
            else if (int bin = 0; std::sscanf(line, "bad %d %d %f %f", &cls, &bin, &c.bias, &c.spread) == 4 &&
                                  cls >= 0 && cls < 10 && bin >= 0 && bin < 10)
            {
                g_bad[cls][bin] = {std::max(0.0f, c.bias), std::clamp(c.spread, 0.0f, std::max(0.0f, c.bias))};
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
                // Learned from misreads (above a quarter of the health a second): learned again from scratch.
                if (k.logRate > std::log(0.25f + kFloor))
                {
                    LOG("CombatLearn: {} learned at {:.0f} %/s, implausible: learned again", k.name,
                        std::exp(k.logRate) * 100.0f);
                    k.samples = 0;
                    k.logRate = 0.0f;
                }
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
            for (int cls = 0; cls < 10; ++cls)
            {
                for (int bin = 0; bin < 10; ++bin)
                {
                    if (const BadBin& b = g_bad[cls][bin]; b.fights > 0.0f)
                    {
                        std::fprintf(f, "bad %d %d %.0f %.0f\n", cls, bin, b.fights, b.bad);
                    }
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
                      "\"win\":%.3f,\"blowsPerSecond\":%.2f,\"bias\":%.3f,\"spread\":%.3f},",
                      static_cast<unsigned long>(now), (g_fight.lastAction - g_fight.start) / 1000.0f, heroLevel,
                      g_fight.heroMax, g_fight.heroStart, g_fight.heroMin, heroHealth,
                      g_fight.mounted ? "true" : "false", g_fight.mountMin, g_fight.mountMax, g_fight.potions,
                      g_fight.kills, g_fight.experience, g_fight.hirelingDeaths, g_fight.died ? "true" : "false",
                      g_fight.unexplained, g_fight.lostTotal, g_fight.dealt, g_fight.forecast.foes,
                      g_fight.forecast.loss, g_fight.forecast.seconds, g_fight.forecast.win, g_blowsPerSecond,
                      g_lossBias, g_lossSpread);
        line = head;
        {
            const Sheet& h = g_fight.heroSheet;
            const CombatLearn::Forecast3& f3 = g_fight.forecast3;
            char extra[640];
            std::snprintf(extra, sizeof(extra),
                          "\"forecast3\":{\"foes\":%d,\"sheets\":%s,\"win\":%.3f,\"loss\":%.3f,\"seconds\":%.1f,"
                          "\"hpLo\":%.3f,\"hpMid\":%.3f,\"hpHi\":%.3f,\"potions\":%d,\"decides\":%s},"
                          "\"heroSheet\":{\"ok\":%s,\"res\":[%.1f,%.1f,%.1f,%.1f],\"defence\":%.1f,\"attack\":%.1f,"
                          "\"attackSpeed\":%d,\"moveSpeed\":%d},\"foes\":[",
                          f3.foes, f3.fromSheets ? "true" : "false", f3.win, f3.loss, f3.seconds, f3.hpLo, f3.hpMid,
                          f3.hpHi, f3.potions, Config::autoAttack.forecast3 ? "true" : "false", h.ok ? "true" : "false",
                          h.res[0], h.res[1], h.res[2], h.res[3], h.defence, h.attack, h.attackSpeed, h.moveSpeed);
            line += extra;
        }
        bool first = true;
        for (const auto& [id, s] : g_fight.foes)
        {
            char foe[1024];
            const float rate = s.strikeSeconds > 0.0f && g_fight.heroMax > 0
                                   ? s.damage / g_fight.heroMax / s.strikeSeconds * 100.0f : 0.0f;
            const Sheet& e = s.sheet;
            std::snprintf(foe, sizeof(foe),
                          "%s{\"kind\":\"%s\",\"level\":%d,\"maxHealth\":%d,\"left\":%d,\"ranged\":%s,"
                          "\"strikeSeconds\":%.1f,\"damage\":%.0f,\"percentPerSecond\":%.2f,\"predicted\":%.2f,"
                          "\"toHirelings\":%d,\"hit\":%.1f,\"prior\":%.2f,\"rate3\":%.2f,\"var3\":%.2f,\"distance\":%.0f,"
                          "\"sheet\":{\"ok\":%s,\"dmgA\":[%.1f,%.1f,%.1f,%.1f],\"dmgB\":[%.1f,%.1f,%.1f,%.1f],"
                          "\"res\":[%.1f,%.1f,%.1f,%.1f],\"attack\":%.1f,\"defence\":%.1f,\"attackSpeed\":%d,"
                          "\"moveSpeed\":%d,\"side\":%d}}",
                          first ? "" : ",", typeName(s.type).c_str(), s.level, s.maxHealth, std::max(0, s.health),
                          s.ranged ? "true" : "false", s.strikeSeconds, s.damage, rate, s.predicted * 100.0f,
                          s.toHirelings, s.hit, s.prior >= 0.0f ? s.prior * 100.0f : -1.0f, s.rate3 * 100.0f, s.var3,
                          s.distance, e.ok ? "true" : "false", e.dmgA[0], e.dmgA[1], e.dmgA[2], e.dmgA[3], e.dmgB[0],
                          e.dmgB[1], e.dmgB[2], e.dmgB[3], e.res[0], e.res[1], e.res[2], e.res[3], e.attack, e.defence,
                          e.attackSpeed, e.moveSpeed, e.side);
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
        LOG("Forecast3: foreseen {:.0f}% lost ({:.0f}% to {:.0f}% of health left at the end), {:.0f}% to win, {} potions, "
            "{} enemies, sheets read: {}{}", g_fight.forecast3.loss * 100.0f, g_fight.forecast3.hpLo * 100.0f,
            g_fight.forecast3.hpHi * 100.0f, g_fight.forecast3.win * 100.0f, g_fight.forecast3.potions,
            g_fight.forecast3.foes, g_fight.forecast3.fromSheets ? "yes" : "no",
            Config::autoAttack.forecast3 ? " (it decides)" : "");
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
            if (f.health <= 0 || (anyEngaged && !f.atHero && !f.atHireling && !s.firstHurtAt && !f.soon))
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
        // Bounded (the live verifier, 2026-11-11: «351934% lost in 147 s», a kill rate near 0 stretching the clock):
        // ten times the hero's health and ten minutes at most, the fight foreseen lost all the same.
        fc.loss = std::min(fc.loss, 10.0f);
        fc.seconds = std::min(clock, 600.0f);
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
        fc.expected = fc.loss * std::exp(g_lossBias);
        g_forecast = fc;
    }

    CombatLearn::Forecast3 g_forecast3;
    constexpr float kPotionHeal = 0.25f;        // a healing potion's worth, share of max health (a guess; to measure)
    constexpr float kPotionCooldown = 10.0f;    // seconds between two (a guess)
    constexpr float kMeleeReach = 100.0f;       // world units within which a melee enemy strikes (a guess)

    // docs/Medicion-de-enemigos.md §2.2: the same enemies and kill order as computeForecast (Smith's rule), each with
    // forecast3's rate and the variance of its log; one that is not on the hero yet strikes only once it arrives (its
    // distance less the melee reach at its move speed; at once if it shoots or already aims); the loss a sum of
    // lognormals taken as one (Fenton-Wilkinson); the health with the potions it can drink in the fight.
    void computeForecast3(const CombatLearn::Look& look, float heroFraction)
    {
        struct Item
        {
            float rate, var, seconds, arrive;
        };
        std::vector<Item> items;
        CombatLearn::Forecast3 r;
        bool anyEngaged = false;
        for (int i = 0; i < look.count; ++i)
        {
            anyEngaged = anyEngaged || look.foes[i].atHero || look.foes[i].atHireling;
        }
        for (int i = 0; i < look.count; ++i)
        {
            const CombatLearn::Foe& f = look.foes[i];
            const Seen& s = g_seen[f.id];
            if (f.health <= 0 || (anyEngaged && !f.atHero && !f.atHireling && !s.firstHurtAt && !f.soon))
            {
                continue;
            }
            const float dps = s.hit > 0.0f ? s.hit * g_blowsPerSecond : 0.0f;
            const float seconds = dps > 0.0f ? std::min(600.0f, f.health / dps) : 600.0f;
            const bool there = f.atHero || f.atHireling || s.ranged;
            const float arrive = !there && s.sheet.ok && s.sheet.moveSpeed > 0
                                     ? std::max(0.0f, s.distance - kMeleeReach) / static_cast<float>(s.sheet.moveSpeed)
                                     : 0.0f;
            items.push_back({std::max(s.rate3, kFloor) * (s.summoner ? kSummonerWeight : 1.0f), s.var3, seconds, arrive});
            r.fromSheets = r.fromSheets || s.prior >= 0.0f;
        }
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b) { return a.rate * b.seconds > b.rate * a.seconds; });
        r.foes = static_cast<int>(items.size());
        float clock = 0.0f, mean = 0.0f, variance = 0.0f;
        for (const Item& it : items)
        {
            clock += it.seconds;
            const float m = it.rate * std::max(0.0f, clock - it.arrive);   // the median of its part of the loss
            if (m > 0.0f)
            {
                const float e = std::exp(it.var);
                const float mu = m * std::sqrt(e);
                mean += mu;
                variance += (e - 1.0f) * mu * mu;
            }
        }
        r.seconds = std::min(clock, 600.0f);
        const int left = Gear::healingLeft();
        r.potions = left > 0 ? std::min(left, static_cast<int>(r.seconds / kPotionCooldown) + 1) : 0;
        const float health = heroFraction + static_cast<float>(r.potions) * kPotionHeal;
        if (mean > 1e-4f && heroFraction > 0.0f)
        {
            const float s2 = std::log(1.0f + variance / (mean * mean));
            const float sd = std::sqrt(std::max(s2, 1e-6f));
            const float mu = std::log(mean) - s2 * 0.5f;
            r.win = normalCdf((std::log(health) - mu) / sd);
            r.loss = std::min(std::exp(mu), 10.0f);
            r.hpMid = health - r.loss;
            r.hpLo = health - std::min(std::exp(mu + 1.2816f * sd), 10.0f);
            r.hpHi = health - std::min(std::exp(mu - 1.2816f * sd), 10.0f);
        }
        else
        {
            r.win = heroFraction > 0.0f ? 1.0f : 0.0f;
            r.hpLo = r.hpMid = r.hpHi = heroFraction;
        }
        g_forecast3 = r;
        if (Config::autoAttack.forecast3 && r.foes > 0)
        {
            g_forecast.win = r.win;
            g_forecast.loss = r.loss;
            g_forecast.seconds = r.seconds;
            g_forecast.expected = r.loss;
        }
    }

    // A fight over: its loss against the forecast made at its start (the bias and spread of the log error), and the
    // hero's blows per second from the health the enemies lost.
    uint32_t g_heroGold = 0;    // the hero's gold at the last look (for the death's log)

    void learnForecast(float seconds)
    {
        if (g_fight.heroMax <= 0)
        {
            return;
        }
        // The class's record of fights gone bad, by the win foreseen at the start (or the grimmer one once more came);
        // before the death's branch below, which returns.
        if (g_class >= 0 && g_class < 10 && g_fight.forecast.foes > 0)
        {
            const int bin = std::clamp(static_cast<int>(g_fight.forecast.win * 10.0f), 0, 9);
            const bool bad = g_fight.died || g_fight.heroMin * 5 < g_fight.heroMax;
            const std::lock_guard lock(g_lock);
            g_bad[g_class][bin].fights += 1.0f;
            g_bad[g_class][bin].bad += bad ? 1.0f : 0.0f;
            g_dirty = true;
            if (bad)
            {
                LOG("CombatLearn: a fight gone bad at {:.0f}% foreseen to win: class {} now {:.1f}% bad there ({:.0f} "
                    "fights of its own)",
                    g_fight.forecast.win * 100.0f, g_class, binBad(g_class, bin) * 100.0f, g_bad[g_class][bin].fights);
            }
        }
        // Punished for dying (the player, 2026-11-10: «castiga al agente por morir»; on Bronze it never should): the
        // forecast grows much more careful (the error counted as at least a doubled loss, three times over, so later
        // fights like this one read as lost sooner and the retreat comes earlier), and the kinds that struck it are
        // learned as hitting half again as hard.
        if (g_fight.died)
        {
            const float actual = std::max(g_fight.lostTotal / g_fight.heroMax, 1.0f);
            const float error = g_fight.forecast.loss > 0.01f
                                    ? std::clamp(std::log(actual) - std::log(g_fight.forecast.loss), 0.7f, 4.0f) : 1.0f;
            const float before = g_lossBias;
            for (int i = 0; i < 3; ++i)
            {
                g_lossSamples = std::min(g_lossSamples + 1, 100);
                g_lossBias += (error - g_lossBias) / static_cast<float>(g_lossSamples + 8);
            }
            int kinds = 0;
            {
                const std::lock_guard lock(g_lock);
                for (const auto& [id, foe] : g_fight.foes)
                {
                    if (foe.strikeSeconds > 0.0f)
                    {
                        if (const auto k = g_kinds.find(foe.type); k != g_kinds.end())
                        {
                            k->second.logRate += std::log(1.5f);
                            ++kinds;
                        }
                    }
                }
            }
            g_dirty = true;
            LOG("CombatLearn: the hero died: punished (forecast bias {:.2f} -> {:.2f}, {} enemy kinds learned as harder; "
                "{} healing potions, {} gold)", before, g_lossBias, kinds, Gear::healingLeft(), g_heroGold);
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
    if (hero)
    {
        g_heroGold = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(hero) + 0x3A8 + 0x46);
    }
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
    // Not across a change of the maximum (a transformation, a level, a load): the drop is no blow. Never more than the
    // maximum in one look.
    static int s_lastMax = 0;
    const bool sameMax = s_lastMax == heroMax;
    s_lastMax = heroMax;
    const float lost = dt > 0.0f && !potion && sameMax
                           ? static_cast<float>(std::min(heroMax, std::max(0, g_lastHealth - heroHealth))) : 0.0f;
    float weights = 0.0f;
    bool engaged = false;
    struct Sighting
    {
        uint32_t type;
        int32_t x, y;
    };
    std::vector<Sighting> sightings;    // enemies met for the first time: on the agent's map (outside g_lock)
    if (look.hero)
    {
        readSheet(look.hero, g_heroSheet);
    }
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
            s.predicted = std::clamp(std::exp(predictLog(s, heroMax, heroLevel)) - kFloor, 0.0f, kMaxRate);
            if (cObject* o = cObjectManager::instance() ? cObjectManager::instance()->object(f.id) : nullptr)
            {
                if (!s.sheet.ok)
                {
                    readSheet(o, s.sheet);
                }
                if (look.hero)
                {
                    s.distance = std::hypot(static_cast<float>(o->x - look.hero->x), static_cast<float>(o->y - look.hero->y));
                }
            }
            priorAndRate(s, heroMax);
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
    computeForecast3(look, g_heroFraction);

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
            g_fight.forecast3 = g_forecast3;
            g_fight.heroSheet = g_heroSheet;
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
        if (g_forecast3.foes > g_fight.forecast3.foes && g_forecast3.win < g_fight.forecast3.win)
        {
            g_fight.forecast3 = g_forecast3;
        }
        g_fight.lastAction = now;
    }
    if (g_inFight)
    {
        g_fight.heroMin = std::min(g_fight.heroMin, heroHealth);
        g_fight.heroMax = heroMax;
        // Dead and back (seen live 2026-11-10, the Daemon: 7 -> 139 health in one look, no potion): the death the
        // game never shows as health 0 to the looks.
        static int s_prevHealth = 0;
        const bool respawned = s_prevHealth > 0 && s_prevHealth * 100 <= heroMax * 15 && heroHealth * 100 >= heroMax * 90;
        s_prevHealth = heroHealth;
        g_fight.died = g_fight.died || respawned || heroHealth <= 0 || CombatState::action(hero) == CombatState::Dead;
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

int CombatLearn::fightPotions()
{
    return g_inFight ? g_fight.potions : 0;
}

float CombatLearn::secondsToDie()
{
    return g_incoming > 1e-4f ? g_heroFraction / g_incoming : 1e6f;
}

const CombatLearn::Forecast& CombatLearn::forecast()
{
    return g_forecast;
}

const CombatLearn::Forecast3& CombatLearn::forecast3()
{
    return g_forecast3;
}

int CombatLearn::killRank(uint32_t id)
{
    const auto it = g_killRank.find(id);
    return it != g_killRank.end() ? it->second : -1;
}

float CombatLearn::badChance(const Forecast& fc)
{
    if (fc.foes <= 0)
    {
        return 0.0f;
    }
    // Between the middles of the tenths (0.05, 0.15 ... 0.95), not a step at each tenth.
    const float w = std::clamp(fc.win * 10.0f - 0.5f, 0.0f, 9.0f);
    const int lo = static_cast<int>(w), hi = std::min(lo + 1, 9);
    const std::lock_guard lock(g_lock);
    const int cls = g_class >= 0 && g_class < 10 ? g_class : 0;
    const float a = binBad(cls, lo), b = binBad(cls, hi);
    return a + (b - a) * (w - static_cast<float>(lo));
}

float CombatLearn::fightSeconds()
{
    return g_inFight && g_lastLook >= g_fight.start ? (g_lastLook - g_fight.start) / 1000.0f : 0.0f;
}
