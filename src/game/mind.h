#pragma once

// Each class its own mind (the player, 2026-11-10: «quiero que cada personaje tenga su propia mentalidad»): the
// values of docs/Mentalidades.md (the class strategist), read through the class of the character controlled. Placeholders
// to be measured (near-deaths per 100 fights, experience an hour); the first part wired: how far above its level it
// goes, when it steps back, the potions it keeps, the horse in a fight.

#include "hero_profile.h"
#include "config/auto_attack.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

namespace Mind
{
    struct Values
    {
        int questLevelSlack;    // levels a quest's ground may lie above the hero's before it farms first
        int farmLevelSlack;     // levels a farming zone may lie above the hero's
        int retreatPercent;     // health % to step back at with no potion
        int lostHealthPct;      // health % below which a fight foreseen lost is left
        int lostHealthDryPct;   // the same with no potion and five or more about
        float lostWin;          // the forecast's win below which a fight is lost
        int potionStock;        // healing potions to keep
        // Everyone fights from the saddle (the player, 2026-11-11: «montado a caballo puede pegar más rápido y
        // resiste mejor todos los golpes; solo tiene la desventaja de recoger más lentamente»): 0 never gets off.
        bool fightMounted;      // stays up with an enemy near
        float dismountTiles;    // gets off with an enemy this near (0: never)
        // From the class guides (docs/Mentalidades.md §2-3, the player, 2026-11-13: «que el agente se comporte tal
        // cual recomiendan las guías»): the art cast when health falls below panicPercent, potions or not; the
        // enemies that may fight the hero at once, with potions and without.
        int panicPercent;
        uint32_t panicArt;
        int maxFoes, maxFoesDry;
    };

    inline const Values& of(HeroProfile::Class cls)
    {
        using C = HeroProfile::Class;
        //                                   quest farm retr lost dry  win    pots mounted off   panic art  foes dry
        // Farming zones a level nearer the hero's (the live verifier, 2026-11-11: the Seraphim, level 2-3, sent to
        // zones of 4-7 three times and dead there three times).
        static constexpr Values kSeraphim  {2,    1,   30,  60,  85,  0.5f,  6,   true,   0.0f, 35, 0x10, 6, 4};
        static constexpr Values kGladiator {3,    2,   25,  60,  85,  0.5f,  8,   true,   0.0f, 35, 0x3F6, 9, 5};
        static constexpr Values kMage      {1,    0,   35,  70,  90,  0.6f,  5,   true,   0.0f, 40, 0x1, 4, 3};
        static constexpr Values kWoodElf   {2,    1,   35,  70,  90,  0.6f,  6,   true,   0.0f, 40, 0x24, 5, 3};
        static constexpr Values kDarkElf   {2,    1,   30,  60,  85,  0.5f,  8,   true,   0.0f, 35, 0x413, 7, 4};
        static constexpr Values kVampiress {2,    1,   30,  65,  85,  0.5f,  8,   true,   0.0f, 35, 0x408, 7, 4};
        static constexpr Values kDwarf     {3,    2,   25,  55,  80,  0.5f,  8,   true,   0.0f, 30, 0x43B, 8, 5};
        static constexpr Values kDaemon    {3,    2,   25,  60,  85,  0.5f,  8,   true,   0.0f, 35, 0x43, 7, 4};
        static constexpr Values kUnknown   {2,    2,   25,  60,  85,  0.5f,  5,   true,   0.0f, 30, 0, 6, 4};
        switch (cls)
        {
        case C::Seraphim: return kSeraphim;
        case C::Gladiator: return kGladiator;
        case C::BattleMage: return kMage;
        case C::WoodElf: return kWoodElf;
        case C::DarkElf: return kDarkElf;
        case C::Vampiress:
        case C::VampiressNight: return kVampiress;
        case C::Dwarf: return kDwarf;
        case C::Daemon: return kDaemon;
        default: return kUnknown;
        }
    }

    // The character controlled now.
    inline const Values& now()
    {
        return of(HeroProfile::current().cls);
    }

