#pragma once
#include "game/aim_assist.h"

#include <windows.h>
#include <cstdint>

// Combat arts and consumables for the auto attack ([Tactics]). Each art slot (keys 6-0) is used by what it does,
// learned from its effect (health regained, enemies hurt) or set in [Tactics] / the ModKit menu: heals only when
// the hero misses health, area arts only with several enemies around the target, single blows only at enemies
// worth it (not those the plain attack kills in a moment), buffs at the start of a hard fight. An art is cast as
// its right click does: the slot key selects it, then the game's own order 0x105 on the target; the mouse stays
// free. Consumables are drunk through the game's usePotion: Mentor in big fights, Undead Death when hard pressed,
// Antidote when health drains with no enemy at hand (poison), Concentration when the arts wanted are recharging,
// a healing potion to the hirelings when one is badly hurt. Healing potions for the hero stay [AutoPotion]'s.
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
}
