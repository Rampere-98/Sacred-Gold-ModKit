#pragma once
#include "game/aim_assist.h"

#include <windows.h>
#include <cstdint>
#include <vector>

// Combat arts and consumables for the auto attack ([Tactics]). Each art slot (keys 6-0) is used by what it does,
// learned from its effect (health regained, enemies hurt) or set in [Tactics] / the ModKit menu: heals only when
// the hero misses health, area arts only with several enemies around the target, single blows at enemies worth
// more than a few blows (with SingleAlways; else only strong ones), buffs at the start of a hard fight. An art is
// cast as its right click does: the slot key selects it (checked in the engine before the cast), then the game's own
// order 0x105 on the target; the mouse stays free. Whether the game took the cast is seen in the hero's action (9
// while an art is under way); a refused cast means the art still recharges, which teaches each art's recharge time.
// No other order goes out while the art lasts, and its effect is measured over exactly that time, without the
// healing of a potion drunk meanwhile. Consumables are drunk through the game's usePotion: Mentor in big fights,
// Undead Death when hard pressed, Antidote when health drains with no enemy at hand (poison), Concentration when the
// arts wanted were refused as recharging, a healing potion to the hirelings when one is badly hurt; one found
// missing is looked for again ever later. Healing potions for the hero stay [AutoPotion]'s.
namespace Tactics
{
    // What the auto attack sees, at each look at the situation.
    struct Situation
    {
        DWORD now = 0;
        int health = 0, maxHealth = 0;
        bool hurt = false;          // lost health lately
        bool threat = false;        // enemies close, closing in or hurting the hero
        uint32_t target = 0;        // the enemy fought, 0 for none
        const AimAssist::Foe* foes = nullptr;
        int count = 0;
        float hx = 0.0f, hy = 0.0f; // the hero on the screen
        float range = 0.0f;         // [AutoAttack] Range
        const uint32_t* hirelings = nullptr;    // ids of the creatures that follow the hero (AutoAttack tells)
        int hirelingCount = 0;
    };

    // Resolves the game functions it needs (if they don't match it never acts) and adds its ModKit menu section.
    void install();

    // At each look at the situation: may start an art or drink a consumable.
    void think(const Situation& s);

    // Every frame: the art under way (select, cast, measure).
    void onFrame(DWORD now);

    // An art is being cast: no other order before this is over, it would cut it short.
    bool busy(DWORD now);

    // The auto attack stopped (off, a click of the player's, a menu): drops the art under way.
    void reset();

    // Arts in the hero's slots, learned, that the game does not let him cast on horseback (the player, 2026-11-11:
    // «no todas las habilidades pueden ser usadas a caballo»): a hard fight is fought on foot then.
    int artsBarredMounted();

    // The Vampiress in her vampire form (the player, 2026-11-12: «al transformarse no debería montar a caballo, solo
    // cuando no está transformada»): bit 0x2000 of hero+0x14 (Manolito, block AH).
    bool vampireForm(unsigned long now);

    // The Vampiress has a fight and would turn into the vampire (the player, 2026-11-14: «en batalla transformada, sin
    // batalla vampiresa normal»): AutoAttack takes her off the horse first.
    bool vampireTurnWanted();

    // A learned art or combo as the bar's arrangement sees it (HeroBuild, docs/Artes-de-la-barra.md).
    struct ArtFact
    {
        uint16_t slotKind = 0;          // what a bar slot holds for it: 2 spell, 3 weapon art, 4 combo
        uint32_t art = 0;               // the slot's data: the art's id, a combo's 0x200 + index
        int level = 0;
        int use = 0;                    // Config::Tactics::Art: 0 not known yet, 1 heal, 2 area, 3 single, 4 buff, 5 off
        unsigned long rechargeMs = 0;   // measured (0: not yet)
        int tries = 0;
        bool mounted = false;           // the game lets it be cast on horseback
    };
    // Every learned art and combo, refreshed every 5 s from the auto attack's looks (a copy: any thread); false before
    // the first.
    bool artFacts(std::vector<ArtFact>& out);
}
