#pragma once

// [AutoAttack] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct AutoAttack
    {
        bool enabled = false;   // on when the game starts; the key toggles it either way
        int key = 0x7A;         // virtual-key code of the toggle key (0x7A = F11)
        int range = 400;        // enemies within this many screen pixels of the hero
        int pauseMs = 700;      // no auto attack for this long after the player's own mouse click
        bool pickup = true;     // with no enemy in range, pick up items and open chests and other containers
        int lootRange = 800;    // ... within this many screen pixels of the hero
        int repeatMs = 1000;    // the attack order again this often while the target lives; 0 = once per target
    };
    inline AutoAttack autoAttack;
}