    // The class's combat plan (the player, 2026-11-14: «el gladiador no puede compartir con mago, pues es totalmente
    // diferente la forma en la que batallan»; docs/Planes-de-combate.md): data the agent reads, one file a class beside
    // the exe, ClassPlans\<Class>.ini. Step 1 of the migration: the pick of the enemy ([Targets], decide's cost
    // multipliers: lower goes sooner) and the distance kept ([Distance], keepDistance). Every value defaults to what the
    // code did before, so a missing file or key changes nothing; with [AutoAttack] ClassPlan=0 the defaults are used
    // whatever the file says.
    struct Plan
    {
        float closing = 0.7f;           // the one coming at the hero
        float sniper = 0.6f;            // shooting at the hero from afar, nobody in the melee
        float shooterInMelee = 0.5f;    // its arrows seen at the hero, others in the melee
        float shooterAlone = 0.35f;     // the same, nobody in the melee
        float atHeroStrong = 0.45f, atHero = 0.6f;          // striking the hero in the melee
        float atHirelingStrong = 0.6f, atHireling = 0.75f;  // striking a hireling
        float woundedFloor = 0.7f;      // the cost at its last breath (1 unhurt): the wounded focused down
        float hardnessPower = 1.5f;     // among those striking: how hard it hits, weighed over its health left
        float killFirst = 0.6f;         // the forecast's first in the kill order
        float stay = 0.4f;              // the enemy fought now (no back and forth)
        int kite = -1;                  // -1 as before (a ranged weapon or the Battle Mage), 0 never, 1 always
        float tooCloseTiles = 3.0f;     // a step back when an enemy comes this near
        float stepTiles = 4.0f;         // how far each step back goes
        int stepsPer30s = 4;            // cornered after this many (keepDistance counts four at most)
        bool loaded = false;            // read from its file
    };

    namespace detail
    {
        struct PlanKey
        {
            const wchar_t* section;
            const wchar_t* name;
            const char* label;
            float Plan::*f;
            int Plan::*i;
            float lo, hi;
        };
        inline const PlanKey kPlanKeys[] = {
            {L"Targets", L"Closing", "Targets.Closing", &Plan::closing, nullptr, 0.05f, 2.0f},
            {L"Targets", L"Sniper", "Targets.Sniper", &Plan::sniper, nullptr, 0.05f, 2.0f},
            {L"Targets", L"ShooterInMelee", "Targets.ShooterInMelee", &Plan::shooterInMelee, nullptr, 0.05f, 2.0f},
            {L"Targets", L"ShooterAlone", "Targets.ShooterAlone", &Plan::shooterAlone, nullptr, 0.05f, 2.0f},
            {L"Targets", L"AtHeroStrong", "Targets.AtHeroStrong", &Plan::atHeroStrong, nullptr, 0.05f, 2.0f},
            {L"Targets", L"AtHero", "Targets.AtHero", &Plan::atHero, nullptr, 0.05f, 2.0f},
            {L"Targets", L"AtHirelingStrong", "Targets.AtHirelingStrong", &Plan::atHirelingStrong, nullptr, 0.05f, 2.0f},
            {L"Targets", L"AtHireling", "Targets.AtHireling", &Plan::atHireling, nullptr, 0.05f, 2.0f},
            {L"Targets", L"WoundedFloor", "Targets.WoundedFloor", &Plan::woundedFloor, nullptr, 0.1f, 1.0f},
            {L"Targets", L"HardnessPower", "Targets.HardnessPower", &Plan::hardnessPower, nullptr, 0.0f, 3.0f},
            {L"Targets", L"KillFirst", "Targets.KillFirst", &Plan::killFirst, nullptr, 0.05f, 2.0f},
            {L"Targets", L"Stay", "Targets.Stay", &Plan::stay, nullptr, 0.05f, 1.0f},
            {L"Distance", L"Kite", "Distance.Kite", nullptr, &Plan::kite, -1.0f, 1.0f},
            {L"Distance", L"TooCloseTiles", "Distance.TooCloseTiles", &Plan::tooCloseTiles, nullptr, 1.0f, 12.0f},
            {L"Distance", L"StepTiles", "Distance.StepTiles", &Plan::stepTiles, nullptr, 1.0f, 10.0f},
            {L"Distance", L"StepsPer30s", "Distance.StepsPer30s", nullptr, &Plan::stepsPer30s, 0.0f, 4.0f},
        };
        // Keys the later steps read (docs/Planes-de-combate.md §1): not an error in these two sections.
        inline const wchar_t* const kLaterKeys[] = {L"HoldTiles", L"CloseWeaponCornered", L"Healer", L"Summoner",
                                                    L"Weakest"};

