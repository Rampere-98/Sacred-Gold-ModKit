#pragma once

// [AutoAttack] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct AutoAttack
    {
        bool enabled = false;   // on when the game starts; the key toggles it either way
        int key = 0x7A;         // virtual-key code of the toggle key (0x7A = F11)
        int range = 400;        // enemies within this many screen pixels of the hero
        bool seek = false;      // also go after any enemy within range; off: only those attacking the hero or the player's
        int pauseMs = 700;      // no auto attack for this long after the player's own mouse click
        bool pickup = true;     // after the game's collect (A): take what it left, open chests and other containers
        bool autoCollect = false;   // collect (A) by itself whenever loot lies about and no fight is on
        int lootRange = 800;    // ... within this many screen pixels of the hero
        int repeatMs = 1000;    // the attack order again this often while the target lives; 0 = once per target
    };
    inline AutoAttack autoAttack;
}
