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
        bool soon = false;          // within its aggro reach of the hero (Manolito, block N: 600 u): joins the fight
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
    // Healing potions drunk in the fight going on (0 out of a fight).
    int fightPotions();

    // The forecast (the player's wish, 2026-11-10: foresee each fight's outcome from the build against the enemies).
    // Each enemy around: how fast the hero kills it (the active slot's expected blow through its resistances, times the
    // hero's effective blows per second, learned fight by fight) and how hard it hits (the learned rate). Killed in the
    // order that loses the least health (Smith's rule: the highest rate per second needed to kill it first), the
    // expected loss is the sum of each one's rate over the time it lives; against the health the hero has, with the
    // spread learned from the past forecasts against what happened, the chance to win. Written into each fight's line
    // of SacredBild-combat.log («forecast») to be measured.
    struct Forecast
    {
        int foes = 0;
        float loss = 0.0f;          // expected health lost, a share of the maximum
        float seconds = 0.0f;       // until all are down
        float win = 1.0f;           // chance the hero's health lasts (0..1)
        float expected = 0.0f;      // the loss corrected by the learned bias (the median of what fights like it cost)
        uint32_t first = 0;         // the enemy to kill first
    };
    // At the last look.
    const Forecast& forecast();
    // The rank of enemy `id` in the forecast's kill order (0: first; -1: not in it).
    int killRank(uint32_t id);
    // The chance a fight like `fc` goes bad (near death, under 20 % of health, or death) for the hero's class, by
    // its own record per tenth of the forecast's win (a prior of 20 fights per tenth from docs/Medidores/
    // Prevision.md §2, 22 034 fights of the 8 copies); linear between the tenths' middles. 0 with no enemy.
    float badChance(const Forecast& fc);
    // Seconds since the fight under way began (0 out of a fight).
    float fightSeconds();
    // The forecast from the enemies' own sheets (docs/Medicion-de-enemigos.md §2.2): each enemy's blows on this hero
    // read from its stats (damage through the hero's resistances, its attack against the hero's defence, its attack
    // speed) and mixed with what its kind was measured to deal; when each comes in; the potions' worth; the loss as a
    // sum of lognormals (Fenton-Wilkinson). Worked out beside the one above at each look and logged; it decides only
    // with [AutoAttack] Forecast3=1 (then forecast() carries its win, loss and seconds).
    struct Forecast3
    {
        int foes = 0;
        bool fromSheets = false;    // at least one enemy's sheet read
        float win = 1.0f;           // chance the loss stays under the health and the potions' worth
        float loss = 0.0f;          // median loss, a share of the maximum
        float hpLo = 1.0f, hpMid = 1.0f, hpHi = 1.0f;   // health left at the end, p10 / p50 / p90 (share; < 0: dead)
        float seconds = 0.0f;       // until all are down
        int potions = 0;            // counted in the health
    };
    // At the last look.
    const Forecast3& forecast3();

    // Its ModKit menu section (the encyclopedia); once.
    void install();
}
