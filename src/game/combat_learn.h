#pragma once

#include <windows.h>
#include <cstdint>

namespace Sacred
{
    struct cCreature;
}

// What the auto attack learns in battle. Each fight is written to SacredBild-combat.log (one JSON line: how long, the
// health lost and the lowest point, potions, kills, experience, the hirelings, each enemy with what it did), mounted
// or not. From the same looks it learns how hard each enemy hits: the health the hero loses while enemies strike it is
// shared among them, and each enemy's rate (a share of the hero's maximum health per second) teaches a small model
// on relative features (its maximum health against the hero's, its level against the hero's, whether it shoots from
// afar), so what it learns from a few kinds holds for others; the kinds met often keep their own measure on top
// (the encyclopedia, in the ModKit menu). Kept in SacredBild-learn.txt beside the game, learned on in every fight.
namespace CombatLearn
{
    struct Foe
    {
        uint32_t id = 0;
        int health = 0, maxHealth = 0;
        bool atHero = false;        // aiming at the hero (or what it rides)
        bool atHireling = false;
        bool ranged = false;        // aiming at the hero from beyond the melee look after look
    };

    struct Look
    {
        DWORD now = 0;
        Sacred::cCreature* hero = nullptr;
        Sacred::cCreature* self = nullptr;      // what the player controls: the hero, or its mount
        const Foe* foes = nullptr;
        int count = 0;
        const uint32_t* hirelings = nullptr;
        int hirelingCount = 0;
    };

    // At each look of the auto attack.
    void observe(const Look& look);

    // How hard enemy `id` (seen at the last look) hits: the share of the hero's maximum health per second it takes,
    // as learned; 0 if it is unknown.
    float danger(uint32_t id);
    // An enemy kind (type id) in the encyclopedia: its learned rate (share of the hero's maximum health per second, -1
    // not yet learned) and how many were killed. False if it is not in it.
    bool kind(uint32_t type, float& rate, int& killed);
    // The same against the middle of the enemies around (1: an ordinary one, 2: twice as hard); 1 if unknown.
    float relativeDanger(uint32_t id);
    // At the rate those striking the hero now hit, seconds until its health is gone (a large number for none).
    float secondsToDie();

    // Its ModKit menu section (the encyclopedia); once.
    void install();
}
