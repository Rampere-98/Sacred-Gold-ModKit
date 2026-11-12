#pragma once

#include <windows.h>

// The hero's gear, as a player tends it ([Gear], ModKit menu): out of a fight it puts on what in the backpack is
// better than what it wears (each equipment slot, by the item's weapon damage or armor, its value deciding the rest),
// and at a merchant it sells what it will not wear (worse than what it has for every slot it fits, or not for its
// class) and buys what would be better, keeping a share of its gold. All through the game's own functions (equip,
// the backpack, the price and the gold, which keeps its anti-cheat copies right), as the inventory and shop windows
// do. Quest items, and items that fit no equipment slot (potions, runes, gems...), are never sold.
namespace Gear
{
    // Every frame of the auto attack, while the game world runs; `calm`: no fight on and nothing being collected.
    void onFrame(DWORD now, bool calm);

    // Merchant trips ([Gear] Trips): the backpack is full, the agent should take the hero to a merchant.
    bool wantsShop();
    // Runes of other classes in the backpack, and whether to take them to a combo master: three or more and gold for
    // the swap (three of them make one of the hero's own class).
    int foreignRunes();
    bool wantsRuneSwap();
    bool wantsCombo();
    bool wantsSocket();
    bool wantsHorse();
    // A horse's worth, to compare two (Manolito, block H2b, seen live): its level (u16 +0x3FE, 1..25) first, its
    // speed (the stats block +0x3A8, u16 +0x40, 140..220) to break ties. 0 for none.
    inline int horseWorth(const void* creature)
    {
        if (!creature)
        {
            return 0;
        }
        const auto* c = static_cast<const unsigned char*>(creature);
        return *reinterpret_cast<const unsigned short*>(c + 0x3FE) * 1000 + *reinterpret_cast<const unsigned short*>(c + 0x3A8 + 0x40);
    }
    // A stable near offers a horse clearly better than the hero's (two levels or more): a trip to buy it.
    void setHorseUpgrade(bool better);         // no horse yet: one from a stable        // something to socket at a smith         // a combo to learn at a combo master

    // Healing potions in the backpack at the last look (-1 not counted yet).
    int healingLeft();
    // The backpack has no room left (whatever the trips setting): the agent leaves items lying.
    bool packFull();
    // The agent opened a shop for the trip: sold to, upgrades put on (whatever the menu says), then done.
    void tripOpened();
    // The trip's shop has nothing more (once): the agent closes it.
    bool tripDone();
    bool shopOpen();
    // A combo master's window open (its runes' page shows the inventory beside it: not to be closed meanwhile).
    bool craftsmanOpen();

    // An item's grade as the game colours it (Manolito, AGENT_ADDRESSES «Color = rareza»: the nibble info+0x06 & 0xF,
    // <=4 white, 5..0xB blue, 0xC..0xE yellow, 0xF gold/unique; a set piece green): 0 white, 1 blue, 2 yellow,
    // 3 gold, 4 set. -1: no equipment (potions, runes, gold...) or not read.
    // equipment: the caller knows it is (by its class), so the grade is read without the damage/armour test (an
    // amulet has neither).
    int grade(uint32_t id, bool equipment = false);

    // A new enemy taken: the weapon slot that deals it the most (each slot's weapons by kind, the hero's multipliers,
    // the enemy's resistances), switched to as the keys 1-5 do when it beats the active one by 15 %. True if switched.
    // Call with the hero at rest (no art under way).
    bool chooseWeaponFor(uint32_t targetId);
    // The active weapon slot's expected blow against creature `targetId` (its resistances and the chance to hit it
    // counted); -1 if unknown.
    float expectedHit(uint32_t targetId);
}
