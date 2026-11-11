#pragma once

// Each class its own mind (the player, 2026-11-10: «quiero que cada personaje tenga su propia mentalidad»): the
// values of docs/Mentalidades.md (the class strategist), read through the class of the character controlled. Placeholders
// to be measured (near-deaths per 100 fights, experience an hour); the first part wired: how far above its level it
// goes, when it steps back, the potions it keeps, the horse in a fight.

#include "hero_profile.h"

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
        bool fightMounted;      // stays up with an enemy near (casters, bows)
        float dismountTiles;    // gets off with an enemy this near
    };

    inline const Values& of(HeroProfile::Class cls)
    {
        using C = HeroProfile::Class;
        //                                   quest farm retr lost dry  win    pots mounted off
        // Farming zones a level nearer the hero's (the live verifier, 2026-11-11: the Seraphim, level 2-3, sent to
        // zones of 4-7 three times and dead there three times).
        static constexpr Values kSeraphim  {2,    1,   30,  60,  85,  0.5f,  6,   true,   3.0f};
        static constexpr Values kGladiator {3,    2,   25,  60,  85,  0.5f,  8,   false, 12.0f};
        static constexpr Values kMage      {1,    0,   35,  70,  90,  0.6f,  5,   true,   3.0f};
        static constexpr Values kWoodElf   {2,    1,   35,  70,  90,  0.6f,  6,   true,   3.0f};
        static constexpr Values kDarkElf   {2,    1,   30,  60,  85,  0.5f,  8,   false, 12.0f};
        static constexpr Values kVampiress {2,    1,   30,  65,  85,  0.5f,  8,   false, 12.0f};
        static constexpr Values kDwarf     {3,    2,   25,  55,  80,  0.5f,  8,   false, 12.0f};
        static constexpr Values kDaemon    {3,    2,   25,  60,  85,  0.5f,  8,   false, 12.0f};
        static constexpr Values kUnknown   {2,    2,   25,  60,  85,  0.5f,  5,   false, 12.0f};
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
}
