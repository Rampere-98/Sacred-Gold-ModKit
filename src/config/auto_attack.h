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
        int roamStep = 6;       // tiles a roaming step goes (shorter where that tile cannot be stood on)
        bool roam = false;      // nothing around: walk off a step at a time toward ground not seen yet (the agent map)
        bool quests = false;    // nothing around: off to the quest under way's target (main first), roaming there
        bool chests = true;     // nothing around and no quest to go to: off to the nearest chest not yet opened (150 tiles)
        bool farm = true;       // nothing around and no quest to go to: off to the zone with the most enemies of the hero's level
        bool background = true; // the agent plays on with the game's window in the background
        int pauseMs = 700;      // no auto attack for this long after the player's own mouse click
        bool pickup = true;     // the agent's own collect (never the game's A): items, chests and containers, the hand-cursor fixed things (hiding places, wells)
        bool lootInFight = true;    // loot nearer than the enemy fought is taken first, enemies about or not
        bool autoCollect = false;   // collect also with the auto attack off, whenever loot lies about and no fight is on
        int lootRange = 1600;    // ... within this many screen pixels of the hero
        int repeatMs = 200;     // the attack order again this often while the target lives (as a held click); 0 = once
    };
    inline AutoAttack autoAttack;
}
