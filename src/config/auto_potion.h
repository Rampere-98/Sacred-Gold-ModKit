#pragma once

// [AutoPotion] in SacredBild.ini (read by config.cpp).
namespace Config
{
    struct AutoPotion
    {
        bool enabled = true;
        int firstPercent = 50;      // before a potion's healing is known: drink once this much health is missing
        int emergencyPercent = 25;  // always drink at or below this much health
        int intervalMs = 1500;      // at least this long between two potions
    };
    inline AutoPotion autoPotion;
}
