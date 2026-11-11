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
        // The weapon in hand (slot 13, else the bow/crossbow slot 12): its weapon-info subclass. 1 one-handed,
        // 2 two-handed, 3 axe, 4 staff/spear, 5 dagger, 6 throwing, 7 bow, 10 dark elf blades, 12 crossbow,
        // 13/14 likely the dwarf's guns. 0: none or not read.
        uint8_t weaponKind = 0;
        bool ranged = false;    // throwing, bow, crossbow, gun
        bool mounted = false;   // on a horse (hero +0x1EC, slot 18)
    };

    // A weapon item's weapon-info subclass (as weaponKind above); 0 for none or not a weapon.
    uint8_t itemWeaponKind(uint32_t id);
    // Throwing, bow, crossbow, gun.
    bool rangedKind(uint8_t kind);
    // The classes whose own skills are with ranged weapons (Wood Elf: bows; Dwarf: guns and crossbows): they fight
    // from afar whenever they have one.
    inline bool prefersRanged(Class cls) { return cls == Class::WoodElf || cls == Class::Dwarf; }

    // Read now (presenting thread; the hero's stats block at +0x3A8: class +8 (1-9), level +0x56).
    Data current();
    const char* className(Class cls);

    // Once a frame: logs the character when it changes (another hero, class, a level).
    void onFrame();
    void drawMenu();
}
