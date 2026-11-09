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
    // The backpack has no room left (whatever the trips setting): the agent leaves items lying.
    bool packFull();
    // The agent opened a shop for the trip: sold to, upgrades put on (whatever the menu says), then done.
    void tripOpened();
    // The trip's shop has nothing more (once): the agent closes it.
    bool tripDone();
    bool shopOpen();

    // An item's grade as the game colours it (Manolito, AGENT_ADDRESSES «Color = rareza»: the nibble info+0x06 & 0xF,
    // <=4 white, 5..0xB blue, 0xC..0xE yellow, 0xF gold/unique; a set piece green): 0 white, 1 blue, 2 yellow,
    // 3 gold, 4 set. -1: no equipment (potions, runes, gold...) or not read.
    int grade(uint32_t id);
}