        inline const wchar_t* planFile(HeroProfile::Class cls)
        {
            using C = HeroProfile::Class;
            switch (cls)
            {
            case C::Seraphim: return L"Seraphim";
            case C::Gladiator: return L"Gladiator";
            case C::BattleMage: return L"BattleMage";
            case C::DarkElf: return L"DarkElf";
            case C::WoodElf: return L"WoodElf";
            case C::Vampiress:
            case C::VampiressNight: return L"Vampiress";
            case C::Dwarf: return L"Dwarf";
            case C::Daemon: return L"Daemon";
            default: return nullptr;
            }
        }

        inline std::string narrow(const wchar_t* s)
        {
            std::string out;
            for (; s && *s; ++s)
            {
                out += *s < 128 ? static_cast<char>(*s) : '?';
            }
            return out;
        }

        inline Plan loadPlan(HeroProfile::Class cls)
        {
            Plan p;
            const wchar_t* name = planFile(cls);
            if (!name)
            {
                return p;
            }
            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring path = exe;
            path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"ClassPlans\\" + name + L".ini";
            const std::string file = "ClassPlans\\" + narrow(name) + ".ini";
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                LOG("ClassPlan: no {} for {}: the code's own values", file, HeroProfile::className(cls));
                return p;
            }
            int read = 0, clamped = 0, unknown = 0;
            for (const PlanKey& k : kPlanKeys)
            {
                wchar_t value[64] = {};
                if (!GetPrivateProfileStringW(k.section, k.name, L"", value, 64, path.c_str()))
                {
                    continue;
                }
                wchar_t* end = nullptr;
                const float v = std::wcstof(value, &end);
                if (end == value || !std::isfinite(v))
                {
                    LOG("ClassPlan: {} {} is not a number: the code's own value kept", file, k.label);
                    continue;
                }
                const float c = std::clamp(v, k.lo, k.hi);
                if (c != v)
                {
                    ++clamped;
                    LOG("ClassPlan: {} {} = {} out of [{}, {}]: {} taken", file, k.label, v, k.lo, k.hi, c);
                }
                if (k.f)
                {
                    p.*(k.f) = c;
                }
                else
                {
                    p.*(k.i) = static_cast<int>(std::lround(c));
                }
                ++read;
            }
            // Keys these sections do not know (a typo would else go unseen).
            for (const wchar_t* section : {L"Targets", L"Distance"})
            {
                wchar_t buffer[4096] = {};
                GetPrivateProfileSectionW(section, buffer, 4096, path.c_str());
                for (const wchar_t* line = buffer; *line; line += std::wcslen(line) + 1)
                {
                    const wchar_t* eq = std::wcschr(line, L'=');
                    const std::wstring key(line, eq ? static_cast<size_t>(eq - line) : std::wcslen(line));
                    if (key.empty() || key[0] == L';')
                    {
                        continue;
                    }
                    bool known = false;
                    for (const PlanKey& k : kPlanKeys)
                    {
                        known = known || (_wcsicmp(k.section, section) == 0 && _wcsicmp(k.name, key.c_str()) == 0);
                    }
                    for (const wchar_t* later : kLaterKeys)
                    {
                        known = known || _wcsicmp(later, key.c_str()) == 0;
                    }
                    if (!known)
                    {
                        ++unknown;
                        LOG("ClassPlan: {} [{}] {}: unknown key, left out", file, narrow(section), narrow(key.c_str()));
                    }
                }
            }
            p.loaded = true;
            LOG("ClassPlan: {} from {}: {} values, {} clamped, {} unknown", HeroProfile::className(cls), file, read,
                clamped, unknown);
            return p;
        }
    }

    // The plan of the character controlled now: loaded once per class; the defaults with ClassPlan=0.
    inline const Plan& plan()
    {
        static const Plan kDefaults;
        if (!Config::autoAttack.classPlan)
        {
            return kDefaults;
        }
        static Plan s_plans[10];
        static bool s_loaded[10] = {};
        const HeroProfile::Class cls = HeroProfile::current().cls;
        const int i = std::clamp(static_cast<int>(cls), 0, 9);
        if (!s_loaded[i])
        {
            s_plans[i] = detail::loadPlan(cls);
            s_loaded[i] = true;
        }
        return s_plans[i];
    }
}
