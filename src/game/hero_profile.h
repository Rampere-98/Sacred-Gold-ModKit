#pragma once
#include <cstdint>

// The character the agent controls (the player's rule, 2026-11-09: "the agent always has to take into account the data
// of the character it controls"): one place every part of the agent reads it from, so nothing is fixed to one class.
namespace HeroProfile
{
    enum class Class : uint32_t
    {
        Unknown = 0,
        Seraphim = 1,
        Gladiator = 2,
        BattleMage = 3,
        DarkElf = 4,
        WoodElf = 5,
        Vampiress = 6,
        VampiressNight = 7,   // the other form
        Dwarf = 8,
        Daemon = 9,         // Underworld
    };

    struct Data
    {
        bool valid = false;
        uint32_t heroId = 0;
        Class cls = Class::Unknown;
        int level = 0;
        int health = 0, maxHealth = 0;
    };

    // Read now (presenting thread; the hero's stats block at +0x3A8: class +8 (1-9), level +0x56).
    Data current();
    const char* className(Class cls);

    // Once a frame: logs the character when it changes (another hero, class, a level).
    void onFrame();
    void drawMenu();
}
